---
name: release
description: Build and publish a jayecu release — studio packages plus a firmware kit per board — to github.com/jaytektas/jayecu. Use when asked to "release", "cut a release", "ship it" or "push a release".
---

# Releasing jayecu

One GitHub release carries **both** the studio (Linux AppImage + Windows installer) and a **firmware
kit for every board** (`definition/boards/*.board.yaml`). It is tagged with the **studio's** version
(`v0.3.7`), because the studio's updater compares that tag with its own version. Firmware has its own
version (`firmware/version.txt`, e.g. 0.4.3) and the studio picks kits **by firmware version only — never
by build**, so a kit whose firmware changed must carry a new firmware version or nobody is offered it.

Publishing is public and outward-facing. **Ask before pushing** unless the user has said to push in this
request. Never delete or overwrite a published release without being asked.

## 0. Before anything

- Everything to ship is **committed** on `master`; `git status --short` is empty. Work happens on
  `master` directly — there is no work branch.
- **Firmware notes are written.** `firmware/CHANGES.md` → `## Unreleased` holds a plain-words line for
  every change a user would notice in the firmware *or the kit* (the kit carries the meta and the
  dashboard, so page changes count). No hashes, no file names. The release step refuses if the firmware
  changed and Unreleased is empty — it prints the commits to write them from.
- **JFramework:** if `apps/studio-jf/jframework.commit` moved, that commit must be **pushed** to
  github.com/jaytektas/JFramework first (fresh clones and the Windows build fetch it), and both SDKs must
  be rebuilt to it:
  ```
  cat apps/studio-jf/jframework.commit ~/jframework-sdk/jframework.commit ~/jframework-sdk-win/jframework.commit
  make sdk        # if the Linux SDK differs
  make sdk-win    # if the Windows SDK differs — the pre-build check only WARNS, and a stale SDK
                  # surfaces as a compile error halfway through the release
  ```

## 1. Tests — all green before building

```
make tests                                                    # firmware host tests (ctest, tests/build)
cmake --build apps/studio-jf/build --target studio_tests --parallel
cmake --build apps/studio-jf/build --parallel --target card_logs_test tab_pages_test \
      viewport_unbound_test page_retention_test viewport_repoint_test   # EXCLUDE_FROM_ALL, still in ctest
ctest --test-dir apps/studio-jf/build                         # every test must PASS, none "Not Run"
(cd apps/studio-jf/tools/layout && python3 check_doc.py && python3 check_all.py)
```
`check_all` has one long-standing complaint (knock page, "Auto takes the input" overlaps a panel);
anything else is new and must be fixed.

**Golden snapshots** (`apps/studio-jf/tests/golden/locations.txt`, `table_dims.txt`) fail whenever the
schema adds, removes or moves fields. Re-bless them only after checking the diff is exactly the intended
schema change — alignment-padding names (`_align_pad_N`) moving with offsets is expected:
```
./apps/studio-jf/build/locate_equiv_test --emit apps/studio-jf/tests/golden/locations.txt
./apps/studio-jf/build/table_dims_test  --emit apps/studio-jf/tests/golden/table_dims.txt
```

## 2. The manual says what this release does

The manual ships inside both packages (Help ▸ User Manual) and on GitHub Pages, so a stale manual ships
with every release. Before building, make it describe what changed:

1. **What changed:** the `## Unreleased` notes, plus the studio's user-visible commits since the last tag
   (`git log --oneline v<last>..HEAD -- apps/studio-jf`).
2. **Where it is written about:** for each change, search `manual/docs` for its studio labels, setting
   paths and the files its `<!-- src: -->` notes cite. Settings the schema retired must not be named
   anywhere any more:
   ```
   git diff v<last>..HEAD -- definition/ecu.schema.yaml | grep '^-.*name:'     # retired names
   grep -rn '<name>' manual/docs                                                # every mention to fix
   ```
3. **Write it** to `manual/STANDARD.md`: plain English, why before how, studio labels in **bold**
   (**Fuel Tuning ▸ Stage 1 ▸ Fuel**), a level badge per section, and a `<!-- src: -->` note naming the
   files the facts come from (files, never line numbers). A new feature gets its section in the chapter
   a reader would look in, not an appendix.
