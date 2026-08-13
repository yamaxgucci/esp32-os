#!/usr/bin/env python3
"""Run kbdvirt + mousevirt + pcmplay as one host process.

Guest drivers listen on QEMU hostfwd ports; these tools are TCP *clients*
with --reconnect, so they can start before the guest loads the .SYS files.

Typical:

  argon run -Gfx -Sd -Virt          # spawned for you, killed with QEMU
  python tools/virt.py              # one extra window if QEMU is already up
  python tools/virt.py --midi       # also midikbd.py (:5559)
  python tools/virt.py --no-kbd --no-mouse   # pcmplay only

Right-Ctrl: kbdvirt starts paused (toggle); mousevirt pauses while held.
Ctrl+C here stops every helper.
"""

from __future__ import annotations

import argparse
import ctypes
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

CREATE_NO_WINDOW = 0x08000000
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
JobObjectExtendedLimitInformation = 9

TOOLS = Path(__file__).resolve().parent

HELPERS: tuple[tuple[str, str, bool], ...] = (
    ("pcm", "pcmplay.py", True),
    ("kbd", "kbdvirt.py", True),
    ("mouse", "mousevirt.py", True),
    ("midi", "midikbd.py", False),
)


class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("PerProcessUserTimeLimit", ctypes.c_int64),
        ("PerJobUserTimeLimit", ctypes.c_int64),
        ("LimitFlags", ctypes.c_uint32),
        ("MinimumWorkingSetSize", ctypes.c_size_t),
        ("MaximumWorkingSetSize", ctypes.c_size_t),
        ("ActiveProcessLimit", ctypes.c_uint32),
        ("Affinity", ctypes.c_size_t),
        ("PriorityClass", ctypes.c_uint32),
        ("SchedulingClass", ctypes.c_uint32),
    ]


class IO_COUNTERS(ctypes.Structure):
    _fields_ = [
        ("ReadOperationCount", ctypes.c_uint64),
        ("WriteOperationCount", ctypes.c_uint64),
        ("OtherOperationCount", ctypes.c_uint64),
        ("ReadTransferCount", ctypes.c_uint64),
        ("WriteTransferCount", ctypes.c_uint64),
        ("OtherTransferCount", ctypes.c_uint64),
    ]


class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION),
        ("IoInfo", IO_COUNTERS),
        ("ProcessMemoryLimit", ctypes.c_size_t),
        ("JobMemoryLimit", ctypes.c_size_t),
        ("PeakProcessMemoryUsed", ctypes.c_size_t),
        ("PeakJobMemoryUsed", ctypes.c_size_t),
    ]


def _win_job():
    """Job that kills children when this process dies (best-effort)."""
    if os.name != "nt":
        return None
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateJobObjectW.restype = ctypes.c_void_p
    handle = k32.CreateJobObjectW(None, None)
    if not handle:
        return None
    info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    ok = k32.SetInformationJobObject(
        ctypes.c_void_p(handle),
        JobObjectExtendedLimitInformation,
        ctypes.byref(info),
        ctypes.sizeof(info),
    )
    if not ok:
        k32.CloseHandle(ctypes.c_void_p(handle))
        return None
    return handle


def _assign_job(job, pid: int) -> None:
    if not job or os.name != "nt":
        return
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = ctypes.c_void_p
    proc = k32.OpenProcess(0x1F0FFF, False, pid)  # PROCESS_ALL_ACCESS
    if not proc:
        return
    try:
        k32.AssignProcessToJobObject(ctypes.c_void_p(job), ctypes.c_void_p(proc))
    finally:
        k32.CloseHandle(ctypes.c_void_p(proc))


def _pump(prefix: str, stream) -> None:
    try:
        for line in iter(stream.readline, ""):
            sys.stdout.write(f"{prefix}{line}")
            sys.stdout.flush()
    except OSError:
        pass
    try:
        stream.close()
    except OSError:
        pass


def _spawn(script: Path, extra: list[str], job) -> subprocess.Popen:
    cmd = [sys.executable, "-u", str(script), *extra]
    flags = 0
    if os.name == "nt":
        flags = CREATE_NO_WINDOW
    env = os.environ.copy()
    env["PYTHONUNBUFFERED"] = "1"
    proc = subprocess.Popen(
        cmd,
        cwd=str(TOOLS.parent),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        env=env,
        creationflags=flags,
    )
    _assign_job(job, proc.pid)
    return proc


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        "--reconnect",
        dest="reconnect",
        action="store_true",
        default=True,
        help="survive guest restart (default)",
    )
    ap.add_argument("--no-reconnect", dest="reconnect", action="store_false")
    for name, _script, default in HELPERS:
        flag = f"--{name}"
        no = f"--no-{name}"
        ap.add_argument(flag, dest=name, action="store_true", default=default)
        ap.add_argument(no, dest=name, action="store_false")
    args = ap.parse_args()

    wanted: list[tuple[str, Path]] = []
    for name, script, _default in HELPERS:
        if getattr(args, name):
            path = TOOLS / script
            if not path.is_file():
                print(f"virt: missing {path}", file=sys.stderr)
                return 1
            wanted.append((name, path))
    if not wanted:
        print("virt: nothing to start (all helpers disabled)", file=sys.stderr)
        return 2

    extra = ["--reconnect"] if args.reconnect else []
    job = _win_job()
    children: list[tuple[str, subprocess.Popen]] = []

    def stop_all() -> None:
        for _name, proc in children:
            if proc.poll() is None:
                try:
                    proc.terminate()
                except OSError:
                    pass
        deadline = time.time() + 2.0
        for _name, proc in children:
            while proc.poll() is None and time.time() < deadline:
                time.sleep(0.05)
            if proc.poll() is None:
                try:
                    proc.kill()
                except OSError:
                    pass

    names = ", ".join(n for n, _p in wanted)
    print(
        f"virt: {names}  (--reconnect={'on' if args.reconnect else 'off'}; "
        f"RCtrl arms kbd / pauses mouse; Ctrl+C stops all)",
        flush=True,
    )

    try:
        for name, path in wanted:
            proc = _spawn(path, extra, job)
            children.append((name, proc))
            t = threading.Thread(
                target=_pump,
                args=(f"[{name}] ", proc.stdout),
                name=f"virt-{name}-log",
                daemon=True,
            )
            t.start()

        # Stay alive while any helper runs.  A helper that exits immediately
        # (missing sounddevice, etc.) should not take the others down.
        while any(p.poll() is None for _n, p in children):
            time.sleep(0.25)
        rc = 0
        for name, proc in children:
            code = proc.poll()
            if code not in (0, None):
                print(f"virt: {name} exited {code}", file=sys.stderr)
                rc = code if rc == 0 else rc
        return rc
    except KeyboardInterrupt:
        print("\nvirt: stopping", flush=True)
        return 0
    finally:
        stop_all()


if __name__ == "__main__":
    raise SystemExit(main())
