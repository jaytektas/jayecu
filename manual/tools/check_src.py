#!/usr/bin/env python3
"""Check the manual's source notes: every file a note cites exists, and no note cites a line number.

A source note sits under a paragraph and says where its facts come from:

    <!-- src: firmware/Engine/Modules/Dfco.cpp (cut and resume rpm); definition/ecu.schema.yaml (dfco.*) -->

It names FILES, and says what to look for in them in words — a function, a setting, a table. It never
gives a line number: a line number is wrong the first time anything above it in the file changes, and
nothing would notice. The file names can go stale too (a file is renamed or deleted), which is what this
catches, at `make manual`, instead of years later.

    python3 manual/tools/check_src.py            # from the repo root; exit 1 on any problem
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DOCS = ROOT / "manual" / "docs"
NOTE = re.compile(r"<!--\s*src:(.*?)-->", re.S)
# A path is anything with a slash and a file extension, or a known top-level file. Paths in brackets
# are descriptions of what to look for and are not checked.
PATH = re.compile(r"(?<![\w/.-])((?:[\w.{},-]+/)+[\w.{},-]*\.[A-Za-z]\w*|Makefile|LICENSE(?:\.exception)?|README\.md|THIRD-PARTY\.md)")
LINE = re.compile(r"(?<![\w/.-])((?:[\w.{},-]+/)*[\w.{},-]+\.[A-Za-z]\w*|Makefile|LICENSE):\d")
EXTERNAL = ("JFramework ",)      # a path prefixed by one of these is in another repository
OWN_REPOS = ("tools/Ardu-Stim/",)  # cloned in by `make stim`; its own repository, so not checked here
# A bare file name (no directory) must name a file somewhere in the repository; one that names nothing
# is a file that was renamed or deleted. Build output and git-ignored folders are not the repository.
BARE = re.compile(r"(?<![\w/.-])([A-Za-z_][\w-]*\.(?:cpp|h|c|py|yaml|json|md|sh|iss|txt|ld|s|cmake|gui|rules|ino))(?![\w/])")
SKIP_DIRS = {"build", "build-win", ".git", ".venv", "site", "reference", ".stm32_cache", "node_modules"}


def repo_names() -> set[str]:
    names = set()
    for p in ROOT.rglob("*"):
        if p.is_file() and not SKIP_DIRS.intersection(p.relative_to(ROOT).parts):
            names.add(p.name)
    return names


def top_level(body: str) -> str:
    """The note with everything inside brackets blanked, so descriptions are not read as paths."""
    out, depth = [], 0
    for ch in body:
        if ch in "([":
            depth += 1
        out.append(ch if depth == 0 else " ")
        if ch in ")]":
            depth = max(0, depth - 1)
    return "".join(out)


def expand(path: str) -> list[str]:
    """`a/{b,c}/d` -> `a/b/d`, `a/c/d`."""
    m = re.search(r"\{([^{}]*)\}", path)
    if not m:
        return [path]
    return [p for alt in m.group(1).split(",") for p in expand(path[:m.start()] + alt + path[m.end():])]


def main() -> int:
    problems = []
    names = repo_names()
    for md in sorted(DOCS.rglob("*.md")):
        rel = md.relative_to(ROOT)
        text = md.read_text()
        for m in NOTE.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            body = top_level(m.group(1))
            for lm in LINE.finditer(body):
                problems.append(f"{rel}:{line}: cites a line number ({lm.group(0)}…) — name the file and what to look for")
            for pm in PATH.finditer(body):
                before = body[max(0, pm.start() - 12):pm.start()]
                if any(before.endswith(e) for e in EXTERNAL):
                    continue
                if pm.group(1).startswith(OWN_REPOS):
                    continue
                for p in expand(pm.group(1)):
                    if SKIP_DIRS.intersection(pathlib.PurePosixPath(p).parts):
                        problems.append(f"{rel}:{line}: cites {p}, which is build output or not in the repository")
                    elif not (ROOT / p).exists():
                        problems.append(f"{rel}:{line}: cites {p}, which does not exist")
            for bm in BARE.finditer(body):
                before = body[max(0, bm.start() - 12):bm.start()]
                if any(before.endswith(e) for e in EXTERNAL):
                    continue
                if bm.group(1) not in names:
                    problems.append(f"{rel}:{line}: cites {bm.group(1)}, which is no file in the repository")
    for p in problems:
        print(p, file=sys.stderr)
    if problems:
        print(f"check_src: {len(problems)} problem(s) in the manual's source notes", file=sys.stderr)
        return 1
    print("  source notes: every cited file exists, no line numbers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
