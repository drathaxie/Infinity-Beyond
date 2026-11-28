#!/usr/bin/env python3
"""Patch an AdventureQuest Worlds Infinity Android APK to load Beyond.

The patch is deliberately offset-free so it keeps working on future releases:
Unity's Java side calls System.loadLibrary("main"), and its libmain.so exports
only JNI_OnLoad. So we move libmain.so aside to libmain_orig.so, drop our shim
in its place, and the shim forwards JNI_OnLoad. Nothing else in the APK is
touched - not the manifest, not classes.dex, not global-metadata.dat.

    python patch_apk.py                     # patch the newest APK in ../Android APK
    python patch_apk.py game.apk -o out.apk
    python patch_apk.py --install           # ...and push it to a connected device
    python patch_apk.py --check out.apk     # verify an already-patched APK

Requires: Android SDK build-tools (zipalign, apksigner), a JDK (keytool), and
the Android NDK to compile the shim. Pass --no-build to reuse the last
compiled shim in shim/out/.
"""

import argparse
import glob
import os
import platform
import re
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SHIM_SRC = os.path.join(HERE, "shim", "beyond_shim.c")
SHIM_OUT = os.path.join(HERE, "shim", "out")
KEYSTORE = os.path.join(HERE, "beyond.keystore")
KEY_PASS = "beyond"
KEY_ALIAS = "beyond"

# ABI -> NDK clang target triple. Only the first two ship in the game's APK; an
# ABI in the APK that is not listed here is left unpatched (and logged), which
# is the safe default if Artix ever adds another.
TRIPLES = {
    "arm64-v8a": "aarch64-linux-android",
    "armeabi-v7a": "armv7a-linux-androideabi",
    "x86_64": "x86_64-linux-android",
}

# v1 signature files. Re-signing requires the old ones gone, or apksigner
# refuses the APK as already signed with a key we do not hold.
SIG_RE = re.compile(r"^META-INF/[^/]+\.(SF|RSA|DSA|EC)$|^META-INF/MANIFEST\.MF$", re.I)


def die(msg):
    sys.exit("error: " + msg)


def run(cmd, **kw):
    """Run a tool, echoing the command; raises on non-zero exit."""
    print("  $ " + " ".join(str(c) for c in cmd))
    subprocess.run(cmd, check=True, **kw)


# --------------------------------------------------------------------------
# Toolchain discovery
# --------------------------------------------------------------------------

def find_sdk():
    for var in ("ANDROID_SDK_ROOT", "ANDROID_HOME"):
        p = os.environ.get(var)
        if p and os.path.isdir(p):
            return p
    home = os.path.expanduser("~")
    for p in (
        os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk"),
        os.path.join(home, "Library", "Android", "sdk"),
        os.path.join(home, "Android", "Sdk"),
    ):
        if os.path.isdir(p):
            return p
    die("Android SDK not found. Set ANDROID_SDK_ROOT.")


def newest_dir(parent):
    """Highest version-numbered subdirectory, e.g. build-tools/37.0.0."""
    if not os.path.isdir(parent):
        return None

    def key(name):
        return [int(x) for x in re.findall(r"\d+", name)] or [0]

    dirs = [d for d in os.listdir(parent) if os.path.isdir(os.path.join(parent, d))]
    return os.path.join(parent, max(dirs, key=key)) if dirs else None


def find_build_tools(sdk):
    bt = newest_dir(os.path.join(sdk, "build-tools"))
    if not bt:
        die("no build-tools in %s - install them via sdkmanager" % sdk)
    return bt


def tool(build_tools, name):
    """build-tools binaries are .exe or .bat on Windows, bare elsewhere."""
    for ext in (".exe", ".bat", ""):
        p = os.path.join(build_tools, name + ext)
        if os.path.isfile(p):
            return p
    die("%s not found in %s" % (name, build_tools))


def find_ndk(sdk):
    for var in ("ANDROID_NDK_ROOT", "ANDROID_NDK_HOME", "NDK_ROOT"):
        p = os.environ.get(var)
        if p and os.path.isdir(p):
            return p
    return newest_dir(os.path.join(sdk, "ndk"))


def find_keytool():
    p = shutil.which("keytool")
    if p:
        return p
    roots = [os.environ.get("JAVA_HOME", "")]
    roots += glob.glob(r"C:\Program Files\Java\*") + glob.glob("/usr/lib/jvm/*")
    roots += glob.glob("/Library/Java/JavaVirtualMachines/*/Contents/Home")
    for r in roots:
        if not r:
            continue
        for ext in (".exe", ""):
            cand = os.path.join(r, "bin", "keytool" + ext)
            if os.path.isfile(cand):
                return cand
    die("keytool not found. Install a JDK or set JAVA_HOME.")


# --------------------------------------------------------------------------
# Shim
# --------------------------------------------------------------------------

