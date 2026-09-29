# Firmware changes

What each firmware version changes, in plain words for the person updating an ECU. The studio shows
the sections newer than the ECU runs before it offers an update, beside the settings the update adds
or retires.

Written WITH the work: a commit that changes something a user would notice adds a line under
Unreleased in the same commit. `make release` turns Unreleased into the next version's section and
raises the version (tools/release_notes.py). A line per change; no commit hashes, no file names.

## Unreleased

## 0.4.1
- Output test: the repeat count goes up to 100,000, and a long test is no longer stopped after
  10 minutes — it runs for as long as its count and timings say.
- Output test: the Count, On Time and Off Time boxes show their units and fit the larger count.

## 0.4.0
- First public firmware.
