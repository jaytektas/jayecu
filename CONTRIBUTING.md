# Contributing to JayECU

## Before a first patch

Read `CLA.md` and add yourself to `CONTRIBUTORS.md` in the same change. Sign off
each commit:

    git commit -s

The short version of the agreement: you keep the copyright in your work, and you
grant this project a licence broad enough to relicense it. That last part is what
lets the firmware and the studio continue to be offered under terms other than GPLv3, which it
loses the ability to do the first time a contribution arrives without it.

If you would rather not grant that, a bug report, a failing test or a description
of the fix is still welcome and needs no agreement.

## What the code has to look like

Everything under `generated/` and `shared/` is produced from `definition/`
by `codegen/codegen.py` — the schema is the source of truth, not the C++. A change
to a field, a table or a signal belongs in `definition/ecu.schema.yaml`, and the
generated artefacts follow from a `make`. They are not stored in git, and editing
one is a change that disappears the next time anyone builds.

Board differences live in `definition/boards/*.board.yaml`, never in `#ifdef`s.

The studio (`apps/studio-jf`) is built on JFramework — no Qt, no GTK. It consumes
the framework as an installed SDK and nothing from the studio leaks back into it;
if a change needs something from the framework, it belongs in the framework, as
its own patch, under its own CLA.

The shipped dashboard is not edited in the studio. It is authored by the scripts
in `apps/studio-jf/tools/layout/` and lives at
`definition/boards/<board>.dashboard.gui`. A studio's own per-ECU document is a
user's file and is never the thing a release is cut from.

## Before submitting

    make tests            # native unit tests
    make firmware         # must build clean for the board you touched
    make studio           # must build clean if you touched the studio
    python3 apps/studio-jf/tools/layout/check_doc.py   # if the change touches pages or widgets

If a change moves the config layout, `layout_hash` moves with it, and a stored
tune from before the change will be rejected by the firmware's boot gate. That is
the gate working — but say so in the commit message, because it means everyone
else's saved tune needs re-burning.

Say what the change does and why the previous behaviour was wrong. A commit
message that explains the reasoning is worth more than one that lists the files
touched — the diff already lists the files.