def host_tag():
    m = {"Windows": "windows-x86_64", "Darwin": "darwin-x86_64", "Linux": "linux-x86_64"}
    return m.get(platform.system(), "linux-x86_64")


def clang_for(ndk, triple, min_api):
    """Lowest API-level clang wrapper the NDK ships at or above min_api.

    The NDK names one wrapper per API level and drops old ones over time, so
    hard-coding a level is a build break waiting to happen. Glob instead.
    """
    bindir = os.path.join(ndk, "toolchains", "llvm", "prebuilt", host_tag(), "bin")
    ext = ".cmd" if platform.system() == "Windows" else ""
    found = []
    for p in glob.glob(os.path.join(bindir, triple + "*-clang" + ext)):
        m = re.search(re.escape(triple) + r"(\d+)-clang", os.path.basename(p))
        if m and int(m.group(1)) >= min_api:
            found.append((int(m.group(1)), p))
    if not found:
        die("no clang wrapper for %s (api>=%d) in %s" % (triple, min_api, bindir))
    return min(found)[1]


def build_shim(ndk, abis, min_api):
    os.makedirs(SHIM_OUT, exist_ok=True)
    built = {}
    for abi in abis:
        triple = TRIPLES.get(abi)
        if not triple:
            print("  ! no toolchain mapping for ABI %s - leaving it unpatched" % abi)
            continue
        out_dir = os.path.join(SHIM_OUT, abi)
        os.makedirs(out_dir, exist_ok=True)
        out = os.path.join(out_dir, "libmain.so")
        run([clang_for(ndk, triple, min_api), "-shared", "-fPIC", "-O2",
             "-Wl,-soname,libmain.so", "-o", out, SHIM_SRC, "-llog", "-ldl"])
        built[abi] = out
    if not built:
        die("no shim was built for any ABI in the APK")
    return built


# --------------------------------------------------------------------------
# APK surgery
# --------------------------------------------------------------------------

def apk_facts(apk):
    """ABIs present, and whether this APK has already been patched."""
    abis, already = set(), False
    with zipfile.ZipFile(apk) as z:
        for n in z.namelist():
            m = re.match(r"^lib/([^/]+)/libmain\.so$", n)
            if m:
                abis.add(m.group(1))
            if re.match(r"^lib/[^/]+/libmain_orig\.so$", n):
                already = True
    if not abis:
        die("%s has no lib/*/libmain.so - is this a Unity Android APK?" % apk)
    return sorted(abis), already


def min_sdk(build_tools, apk):
    out = subprocess.run([tool(build_tools, "aapt2"), "dump", "badging", apk],
                         capture_output=True, text=True).stdout
    m = re.search(r"minSdkVersion:'(\d+)'", out)
    return int(m.group(1)) if m else 21


def repack(src, dst, shims, already_patched):
    """Copy every entry through, renaming libmain.so aside and dropping in ours.

    ponytail: this decompresses and recompresses the whole ~460MB archive rather
    than copying raw entries (which would need a hand-written zip writer). It
    measures ~8s, so the simple version stays.
    """
    renamed = replaced = 0
    with zipfile.ZipFile(src) as zin, \
            zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            if SIG_RE.match(item.filename):
                continue  # old signature; we re-sign below
            m = re.match(r"^lib/([^/]+)/libmain\.so$", item.filename)
            if m and m.group(1) in shims:
                if not already_patched:
                    # Stash Unity's real loader under the name the shim dlopens.
                    orig = zipfile.ZipInfo("lib/%s/libmain_orig.so" % m.group(1),
                                           date_time=item.date_time)
                    orig.compress_type = item.compress_type
                    orig.external_attr = item.external_attr
                    zout.writestr(orig, zin.read(item))
                    renamed += 1
                # else: the input already carries libmain_orig.so and this entry
                # is a previous shim, so just overwrite it with the fresh one.
                shim = zipfile.ZipInfo(item.filename, date_time=item.date_time)
                shim.compress_type = item.compress_type
                shim.external_attr = item.external_attr
                with open(shims[m.group(1)], "rb") as f:
                    zout.writestr(shim, f.read())
                replaced += 1
                continue
            zout.writestr(item, zin.read(item))
    print("  stashed %d original libmain.so, installed %d shim(s)" % (renamed, replaced))


def ensure_keystore(keytool_path):
    if os.path.isfile(KEYSTORE):
        return
    print("Creating signing key %s (password: %s)" % (KEYSTORE, KEY_PASS))
    run([keytool_path, "-genkeypair", "-keystore", KEYSTORE, "-alias", KEY_ALIAS,
         "-storepass", KEY_PASS, "-keypass", KEY_PASS, "-keyalg", "RSA",
         "-keysize", "2048", "-validity", "10000",
         "-dname", "CN=Beyond, OU=Beyond, O=Beyond, C=US"])