4. **Screenshots** of pages that changed: re-run the page's script in `manual/tools/shots/`
   (they drive `manual/tools/shoot.sh`: a demo project, offline, in a throwaway HOME) and look at the
   result before keeping it.
5. `make manual` must pass — it regenerates the settings/reference pages from the schema, runs
   `check_src.py`, and builds with `--strict` (a broken link or anchor fails it).

Commit the manual with the release's other work (e.g. `manual: per-stage fuel`).

## 3. Studio version

Every release bumps the studio version (the tag must be newer than the latest published release):
`apps/studio-jf/CMakeLists.txt` line 5, `project(jframework_studio VERSION x.y.z ...)`. Commit it alone:
`studio x.y.z`.

## 4. Build

```
make release        # ~10 min — run it in the background and wait for it to finish
```
It does, in order:
1. `tools/release_notes.py prepare` — if the firmware changed since the newest published kit, requires
   Unreleased notes, raises `firmware/version.txt` a patch (a hand-raised minor/major is kept), turns
   Unreleased into that version's section and **commits** `firmware x.y.z`. Firmware unchanged → no bump.
2. A kit for every other board, then the studio packages (which bundle the kits of this firmware version).
3. `tools/make_release.py` gathers everything into `release/v<studio>/` with `SHA256SUMS`, and refuses a
   board+version already published with a different build.

**Re-running after a failure is safe**: prepare sees the version is prepared but unpublished and carries
on (folding any new Unreleased notes into it) instead of bumping again.

## 5. Verify before publishing

```
ls release/v<studio>/          # 10 files: setup.exe, AppImage, per board: -firmware.bin, -kit.json, .meta, (.gui)
for f in release/v<studio>/*-kit.json; do python3 -c "import json,sys;k=json.load(open(sys.argv[1]));print(k['board'],k['version'],k['build'],[n['version'] for n in k['notes']])" $f; done
ls firmware/build/ship-kits    # ONLY this release's firmware version — the packages bundle these
git status --short             # empty
git log -1 --format=%h -- firmware definition codegen generated third_party cmake ':(exclude)firmware/CHANGES.md'
```
Every kit's `build` equals that last hash, its `version` is the new (or unchanged) firmware version, and
its notes include the new section. A kit whose version already exists in an older release with a
different build must never be published (it would never be offered, and the packages would mislabel it).

## 6. Publish (after the user's go-ahead)

```
git push origin master
gh release create v<studio> --target master --title "jayecu v<studio>" --notes "<notes>" release/v<studio>/*
gh release view v<studio> --json isDraft,isPrerelease,assets --jq '{draft:.isDraft,pre:.isPrerelease,n:(.assets|length)}'
gh release list -L 2           # the new one is Latest
```
Release notes: the new firmware section from `firmware/CHANGES.md` (headed "Firmware x.y.z (build …)"),
then the studio's user-visible changes since the last release (from `git log` of apps/studio-jf), then
"Firmware kits for <boards>." A beta (`x.y.z-beta.N`) is published with `--prerelease`.

## 7. Afterwards

- **The online manual:** `make manual-publish` pushes `manual/site` to the gh-pages branch
  (https://jaytektas.github.io/jayecu/). Public, so it goes with the release's go-ahead; run it from the
  commit the release was built from.
- Tell the user: the release URL, the studio and firmware versions and build, and anything not verified
  (e.g. not tried on the bench).
- Existing ECUs get changed pages through the studio's "Newer pages for this ECU" offer once they open
  the ECU with the new studio; a firmware update also offers the kit's notes.
- Optional, when asked: `make bench-studio` puts the new AppImage on bench.lan (replaces the bench's
  installed studio — the user's studio there must be closed first).

## Mistakes this procedure exists to prevent

- v0.3.4 shipped changed firmware still labelled 0.4.0 — never offered, and the packages bundled it under
  the wrong name. (Now caught by prepare + make_release.)
- A release built while the Windows SDK was behind the JFramework pin — a compile error mid-release.
- Snapshot tests left failing, or tests "Not Run" because their binaries were never built.
- A manual that still describes the last release: retired settings named, new features missing, old
  screenshots — shipped inside both packages and online.
