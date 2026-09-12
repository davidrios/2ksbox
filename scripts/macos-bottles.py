#!/usr/bin/env python3
"""Put the floor's Homebrew bottles in a staged 2ksbox.app.

    scripts/macos-bottles.py <app>/Contents <floor> <bottle tag> <cache dir>
    scripts/macos-bottles.py build/macos/2ksbox.app/Contents 14.0 arm64_sonoma build/macos-bottles

Homebrew pours the bottle built for the macOS it runs on, so every library
`package-macos.sh` copied out of it on a macOS 26 Mac is a macOS 26 build,
and the app would refuse to start anywhere older. Homebrew also publishes
the same version built on each older macOS it supports (the tag names
it: `arm64_sonoma` is macOS 14), and those are what this puts in its
place -- only for the files that need it, since a bottle whose own build
system chose a lower target (Qt's base, glib) is already fine.

A staged file is matched to the Homebrew file it was copied from by its
LC_UUID, which the install-name rewrites and ad-hoc signatures of the
staging do not touch. The older build of that same file is then fetched
from Homebrew's registry (ghcr.io, cached under <cache dir>), copied over
it, and given the staged file's install name, dependencies and rpaths --
what the staging had done to the file it replaces, carried across. It
does not sign: the packager's own pass does that next.

Exits non-zero when a bottle for the tag does not exist, when the
installed version is not the one Homebrew has bottles of (brew upgrade),
or when the older build links something the newer one did not. Files
above the floor that Homebrew did not make are reported and left to the
packager's floor check, which fails on them.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request

DEP_CMDS = {"LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB",
            "LC_LOAD_UPWARD_DYLIB", "LC_LAZY_LOAD_DYLIB"}
MACHO_MAGIC = {b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe",
               b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}
FIELD = re.compile(r"^(name|path) (.*) \(offset \d+\)$")


def die(msg):
    print("macos-bottles.py: " + msg, file=sys.stderr)
    sys.exit(1)


def out(*args):
    return subprocess.check_output(args, text=True)


def vt(v):
    return tuple(int(x) for x in v.split("."))


def is_macho(path):
    if os.path.islink(path) or not os.path.isfile(path):
        return False
    with open(path, "rb") as f:
        return f.read(4) in MACHO_MAGIC


def load_commands(path):
    """The install name, dependencies, rpaths, UUID and minos of a Mach-O."""
    lc = {"id": None, "deps": [], "rpaths": [], "uuid": None, "minos": None}
    cmd = None
    for line in out("otool", "-l", path).splitlines():
        s = line.strip()
        if s.startswith("cmd "):
            cmd = s.split()[1]
            continue
        m = FIELD.match(s)
        if m and cmd == "LC_ID_DYLIB":
            lc["id"] = m.group(2)
        elif m and cmd in DEP_CMDS:
            lc["deps"].append(m.group(2))
        elif m and cmd == "LC_RPATH":
            lc["rpaths"].append(m.group(2))
        elif cmd == "LC_UUID" and s.startswith("uuid ") and lc["uuid"] is None:
            lc["uuid"] = s.split()[1]
        elif cmd == "LC_BUILD_VERSION" and s.startswith("minos ") and lc["minos"] is None:
            lc["minos"] = s.split()[1]
    return lc


def leaf(dep):
    """What a dependency is called whatever path it is reached by:
    libfoo.1.dylib, or QtCore for .../QtCore.framework/Versions/A/QtCore."""
    return os.path.basename(dep)


def system(dep):
    return dep.startswith(("/usr/lib/", "/System/"))


def ghcr(image, path, accept=None):
    """GET from Homebrew's registry with an anonymous pull token."""
    base = "https://ghcr.io"
    scope = "repository:homebrew/core/%s:pull" % image
    token = json.load(urllib.request.urlopen("%s/token?scope=%s" % (base, scope)))["token"]
    headers = {"Authorization": "Bearer " + token}
    if accept:
        headers["Accept"] = accept
    req = urllib.request.Request("%s/v2/homebrew/core/%s/%s" % (base, image, path), headers=headers)
    return urllib.request.urlopen(req).read()


