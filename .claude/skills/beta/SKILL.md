---
name: beta
description: Put a beta firmware kit (and optionally the studio) on the bench machine for testing, in the order that works. Use when asked to "put a beta on the bench", "make a test kit", "stage the firmware", "update the bench", or whenever new firmware needs testing on the bench ECU before a release.
---

# A beta on the bench

A beta is how new firmware reaches the bench ECU before a release: a firmware **kit** installed into the
bench studio, which offers the update on the next connect and carries the tune across. Nothing is
published. For a release, use the `release` skill instead.

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