def check(build_tools, apk):
    """Self-check: patched APK must be structurally sound and verifiably signed."""
    ok = True
    with zipfile.ZipFile(apk) as z:
        names = set(z.namelist())
        abis = {re.match(r"^lib/([^/]+)/", n).group(1)
                for n in names if n.startswith("lib/") and "/" in n[4:]}
        for abi in sorted(abis):
            main = "lib/%s/libmain.so" % abi
            orig = "lib/%s/libmain_orig.so" % abi
            if main not in names:
                continue
            if orig not in names:
                print("  FAIL %s: no libmain_orig.so - the shim has nothing to forward to" % abi)
                ok = False
            elif z.read(main) == z.read(orig):
                print("  FAIL %s: libmain.so still equals the original - shim not installed" % abi)
                ok = False
            else:
                print("  ok   %s: shim installed, original stashed" % abi)
    try:
        run([tool(build_tools, "apksigner"), "verify", apk])
        print("  ok   signature verifies")
    except subprocess.CalledProcessError:
        print("  FAIL signature does not verify")
        ok = False
    return ok


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("apk", nargs="?", help="input APK (default: newest in ../Android APK)")
    ap.add_argument("-o", "--out", help="output APK (default: <input>-beyond.apk)")
    ap.add_argument("--no-build", action="store_true", help="reuse the shim in shim/out/")
    ap.add_argument("--install", action="store_true", help="adb install the result")
    ap.add_argument("--check", metavar="APK", help="verify an already-patched APK and exit")
    args = ap.parse_args()

    sdk = find_sdk()
    build_tools = find_build_tools(sdk)
    print("SDK         : %s" % sdk)
    print("build-tools : %s" % build_tools)

    if args.check:
        sys.exit(0 if check(build_tools, args.check) else 1)

    apk = args.apk
    if not apk:
        found = sorted(glob.glob(os.path.join(HERE, "..", "Android APK", "*.apk")),
                       key=os.path.getmtime)
        found = [f for f in found if not f.endswith("-beyond.apk")]
        if not found:
            die("no APK given and none found in ../Android APK")
        apk = found[-1]
    apk = os.path.abspath(apk)
    if not os.path.isfile(apk):
        die("no such APK: " + apk)
    out = os.path.abspath(args.out or re.sub(r"\.apk$", "", apk) + "-beyond.apk")

    abis, already = apk_facts(apk)
    api = min_sdk(build_tools, apk)
    print("input       : %s" % apk)
    print("ABIs        : %s (minSdk %d)%s"
          % (", ".join(abis), api, "  [already patched - refreshing shim]" if already else ""))

    if args.no_build:
        shims = {a: os.path.join(SHIM_OUT, a, "libmain.so") for a in abis}
        shims = {a: p for a, p in shims.items() if os.path.isfile(p)}
        if not shims:
            die("--no-build given but no compiled shim in %s" % SHIM_OUT)
    else:
        ndk = find_ndk(sdk)
        if not ndk:
            die('Android NDK not found. Install it (sdkmanager "ndk;28.0.13004108"),\n'
                "       set ANDROID_NDK_ROOT, or pass --no-build to reuse shim/out/.")
        print("NDK         : %s" % ndk)
        print("Building shim...")
        shims = build_shim(ndk, abis, api)

    tmp = out + ".unaligned"
    print("Repacking...")
    repack(apk, tmp, shims, already)

    print("Aligning...")
    run([tool(build_tools, "zipalign"), "-p", "-f", "4", tmp, out])
    os.remove(tmp)

    print("Signing...")
    ensure_keystore(find_keytool())
    run([tool(build_tools, "apksigner"), "sign", "--ks", KEYSTORE,
         "--ks-pass", "pass:" + KEY_PASS, "--key-pass", "pass:" + KEY_PASS, out])
    idsig = out + ".idsig"
    if os.path.isfile(idsig):
        os.remove(idsig)

    print("Verifying...")
    if not check(build_tools, out):
        die("patched APK failed verification: " + out)
    print("\nPatched APK: %s" % out)

    if args.install:
        adb = os.path.join(sdk, "platform-tools", "adb")
        print("Installing...")
        try:
            run([adb, "install", "-r", out])
        except subprocess.CalledProcessError:
            # Re-signing changes the signature, so an existing install blocks
            # this. Uninstalling wipes the app's local data, so that is the
            # user's call to make, not ours.
            print("\nInstall failed. If the stock game is installed, its signature differs.\n"
                  "Uninstall it first (this deletes the app's local data):\n"
                  "  adb uninstall com.Artix.aq2d")
            sys.exit(1)
        print("Watch the loader with:  adb logcat -s Beyond")


if __name__ == "__main__":
    main()
