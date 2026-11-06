#!/usr/bin/env bash
# Build-and-install helper for this fork on macOS.
#   ./deploy_mac_local.sh            # rebuild (make), refresh the bundled app in build/, install to /Applications
#   ./deploy_mac_local.sh --no-build # skip make, just redeploy the existing sioyek.app
#
# Assumes: Homebrew qt (with QtWebEngine) at /opt/homebrew/opt/qt, mupdf already built in mupdf/build/release,
# and qmake already run once (so a Makefile exists). build/sioyek.app is the macdeployqt-bundled copy; we only
# swap the main executable and data files into it, then fix library paths so the WebEngine helper process
# (which lives deep inside QtWebEngineCore.framework) can find the bundled frameworks via @rpath.
set -euo pipefail
cd "$(dirname "$0")"
export PATH=/opt/homebrew/opt/qt/bin:$PATH

if [[ "${1:-}" != "--no-build" ]]; then
    make -j8
fi

for f in shaders prefs.config prefs_user.config keys.config keys_user.config; do
    rm -rf "sioyek.app/Contents/MacOS/$f"; cp -r "pdf_viewer/$f" "sioyek.app/Contents/MacOS/$f"
done
cp tutorial.pdf sioyek.app/Contents/MacOS/tutorial.pdf

if [[ ! -d build/sioyek.app/Contents/Frameworks/QtWebEngineCore.framework ]]; then
    echo "build/sioyek.app has no bundled Qt yet; running macdeployqt (slow, several minutes)"
    rm -rf build && mkdir build && cp -R sioyek.app build/
    macdeployqt build/sioyek.app
else
    cp sioyek.app/Contents/MacOS/sioyek build/sioyek.app/Contents/MacOS/sioyek
    for f in shaders prefs.config prefs_user.config keys.config keys_user.config tutorial.pdf; do
        rm -rf "build/sioyek.app/Contents/MacOS/$f"; cp -r "sioyek.app/Contents/MacOS/$f" "build/sioyek.app/Contents/MacOS/$f"
    done
fi

# Rewrite every Homebrew / @executable_path library reference to @rpath (macdeployqt leaves the
# QtWebEngineProcess helper pointing at /opt/homebrew, which crashes it on launch).
python3 -I - build/sioyek.app <<'PY'
import os, re, subprocess, sys
app = sys.argv[1]
fw_dir = os.path.join(app, "Contents/Frameworks")
have = set(os.listdir(fw_dir))
def machos():
    for root, _, files in os.walk(app):
        for f in files:
            p = os.path.join(root, f)
            if os.path.islink(p): continue
            try:
                with open(p, "rb") as fh: magic = fh.read(4)
            except OSError: continue
            if magic in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe"): yield p
def rpaths(p):
    return re.findall(r"path (\S+) \(offset", subprocess.run(["otool","-l",p],capture_output=True,text=True).stdout)
for p in machos():
    lines = subprocess.run(["otool", "-L", p], capture_output=True, text=True).stdout.splitlines()[1:]
    deps = [m.group(1) for m in (re.match(r"^\t(\S+) \(", l) for l in lines) if m]
    is_dylib = subprocess.run(["otool","-D",p],capture_output=True,text=True).stdout.strip().count("\n") >= 1
    cmds = []
    for i, dep in enumerate(deps):
        new = None
        m2 = re.search(r"/((Qt\w+)\.framework/Versions/A/\2)$", dep)
        if dep.startswith("/opt/homebrew/"):
            if m2 and m2.group(2) + ".framework" in have: new = "@rpath/" + m2.group(1)
            elif os.path.basename(dep) in have: new = "@rpath/" + os.path.basename(dep)
            else: print("WARNING: dependency not bundled:", dep, "<-", p)
        elif dep.startswith("@executable_path/../Frameworks/"):
            new = "@rpath/" + dep[len("@executable_path/../Frameworks/"):]
        if new and new != dep:
            if i == 0 and is_dylib: cmds += ["-id", new]
            else: cmds += ["-change", dep, new]
    for rp in rpaths(p):
        if rp.startswith("/opt/homebrew"): cmds += ["-delete_rpath", rp]
    if cmds:
        subprocess.run(["install_name_tool"] + cmds + [p], check=True, capture_output=True)
main = os.path.join(app, "Contents/MacOS/sioyek")
if "@executable_path/../Frameworks" not in rpaths(main):
    subprocess.run(["install_name_tool", "-add_rpath", "@executable_path/../Frameworks", main], check=True)
print("library paths fixed")
PY

# stray dirs created by a running helper break strict signature checks
find build/sioyek.app/Contents/Frameworks -type d -empty -delete
codesign --force --deep -s - build/sioyek.app
codesign --verify --deep --strict build/sioyek.app && echo "signature ok"

if [[ "${INSTALL:-1}" == "1" ]]; then
    osascript -e 'quit app "sioyek"' >/dev/null 2>&1 || true
    for _ in 1 2 3 4 5; do pgrep -x sioyek >/dev/null || break; sleep 1; done
    pgrep -x sioyek >/dev/null && pkill -x sioyek && sleep 1
    rm -rf /Applications/sioyek.app
    cp -R build/sioyek.app /Applications/sioyek.app
    echo "installed to /Applications/sioyek.app"
fi
