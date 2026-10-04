---
name: spec
description: Build a feature spec with Jay by interviewing him, one question at a time, and writing the answers into a spec file he watches in the editor. Use when asked to "spec", "write a spec", "spec out", "interview me", "hash out", or `/spec <name>`; also to pick up an existing spec in docs/specs/ and carry on.
---

# Building a spec

A spec is a Markdown file in `docs/specs/<name>.md`, open in Jay's editor while we work. The file is the
record — every answer goes into it as it is given, so the editor shows the spec growing. Chat is for
the questions, not the content.

`/spec <name>` starts or resumes `docs/specs/<name>.md`. No name → ask for one (short, kebab-case).

## 1. Start or resume

- **New:** create the file from the template below, every section `_open_`. Say the path in one line.
- **Existing:** read it, and pick up at the first section still `_open_` or the first open question.
  Say where we are in one line.
- **Read the code the feature touches before the first question** — the schema
  (`definition/ecu.schema.yaml`), the module(s) under `firmware/`, the studio page if there is one, and
  any `docs/*-design.md` on the subject. Questions the code already answers are not asked; they are
  written in as facts, with the file named.

## 2. Interview

- **One question at a time**, with AskUserQuestion: 2–4 concrete options, the one the code or common
  practice points to first and marked (Recommended), each with its consequence in a line. A question
  with no sensible options is asked plainly in one line.
- Order: what problem and for whom → what it does (behaviour, edge cases, failure / fail-safe) →
  settings and defaults → what the studio shows → how we test it on the bench → what is out of scope.
- **Write each answer into the file straight away** (Edit), in the spec's own words — plain English, why
  before how, as the manual is written (`manual/STANDARD.md`). Do not batch answers up.
- **Challenge, don't transcribe.** If an answer contradicts the code, another section, a safety rule
  already in the firmware (fail-safes, key-off / engine-stopped gates, latches) or the memory notes, say
  so in one line and ask which wins. Record the decision and the reason under **Decisions**.
- Anything not settled goes under **Open questions** with who or what would settle it — never guessed.
- Keep replies short: the question, nothing else. Every ~5 answers, one line on what is settled and
  what is next.

## 3. Finish

When every section is filled or its gaps are listed as open questions:
- Read the whole file once for contradictions and missing failure cases; ask about any found.
- Set **Status** to `agreed` only when Jay says so; otherwise `draft`.
- Offer a second opinion from the local model (Qwen in Continue): a prompt Jay can paste, asking it to
  review the file for gaps and contradictions. Neither model edits the file while the other is.
- Do not commit unless asked. Do not start implementing — the spec is the deliverable.

## Template

```markdown
# <Title>

**Status:** draft · **Started:** <YYYY-MM-DD> · **Owner:** Jay

## Problem
_open_ — what is wrong or missing today, and who it hurts.

## Goals
_open_

## Non-goals
_open_

## Behaviour
_open_ — what it does, step by step, including startup, key-off / key-on and engine-stopped.

## Failure and fail-safe
_open_ — every way it can go wrong, what the ECU does, which DTC, latched or self-healing.

## Settings
_open_ — name, units, range, default, and why that default.

## Studio
_open_ — which page, what is shown, what the user does.

## Testing
_open_ — unit tests, and the bench procedure (tools/bench_*.py) that proves it on hardware.

## Decisions
_none yet_ — decision, reason, date.

## Open questions
_none yet_ — question, and what would settle it.
```