def fetch_bottle(formula, tag, cache):
    """Extract the `tag` bottle of the installed version of `formula` under
    <cache>/<tag>/ and return the keg's directory there."""
    name, pkgver = formula["name"], formula["pkgver"]
    keg = os.path.join(cache, tag, name, pkgver)
    if os.path.isdir(keg):
        return keg
    rebuild = formula["bottle"]["stable"]["rebuild"]
    ref = pkgver + ("-%d" % rebuild if rebuild else "")
    # Homebrew's own image naming (GitHubPackages.image_formula_name).
    image = name.replace("@", "/").replace("+", "x")
    try:
        index = json.loads(ghcr(image, "manifests/" + ref, "application/vnd.oci.image.index.v1+json"))
    except urllib.error.URLError as e:
        die("cannot fetch the bottle manifest of %s %s (%s); the first package needs the network" % (name, ref, e))
    entries = [m for m in index.get("manifests", [])
               if tag in m.get("annotations", {}).get("org.opencontainers.image.ref.name", "").split(".")]
    if not entries:
        have = sorted({t for m in index.get("manifests", [])
                       for t in m.get("annotations", {}).get("org.opencontainers.image.ref.name", "").split(".")
                       if t.startswith("arm64_")})
        die("Homebrew publishes no %s bottle of %s %s (it has %s): its floor has moved past this one, "
            "so scripts/macos-floor.sh should say so after `brew update`" % (tag, name, pkgver, ", ".join(have)))
    digest = entries[0]["annotations"]["sh.brew.bottle.digest"]
    tarball = os.path.join(cache, "%s--%s.%s.tar.gz" % (name, ref, tag))
    ok = False
    if os.path.isfile(tarball):
        with open(tarball, "rb") as f:
            ok = hashlib.sha256(f.read()).hexdigest() == digest
    if not ok:
        try:
            data = ghcr(image, "blobs/sha256:" + digest)
        except urllib.error.URLError as e:
            die("cannot fetch the %s bottle of %s %s (%s)" % (tag, name, pkgver, e))
        if hashlib.sha256(data).hexdigest() != digest:
            die("the %s bottle of %s %s does not match its digest" % (tag, name, pkgver))
        os.makedirs(cache, exist_ok=True)
        with open(tarball + ".part", "wb") as f:
            f.write(data)
        os.replace(tarball + ".part", tarball)
    # Into a scratch directory first, so an interrupted extraction never
    # leaves a keg that the next run would take for a whole one.
    os.makedirs(os.path.join(cache, tag, name), exist_ok=True)
    scratch = tempfile.mkdtemp(dir=os.path.join(cache, tag))
    with tarfile.open(tarball) as t:
        t.extractall(scratch)
    src = os.path.join(scratch, name, pkgver)
    if not os.path.isdir(src):
        die("the %s bottle of %s does not hold %s/%s" % (tag, name, name, pkgver))
    os.replace(src, keg)
    shutil.rmtree(scratch)
    return keg


def swap(staged, old, src, rel, tag, floor):
    """Copy the older build over a staged file and give it what the staging
    gave the file it replaces: install name, dependencies, rpaths."""
    new = load_commands(src)
    if new["minos"] and vt(new["minos"]) > vt(floor):
        die("%s in the %s bottle is itself built for macOS %s" % (rel, tag, new["minos"]))
    mode = os.stat(staged).st_mode
    shutil.copyfile(src, staged)
    os.chmod(staged, mode)
    by_leaf = {leaf(d): d for d in old["deps"]}
    args = []
    if old["id"]:
        args += ["-id", old["id"]]
    for d in new["deps"]:
        if system(d):
            continue
        want = by_leaf.get(leaf(d))
        if want is None:
            die("%s in the %s bottle links %s, which the build it replaces did not" % (rel, tag, d))
        if want != d:
            args += ["-change", d, want]
    for r in new["rpaths"]:
        args += ["-delete_rpath", r]
    # Deleting and adding the same rpath in one call is refused, so two.
    for step in (args, [a for r in old["rpaths"] for a in ("-add_rpath", r)]):
        if not step:
            continue
        p = subprocess.run(["install_name_tool"] + step + [staged], capture_output=True, text=True)
        if p.returncode != 0:
            die("install_name_tool on %s: %s" % (staged, p.stderr.strip()))
    got = load_commands(staged)
    stray = [d for d in got["deps"] if not system(d) and d not in old["deps"]]
    if stray or got["rpaths"] != old["rpaths"] or got["id"] != old["id"]:
        die("%s did not come out with the staged file's load commands (%s)" % (staged, stray or got["rpaths"]))


