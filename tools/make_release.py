#!/usr/bin/env python3
"""Gather one release into one folder, named the way the studio's two update checks read it.

    python3 tools/make_release.py --studio-version X.Y.Z [--boards "jaytek_v1 proteus_f7"] [--out DIR]
    python3 tools/make_release.py --studio-version X.Y.Z-beta.N --fw-version A.B.C-beta.M   (make beta-release)

`make release` builds everything first and then runs this. The folder it writes is uploaded, file for file,
as the assets of ONE GitHub release on jaytektas/jayecu, tagged vX.Y.Z (the studio's version):

    jayecu-studio-X.Y.Z-x86_64.AppImage     the studio's own update on Linux   (JRelease: ends -x86_64.AppImage)
    jayecu-studio-X.Y.Z-setup.exe           the studio's own update on Windows (JRelease: ends -setup.exe)
    <board>-<fw>-kit.json                   each board's kit label             (FirmwareFetch: ends -kit.json)
    <board>-<fw>-firmware.bin / .meta / .gui   the files that label names
    SHA256SUMS                              every file above; both checks refuse a file it does not list

The kit folder holds its label as plain kit.json; the release must call it <board>-<fw>-kit.json or the
studio never sees the kit. That rename, and a SHA256SUMS that covers the studio packages AND the kits, are
the two things easy to get wrong by hand, which is why this exists.

THE TAG IS THE STUDIO'S VERSION. The studio compares it with its own, so a release whose tag is not newer
than the one already published is never offered — even one that only changes firmware. This checks the
latest published release and refuses a tag that would not be newer.
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import sys
import urllib.error
import urllib.request

REPO = pathlib.Path(__file__).resolve().parent.parent
LATEST = "https://api.github.com/repos/jaytektas/jayecu/releases/latest"
RELEASES = "https://api.github.com/repos/jaytektas/jayecu/releases?per_page=100"


def version_key(v: str):
    """1.2.3 < 1.2.4-beta.1 < 1.2.4 — the numbers, then a pre-release sorts before its release."""
    v = v.lstrip("v")
    base, _, pre = v.partition("-")
    nums = tuple(int(x) for x in base.split(".") if x.isdigit())
    return nums + (0,) * (3 - len(nums)), (1,) if not pre else (0, pre)


def die(msg: str):
    print(f"make_release: {msg}", file=sys.stderr)
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--studio-version", required=True)
    ap.add_argument("--fw-version", help="default: firmware/version.txt; a beta's kits are in <board>/betas")
    ap.add_argument("--boards", default="jaytek_v1")
    ap.add_argument("--out", help="default: release/v<studio-version>")
    ap.add_argument("--offline", action="store_true", help="skip the check against the published release")
    ap.add_argument("--allow-dirty", action="store_true", help="accept kits built from uncommitted changes (testing)")
    a = ap.parse_args()

    ver = a.studio_version
    fw_ver = a.fw_version or (REPO / "firmware" / "version.txt").read_text().strip()
    kits = "betas" if "-" in fw_ver else "kits"
    out = pathlib.Path(a.out) if a.out else REPO / "release" / f"v{ver}"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    files = []

    def put(src: pathlib.Path, name: str):
        if not src.is_file():
            die(f"{src} is missing")
        if name in files:
            die(f"two files would both be called {name}")
        shutil.copyfile(src, out / name)
        files.append(name)

    # The studio packages, for exactly this version.
    put(REPO / f"apps/studio-jf/build/jayecu-studio-{ver}-x86_64.AppImage", f"jayecu-studio-{ver}-x86_64.AppImage")
    put(REPO / f"apps/studio-jf/installer/Output/jayecu-studio-{ver}-setup.exe", f"jayecu-studio-{ver}-setup.exe")

    # One kit per board, the one for this firmware version.
    for board in a.boards.split():
        kit = REPO / "firmware" / "build" / board / kits / f"{board}-{fw_ver}"
        label_path = kit / "kit.json"
        if not label_path.is_file():
            die(f"no kit for {board} {fw_ver} at {kit} — make kit BOARD={board} (a beta: make beta-release)")
        label = json.loads(label_path.read_text())
        if label.get("board") != board or label.get("version") != fw_ver:
            die(f"{label_path} describes {label.get('board')} {label.get('version')}, not {board} {fw_ver}")
        build = label.get("build", "")
        if (not build or build.startswith("unknown") or build.endswith("-dirty")) and not a.allow_dirty:
            die(f"{board}'s firmware says build '{build}': it was not built from a clean commit, so an ECU running it "
                f"could not be traced to its source. Commit, then make release (--allow-dirty to test only).")
        need = label.get("min_studio", "")
        if need and version_key(need) > version_key(ver):
            die(f"{board} {fw_ver} needs studio {need}, newer than this release's studio {ver}")
        put(label_path, f"{board}-{fw_ver}-kit.json")
        for key in ("firmware", "meta", "dashboard"):
            if label.get(key):
                put(kit / label[key], label[key])

    # The studio's updater takes THE file ending in its platform's suffix — so exactly one of each.
    for suffix in ("-x86_64.AppImage", "-setup.exe"):
        n = sum(1 for f in files if f.endswith(suffix))
        if n != 1:
            die(f"{n} files end in {suffix}; the studio's updater needs exactly one")

    sums = "".join(f"{hashlib.sha256((out / n).read_bytes()).hexdigest()}  {n}\n" for n in sorted(files))
    (out / "SHA256SUMS").write_text(sums)

    # Would the studios out there see it? Only if the tag is newer than the latest published release.
    if not a.offline:
        try:
            with urllib.request.urlopen(urllib.request.Request(LATEST, headers={"User-Agent": "jayecu-release"}),
                                        timeout=15) as r:
                rel = json.load(r)
            latest = rel.get("tag_name", "")
            if latest and version_key(ver) <= version_key(latest):
                die(f"the latest published release is {latest}; v{ver} is not newer, so no studio would offer "
                    f"it. Raise the studio's version (apps/studio-jf/CMakeLists.txt project VERSION).")
            # A KIT IS KNOWN BY ITS VERSION ALONE. The studio compares kit versions and never looks at the
            # build, so different firmware under a version already published is never offered to anyone —
            # and the packages would bundle it under a name that means something else. Same build is fine
            # (a studio-only release carrying the unchanged kit); a different one needs version.txt raised.
            # EVERY release, not just the latest: an older one still serves its kits, and a clash with any
            # of them is the same clash.
            with urllib.request.urlopen(urllib.request.Request(RELEASES, headers={"User-Agent": "jayecu-release"}),
                                        timeout=15) as r:
                releases = json.load(r)
            # A BETA must also be newer than every published beta: a beta studio compares the tag with
            # its own, pre-releases included, so v0.3.8-beta.2 after v0.3.8-beta.3 reaches nobody.
            if "-" in ver:
                newest = max((x.get("tag_name", "") for x in releases), key=version_key, default="")
                if newest and version_key(ver) <= version_key(newest):
                    die(f"{newest} is already published; v{ver} is not newer, so no beta studio would offer it")
            for board in a.boards.split():
                ours = json.loads((out / f"{board}-{fw_ver}-kit.json").read_text()).get("build", "")
                for rel in releases:
                    url = {x.get("name"): x.get("browser_download_url")
                           for x in rel.get("assets", [])}.get(f"{board}-{fw_ver}-kit.json")
                    if not url:
                        continue
                    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "jayecu-release"}),
                                                timeout=15) as k:
                        published = json.load(k).get("build", "")
                    if published != ours:
                        die(f"{rel.get('tag_name')} already ships {board} {fw_ver} as build {published}; this one is "
                            f"build {ours}. No studio would offer it. Raise firmware/version.txt and make release again.")
            print(f"  latest published: {latest or 'none'}")
        except urllib.error.HTTPError as e:
            if e.code != 404:
                die(f"could not ask GitHub for the latest release (HTTP {e.code}); --offline to skip")
            print("  latest published: none (jaytektas/jayecu has no releases yet)")
        except OSError as e:
            die(f"could not ask GitHub for the latest release ({e}); --offline to skip")

    print(f"  release: {out.relative_to(REPO)}")
    for n in sorted(files) + ["SHA256SUMS"]:
        print(f"    {n}")
    beta = "-" in ver
    print(f"  tag v{ver} on jaytektas/jayecu (public), upload every file above"
          + (", mark it a PRE-RELEASE (beta channel only)" if beta else ", a full release (not a pre-release)"))


if __name__ == "__main__":
    main()
