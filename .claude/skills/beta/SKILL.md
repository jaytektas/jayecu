---
name: beta
description: Put a beta firmware kit (and optionally the studio) on the bench machine for testing, or publish a beta as a GitHub pre-release for opt-in studios, in the order that works. Use when asked to "put a beta on the bench", "make a test kit", "stage the firmware", "update the bench", "push a beta", "publish a beta", or whenever new firmware needs testing before a release.
---

# A beta on the bench

A beta is how new firmware reaches the bench ECU before a release: a firmware **kit** installed into the
bench studio, which offers the update on the next connect and carries the tune across. Nothing is
published. To send a beta to other people's studios, see **Publishing a beta** below. For a release, use
the `release` skill instead.

## Do it with the script — never by hand

```
make bench-beta BENCH=bench.lan              # the kit only
make bench-beta BENCH=bench.lan STUDIO=1     # …and deploy the studio (it must be closed on the bench)
```

`tools/bench_beta.sh` does the steps in the only order that works, and checks each one:

1. **Everything firmware-side is committed.** The build id is the newest commit touching `firmware`,
   `definition`, `codegen`, … — a `-dirty` build cannot be told from any other. It refuses otherwise.
2. **The firmware is built**, which runs codegen first, so the meta, `version.h` and the image all name
   the same build.
3. **The kit is made from that image.** `make_kit.py` reads the build out of `code.bin` and refuses a
   kit whose meta names another.
4. **The version sorts above every beta the bench has seen** (`<next patch>-beta.<N+1>`): the studio
   offers kits by version only, so a beta numbered below an installed one is never offered.
5. **Installed** into the bench studio's firmware folder and copied to `~/Desktop/test-kit`.
6. With `STUDIO=1`, the AppImage is built and deployed (`bench_deploy.sh`) — only if the studio is closed
   on the bench. If it is open the script says so; ask the user to close it, then run again.

## Mistakes this exists to prevent

- **Running `make_kit.py` without rebuilding the firmware.** A codegen after the last firmware build
  (any schema edit does one) leaves an image reporting the OLD build inside a kit labelled with the new.
  The studio flashes it, sees the old build come back, and refuses to put the tune back
  ("came back running 43ab121, not 1efcde7").
- **A beta numbered below one already on the bench** — never offered.
- **A partial kit** (no `kit.json` or dashboard) copied over after a failed kit build.
- **Replacing the AppImage under a running studio.**

## Afterwards

Tell the user: the version and build, that it is installed, and what to do — connect and accept the
update. Anything the new firmware changes in the output templates needs the output wizard run again on
the outputs that use them.

## Publishing a beta (opt-in studios get it over the air)

A published beta is a GitHub **pre-release** on jaytektas/jayecu. `/releases/latest` never returns a
pre-release, so only studios that opted in see it: Preferences ▸ **Include beta studio versions** (the
studio updates itself) and **Include beta firmware** (the kits are offered on connect).

1. Everything committed; tests green (the `release` skill's step 1); firmware notes under
   `## Unreleased` in `firmware/CHANGES.md` — a beta kit carries them as its own notes.
2. Both SDKs at the JFramework pin (`make sdk`, `make sdk-win`) — the script refuses otherwise.
3. `make beta-release [BETA_BOARDS="jaytek_v1 proteus_f7"]` (~10 min, run it in the background). It picks
   studio `<next patch>-beta.N` and firmware `<next patch>-beta.M`, each one past every beta published and
   built here, builds firmware → kit per board, builds and packages the studio as that version with those
   kits, puts the build dirs back to the plain version, and gathers `release/v<studio beta>/`.
4. Check it (`ls release/v<beta>/`, each `*-kit.json`'s version and build = the last firmware commit).
5. **Ask the user before publishing** — it is public. The tag's commit must be on GitHub: push it to the
   **`beta`** branch there — never the local work branch (its name belongs to other work):
   ```
   git push --force origin HEAD:refs/heads/beta
   gh release create v<studio beta> --prerelease --target <commit> --title "jayecu v<studio beta> (beta)" \
       --notes-file <notes> release/v<studio beta>/*
   gh release view v<studio beta> --json isPrerelease,assets --jq '{pre:.isPrerelease,n:(.assets|length)}'
   ```
   Notes: the Unreleased firmware lines, the studio's changes since the last tag, "Beta — for testing."
   Never publish a beta without `--prerelease`: it would become Latest and reach every studio.

A beta's firmware version (`0.4.4-beta.3`) sorts below the release it leads to (`0.4.4`), so the real
release later supersedes it everywhere. `make release` is unaffected: it uses `kits/`, betas use `betas/`.