def main():
    if len(sys.argv) != 5:
        print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
        sys.exit(2)
    contents, floor, tag, cache = sys.argv[1:]
    cache = os.path.abspath(cache)

    # What in the bundle is built for a newer macOS than the floor.
    over = {}
    for root, _dirs, files in os.walk(contents):
        for n in files:
            p = os.path.join(root, n)
            if is_macho(p):
                lc = load_commands(p)
                if lc["minos"] and vt(lc["minos"]) > vt(floor):
                    over[p] = lc
    if not over:
        print("bottles        nothing in the app is above macOS %s" % floor)
        return

    # Where each came from: the file in the Cellar with the same UUID. A
    # staged library carries the name it was linked by, which in a keg is
    # usually a symlink (libzstd.1.dylib -> libzstd.1.5.7.dylib), so the
    # names are followed to the file itself; the bottle holds the same one.
    cellar = os.path.realpath(out("brew", "--cellar").strip())
    names = {os.path.basename(p) for p in over}
    origin = {}
    for root, _dirs, files in os.walk(cellar):
        for n in files:
            if n in names:
                p = os.path.realpath(os.path.join(root, n))
                if p.startswith(cellar + os.sep) and is_macho(p):
                    u = load_commands(p)["uuid"]
                    if u:
                        origin.setdefault(u, p)
    plan = {}
    for p, lc in sorted(over.items()):
        o = origin.get(lc["uuid"])
        if o is None:
            print("bottles        %s is built for macOS %s and is not Homebrew's"
                  % (os.path.relpath(p, contents), lc["minos"]), file=sys.stderr)
            continue
        name, ver, rel = os.path.relpath(o, cellar).split(os.sep, 2)
        plan.setdefault((name, ver), []).append((p, rel))
    if not plan:
        return

    # The bottles of the installed versions. Homebrew builds every tag of
    # a version at once, so the version installed here is the one to ask
    # for -- as long as it is Homebrew's current one, whose rebuild number
    # `brew info` knows.
    info = json.loads(out("brew", "info", "--json=v2", *sorted({n for n, _ in plan})))
    formulae = {}
    for f in info["formulae"]:
        if f.get("tap") != "homebrew/core":
            die("%s comes from %s, and only homebrew/core has bottles for every macOS" % (f["name"], f.get("tap")))
        stable, rev = f["versions"]["stable"], f.get("revision", 0)
        f["pkgver"] = stable + ("_%d" % rev if rev else "")
        formulae[f["name"]] = f
    for name, ver in sorted(plan):
        f = formulae.get(name)
        if f is None:
            die("brew info knows no %s" % name)
        if ver != f["pkgver"]:
            die("%s %s is installed and Homebrew's bottles are of %s: brew upgrade %s" % (name, ver, f["pkgver"], name))

    swapped = 0
    for (name, ver), items in sorted(plan.items()):
        keg = fetch_bottle(formulae[name], tag, cache)
        for staged, rel in items:
            src = os.path.join(keg, rel)
            if not is_macho(src):
                die("the %s bottle of %s %s has no %s" % (tag, name, ver, rel))
            swap(staged, over[staged], src, rel, tag, floor)
            swapped += 1
        print("bottles        %s %s from %s: %d file%s" % (name, ver, tag, len(items), "" if len(items) == 1 else "s"))
    print("bottles        %d file%s swapped for macOS %s builds" % (swapped, "" if swapped == 1 else "s", floor))


if __name__ == "__main__":
    main()
