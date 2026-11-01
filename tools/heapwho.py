#!/usr/bin/env python3
"""How the holes in a board's heap were made.

Reads `mem who` from a CONFIG_ARGON_HEAP_WHO build: every block still held,
with its age (order of allocation) and the calls that took it, and the last
blocks given back.  Lays them out in address order and, for every hole big
enough to matter, says who walls it in and who lived in it before.

    python tools/heapwho.py w1.txt --elf build/argonos.elf [--min 512]

Blocks taken before the record started (FreeRTOS's first few) are not in it;
their space shows as a hole with nobody in it - compare with `mem /i`.
"""
import argparse
import bisect
import os
import re
import shutil
import subprocess
import sys

# Heap regions of the CYD's ESP32 build (heap_init at boot).  Holes are
# measured inside a region only.
REGIONS = [
    (0x3FFAE6E0, 0x1920, 'DRAM'),
    (0x3FFC3AA0, 0x1C560, 'DRAM'),
    (0x3FFE0440, 0x3AE0, 'D/IRAM'),
    (0x3FFE4350, 0x1BCB0, 'D/IRAM'),
    (0x40096F20, 0x90E0, 'IRAM'),
]
HEADER = 4          # TLSF's word in front of each block

# Frames that are the allocator, not its user.
ALLOC_FUNCS = re.compile(
    r'^(heap_caps_\w+|multi_heap_\w+|malloc|calloc|realloc|free|_malloc_r|'
    r'_calloc_r|_realloc_r|_free_r|pvPortMalloc|vPortFree|\w*_malloc\w*|'
    r'\w*_calloc\w*|\w*_zalloc\w*|ag_port_alloc\w*|ag_port_calloc\w*|'
    r'esp_coex_common_malloc\w*|wifi_malloc|wifi_calloc|wifi_zalloc\w*|'
    r'mem_malloc|mem_calloc|memp_malloc\w*|esf_buf_alloc\w*|xTaskCreate\w*|'
    r'prvCreate\w*|xQueueGenericCreate\w*|xQueueCreate\w*)$')


def task_name(word):
    w = int(word, 16)
    s = ''.join(chr((w >> (8 * i)) & 0xFF) for i in range(4))
    return s.rstrip('\0').replace('\0', '') or '?'


def parse(path):
    live, gone, marks, seq = [], [], [], 0
    for line in open(path, encoding='utf-8', errors='replace'):
        f = line.split()
        if not f:
            continue
        if f[0] == 'S':
            seq = int(f[1], 16)
        elif f[0] == 'M':
            marks.append((int(f[1], 16), task_name(f[2])))
        elif f[0] in ('L', 'G') and len(f) >= 6:
            r = dict(ptr=int(f[1], 16), size=int(f[2], 16), seq=int(f[3], 16),
                     gone=int(f[4], 16), task=task_name(f[5]),
                     pcs=[int(x, 16) for x in f[6:] if int(x, 16)])
            (live if f[0] == 'L' else gone).append(r)
    return live, gone, marks, seq


def addr2line():
    for root in (r'D:\Espressif\tools\tools', r'D:\Espressif\tools'):
        for dp, _, files in os.walk(root):
            if 'xtensa-esp32-elf-addr2line.exe' in files:
                return os.path.join(dp, 'xtensa-esp32-elf-addr2line.exe')
    return shutil.which('xtensa-esp32-elf-addr2line')


def symbolize(elf, pcs):
    tool = addr2line()
    pcs = sorted(set(pcs))
    if not tool or not pcs:
        return {}
    out = subprocess.run([tool, '-f', '-C', '-e', elf] + ['%x' % (p - 3) for p in pcs],
                         capture_output=True, text=True).stdout.splitlines()
    names = {}
    for i, p in enumerate(pcs):
        fn = out[2 * i] if 2 * i < len(out) else '?'
        loc = out[2 * i + 1] if 2 * i + 1 < len(out) else ''
        loc = re.sub(r'.*[\\/]', '', loc)
        names[p] = (fn, loc)
    return names


def owner(r, names, depth=2):
    """The first frames that are not the allocator itself."""
    got = []
    for p in r['pcs']:
        fn, loc = names.get(p, ('%08x' % p, ''))
        if ALLOC_FUNCS.match(fn) and not got:
            continue
        got.append('%s (%s)' % (fn, loc) if loc and not loc.startswith('??') else fn)
        if len(got) == depth:
            break
    if not got and r['pcs']:
        fn, _ = names.get(r['pcs'][-1], ('%08x' % r['pcs'][-1], ''))
        got.append(fn)
    return ' <- '.join(got) or '?'


def phase(seq, marks):
    """Which step of the session a moment belongs to."""
    name = 'boot'
    for s, n in marks:
        if seq > s:
            name = n
    return name


def region_of(addr):
    for base, size, kind in REGIONS:
        if base <= addr < base + size:
            return base, size, kind
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('who')
    ap.add_argument('--elf', required=True)
    ap.add_argument('--min', type=int, default=512, help='smallest hole shown')
    a = ap.parse_args()

    live, gone, marks, seq = parse(a.who)
    names = symbolize(a.elf, [p for r in live + gone for p in r['pcs']])
    print('%d blocks held, %d given back on record, %d allocations so far' %
          (len(live), len(gone), seq))
    if marks:
        print('marks: ' + ', '.join('%s@%d' % (n, s) for s, n in marks))

    live.sort(key=lambda r: r['ptr'])
    starts = [r['ptr'] for r in live]
    for base, size, kind in REGIONS:
        if kind == 'IRAM':
            continue
        end = base + size
        inside = [r for r in live if base <= r['ptr'] < end]
        print('\n== %s %08x..%08x (%d KB): %d blocks held' %
              (kind, base, end, size // 1024, len(inside)))
        prev, cursor = None, base
        for r in inside + [None]:
            nxt = r['ptr'] - HEADER if r else end
            hole = nxt - cursor
            if hole >= a.min:
                print('\n  hole %08x %6d B' % (cursor, hole))
                for side, b in (('below', prev), ('above', r)):
                    if b is None:
                        print('    %s: edge of the region' % side)
                        continue
                    print('    %s: %5d B  age %-5d %-5s %s  [%s]' %
                          (side, b['size'], b['seq'], phase(b['seq'], marks),
                           owner(b, names), b['task']))
                tenants = [g for g in gone
                           if g['ptr'] < nxt and g['ptr'] + g['size'] > cursor]
                tenants.sort(key=lambda g: g['gone'])
                for g in tenants[-6:]:
                    print('    was: %5d B  age %-5d gone %-5d (%s->%s) %s  [%s]' %
                          (g['size'], g['seq'], g['gone'], phase(g['seq'], marks),
                           phase(g['gone'], marks), owner(g, names), g['task']))
                if not tenants:
                    print('    was: nobody on record')
            if r:
                cursor = r['ptr'] + r['size']
                cursor = (cursor + 3) & ~3
                prev = r

    # The small and the late: blocks that sit alone between two holes.
    print('\n== held blocks under 1 KB taken after boot, by owner')
    boot_end = marks[0][0] if marks else seq
    late = {}
    for r in live:
        if r['size'] < 1024 and r['seq'] > boot_end:
            k = owner(r, names)
            late.setdefault(k, []).append(r)
    for k, rs in sorted(late.items(), key=lambda kv: -sum(r['size'] for r in kv[1])):
        print('  %3d blocks %6d B  %s' % (len(rs), sum(r['size'] for r in rs), k))


if __name__ == '__main__':
    main()
