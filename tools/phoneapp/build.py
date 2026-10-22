#!/usr/bin/env python3
"""Build the ArgonOS phone app (Android) without Gradle.

    python tools/phoneapp/build.py            -> build/phoneapp/ArgonOS.apk
    python tools/phoneapp/build.py --install  -> and adb install -r it

Why no Gradle: the app is one Java file and a few pages, it uses nothing
outside android.jar, and a Gradle build wants the Android Gradle plugin and a
Maven repository before it will compile a line.  The SDK's own tools - aapt2,
javac, d8, zipalign, apksigner - are all it takes, and they are already on this
machine with Unity's Android support.

Where the tools come from, first match wins:
  ANDROID_HOME / ANDROID_SDK_ROOT, else Unity's AndroidPlayer/SDK;
  JAVA_HOME if it has javac, else Unity's AndroidPlayer/OpenJDK.

The page is not copied into this directory by hand: term.html is
apps/phone/web/index.html - the page every board serves - with the Bluetooth
shim put in front of its scripts, made fresh on every build.  One page, two
ways to reach it.

The APK is signed with a debug key kept in ~/.android/argon-debug.keystore
(made on the first build).  Keep that file: Android refuses to update an app
signed with a different key, and a new key means uninstalling first.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "build", "phoneapp")
PAGE = os.path.join(ROOT, "apps", "phone", "web", "index.html")

MIN_SDK = 24
TARGET_SDK = 34
KEYSTORE = os.path.join(os.path.expanduser("~"), ".android", "argon-debug.keystore")
KS_PASS = "android"  # a debug key, as Android's own debug.keystore is
ALIAS = "argon"


def die(msg):
    print("phoneapp: " + msg, file=sys.stderr)
    sys.exit(1)


def unity_player_dirs():
    roots = [r"C:\Program Files\Unity\Hub\Editor", r"D:\Program Files\Unity\Hub\Editor",
             r"C:\Program Files\Unity\Editor"]
    found = []
    for r in roots:
        for d in sorted(glob.glob(os.path.join(r, "*")), reverse=True):
            p = os.path.join(d, "Editor", "Data", "PlaybackEngines", "AndroidPlayer")
            if os.path.isdir(p):
                found.append(p)
    return found


def find_sdk():
    for v in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        p = os.environ.get(v)
        if p and os.path.isdir(os.path.join(p, "build-tools")):
            return p
    for p in unity_player_dirs():
        sdk = os.path.join(p, "SDK")
        if os.path.isdir(os.path.join(sdk, "build-tools")):
            return sdk
    die("no Android SDK: set ANDROID_HOME, or install Unity's Android support")


def find_jdk():
    exe = ".exe" if os.name == "nt" else ""
    p = os.environ.get("JAVA_HOME")
    if p and os.path.isfile(os.path.join(p, "bin", "javac" + exe)):
        return p
    for d in unity_player_dirs():
        jdk = os.path.join(d, "OpenJDK")
        if os.path.isfile(os.path.join(jdk, "bin", "javac" + exe)):
            return jdk
    die("no JDK with javac: set JAVA_HOME")


def newest(dirs):
    def key(name):
        parts = []
        for x in os.path.basename(name).replace("android-", "").split("."):
            parts.append(int(x) if x.isdigit() else 0)
        return parts
    return sorted(dirs, key=key)[-1] if dirs else None


def run(cmd, env=None):
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        print(" ".join('"%s"' % c if " " in c else c for c in cmd), file=sys.stderr)
        sys.stderr.write(r.stdout + r.stderr)
        die("failed: " + os.path.basename(cmd[0]))
    noise = [l for l in (r.stdout + r.stderr).splitlines()
             if l.strip() and "bootstrap class path" not in l
             and "source value 8 is obsolete" not in l
             and "target value 8 is obsolete" not in l
             and "To suppress warnings about obsolete" not in l
             and not l.startswith("Note:") and "warning: [options]" not in l
             and not l.strip().startswith("3 warnings")]
    for l in noise:
        print("  " + l)


def make_assets(dst):
    shutil.copytree(os.path.join(HERE, "assets"), dst)
    with open(PAGE, "rb") as f:
        page = f.read()
    # Right after the charset line, so the shim runs before any of the page's
    # own scripts and its WebSocket is the one they find.
    tag = b'<script src="shim.js"></script>\n'
    nl = page.find(b"\n")
    if not page.startswith(b"<meta charset") or nl < 0:
        die("index.html no longer starts with its charset line; put the shim tag somewhere else")
    with open(os.path.join(dst, "term.html"), "wb") as f:
        f.write(page[:nl + 1] + tag + page[nl + 1:])


def ensure_key(jdk):
    if os.path.isfile(KEYSTORE):
        return
    os.makedirs(os.path.dirname(KEYSTORE), exist_ok=True)
    print("phoneapp: making the debug key " + KEYSTORE)
    run([os.path.join(jdk, "bin", "keytool"), "-genkeypair", "-keystore", KEYSTORE,
         "-storepass", KS_PASS, "-keypass", KS_PASS, "-alias", ALIAS,
         "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
         "-dname", "CN=ArgonOS debug"])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--install", action="store_true", help="adb install -r the result")
    args = ap.parse_args()

    sdk = find_sdk()
    jdk = find_jdk()
    bt = newest(glob.glob(os.path.join(sdk, "build-tools", "*")))
    plat = newest([p for p in glob.glob(os.path.join(sdk, "platforms", "android-*"))
                   if os.path.isfile(os.path.join(p, "android.jar"))])
    if not bt or not plat:
        die("the SDK has no build-tools or no platform")
    jar = os.path.join(plat, "android.jar")
    exe = ".exe" if os.name == "nt" else ""
    bat = ".bat" if os.name == "nt" else ""
    env = dict(os.environ, JAVA_HOME=jdk,
               PATH=os.path.join(jdk, "bin") + os.pathsep + os.environ.get("PATH", ""))
    print("phoneapp: SDK %s, %s, %s" % (sdk, os.path.basename(bt), os.path.basename(plat)))

    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    assets = os.path.join(OUT, "assets")
    make_assets(assets)

    base = os.path.join(OUT, "base.apk")
    run([os.path.join(bt, "aapt2" + exe), "link", "-o", base,
         "--manifest", os.path.join(HERE, "AndroidManifest.xml"), "-I", jar,
         "-A", assets, "--min-sdk-version", str(MIN_SDK),
         "--target-sdk-version", str(TARGET_SDK)], env)

    classes = os.path.join(OUT, "classes")
    os.makedirs(classes)
    srcs = glob.glob(os.path.join(HERE, "src", "**", "*.java"), recursive=True)
    run([os.path.join(jdk, "bin", "javac" + exe), "-encoding", "UTF-8",
         "-source", "8", "-target", "8", "-bootclasspath", jar, "-classpath", jar,
         "-Xlint:deprecation", "-d", classes] + srcs, env)

    dex = os.path.join(OUT, "dex")
    os.makedirs(dex)
    cls = glob.glob(os.path.join(classes, "**", "*.class"), recursive=True)
    run([os.path.join(bt, "d8" + bat), "--release", "--min-api", str(MIN_SDK),
         "--lib", jar, "--output", dex] + cls, env)

    with zipfile.ZipFile(base, "a", zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(dex, "classes.dex"), "classes.dex")

    aligned = os.path.join(OUT, "aligned.apk")
    run([os.path.join(bt, "zipalign" + exe), "-f", "-p", "4", base, aligned], env)

    ensure_key(jdk)
    apk = os.path.join(OUT, "ArgonOS.apk")
    run([os.path.join(bt, "apksigner" + bat), "sign", "--ks", KEYSTORE,
         "--ks-pass", "pass:" + KS_PASS, "--key-pass", "pass:" + KS_PASS,
         "--ks-key-alias", ALIAS, "--out", apk, aligned], env)
    run([os.path.join(bt, "apksigner" + bat), "verify", apk], env)
    print("phoneapp: %s (%d KB)" % (os.path.relpath(apk, ROOT), os.path.getsize(apk) // 1024))

    if args.install:
        adb = os.path.join(sdk, "platform-tools", "adb" + exe)
        run([adb, "install", "-r", apk], env)
        print("phoneapp: installed")


if __name__ == "__main__":
    main()
