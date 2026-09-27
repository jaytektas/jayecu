# The jayecu User Manual — writing standard

Every chapter is written to this standard. It is the brief each author (person or agent) works from,
and the checklist a chapter is reviewed against before it is accepted.

## 1. What the manual is

A **reference book** for the jayecu ECU and jayecu Studio that takes a reader from their first
wiring job to advanced tuning. It is read two ways, and every chapter serves both:

- **Front to back** by someone learning: concepts build in order, each chapter says what it assumes.
- **Dipped into** by someone with a problem: every heading is findable from the index and search,
  and a section makes sense on its own.

It is published as a static HTML site (MkDocs + Material), shipped inside the studio (Help ▸ User
Manual opens the local copy in the browser) and on the project's GitHub Pages.

## 2. Audience and levels

Readers range from a first-time installer to a professional calibrator. Every section carries one
level badge, placed directly under its heading:

| Badge | Meaning |
|---|---|
| `:material-circle:{ .level-basic } Basic` | No prior EFI knowledge. Explains every term the first time. |
| `:material-circle:{ .level-intermediate } Intermediate` | Has a running engine and knows what VE, lambda and timing are. |
| `:material-circle:{ .level-advanced } Advanced` | Calibrator, or someone wiring unusual hardware. May assume the rest of the manual. |

A chapter usually moves from Basic at the top to Advanced at the bottom. A reader should be able to
stop at the first Advanced section and still have a working configuration.

## 3. Voice

- Plain English, second person ("you"), present tense, active voice. Short sentences.
- Explain **why** before **how**. A setting described only by its name is not described.
- Metric units first (kPa, °C, mm, ms, lambda); give AFR/°F/psi in brackets where readers expect them.
- Name things exactly as the studio shows them, in **bold** (**Fuel Tuning ▸ VE Table**,
  **Tools ▸ Trigger Designer**). Settings are named by their studio label, with the schema path in
  `code` the first time (**Stoich AFR** `FuelCalculator.stoich_afr_x10`).
- No marketing language. No comparisons with other ECUs and no "like brand X does" — describe jayecu
  on its own terms. Naming another product is fine only where jayecu **works with** it (importing a
  TunerStudio `.ini`, a Haltech CAN template, a MegaLogViewer log).

## 4. Accuracy — the rule that overrides every other rule

**Nothing is written from general EFI knowledge where the firmware decides the answer.** Every
behaviour, default, range, unit, limit and sequence is taken from this repository:

| Question | Source of truth |
|---|---|
| A setting's name, units, range, default, help | `definition/ecu.schema.yaml` |
| What the firmware actually does with it | `firmware/` (the module's source) |
| Pins, connectors, electrical limits | `definition/boards/*.board.yaml`, `firmware/Platform/boards/` |
| Trigger decoding | `firmware/Scheduler/`, `docs/generic-trigger-design.md` |
| Studio behaviour and page layout | `apps/studio-jf/` and the running studio |
| DTCs | `definition/ecu.schema.yaml` (`firmware_dtc`, `module_dtc`), `docs/dtc-codes.md` |

Each factual paragraph carries a hidden source note for maintainers, e.g.
`<!-- src: firmware/Engine/Modules/Dfco.cpp (cut and resume rpm); definition/ecu.schema.yaml (dfco.*) -->`.
A note names the FILE, with its full path from the repository root, and says in words what to look for
in it — a function, a setting, a table. **Never a line number**: it is wrong the first time anything
above it changes, and nothing notices. `make manual` runs `manual/tools/check_src.py`, which fails the
build if a note cites a line number or a file that does not exist. When the source is ambiguous or the author
cannot confirm something, it is **not guessed**: it goes in the chapter's `open-questions` list
(section 9) and the text says only what is certain.

General EFI theory (what VE is, why knock happens) may be written from knowledge, but must not
contradict how jayecu implements it.

## 4a. The worked example

**Chapter 24, Boost (`docs/part3/24-boost.md`), is the reference chapter.** It was written to this
standard as the pilot: read it before writing, and match its depth, structure, source notes, figures,
examples and tables. Its screenshots are made by `manual/tools/shots/boost.sh` and its diagrams are
`docs/img/diagrams/boost-*.svg`, and a new chapter follows the same pattern.

## 5. Chapter template

Chapters come in two shapes. Use the one that fits; keep the section order.

### 5a. Module chapter (one firmware module, e.g. Boost, Launch, Knock)

```markdown
# <Module name as the studio shows it>

<level badge>

> **In one sentence:** what this module does for the engine.

## What it does                      (Basic)
Plain-language description. What problem it solves, when you need it, when you do not.

## How it works                      (Intermediate)
The mechanism, with at least one diagram (figure). Inputs it reads, outputs it drives, the
decision logic, the order things happen in. State machines as diagrams.

## Before you start                  (Basic)
Hardware and wiring it needs (link to the wiring chapter), other modules that must be set up
first, sensors it depends on.

## Setting it up                     (Basic → Intermediate)
Step-by-step in the studio, one numbered step per action, a screenshot for every page visited.
Every setting on the page is explained in the order it appears.

## Worked examples                   (Intermediate)
At least two realistic setups with the actual values used and why (e.g. "single-turbo 2JZ,
wastegate on a 3-port solenoid"; "NA 4-cylinder, launch with ignition cut").

## Tuning it                         (Intermediate → Advanced)
How to arrive at good values on a running engine: what to log, what to look for, safe first
values, how to iterate.

## Diagnostics                       (Intermediate)
The DTCs it can raise (code, meaning, what sets it, what to check), its output channels worth
logging, and what the studio shows while it works.

## Troubleshooting                   (Basic → Advanced)
Symptom → likely causes → checks, as a table.

## Settings reference
The generated table for the module (section 7) — included, never hand-copied.

## Related
Links to the chapters a reader needs next.
```

### 5b. Topic chapter (wiring practice, tuning principles, firmware recovery, …)

Same voice and standards, free structure, but always:
**Overview** (what and why) → **Concepts** (with diagrams) → **Procedure** (numbered steps, one
screenshot or diagram per step that changes something) → **Examples** → **Pitfalls /
troubleshooting** → **Related**.

## 6. Illustrations — the standard

Rich illustration is required, not decoration. **Minimums per chapter:** one diagram explaining the
mechanism, one screenshot per studio page the chapter tells the reader to use, and one annotated
figure for any procedure with more than three steps.

| Kind | How it is made | Where it lives |
|---|---|---|
| **Studio screenshots** | Captured by the studio itself, never by hand: `manual/tools/shoot.sh [--crop GEOM] [--set path=value]... "<page path>" <name>` opens a demo project offline (1600×1000 window), sets any values you give it in the units the studio shows, selects the page and photographs it. **Show the module switched on with the chapter's example values** (`--set boost.enabled=1 ...`); a page of greyed controls documents nothing. Crop to the panel being discussed. Every chapter keeps its captures in a script, `manual/tools/shots/<chapter>.sh`, so all of them can be re-shot after a UI change. | `manual/docs/img/studio/` |
| **Annotated screenshots** | The capture plus numbered callout markers (SVG overlay) keyed to the steps in the text. | `manual/docs/img/studio/` |
| **Diagrams** (wiring, signal flow, trigger wheels, state machines, timing) | Hand-written SVG in the house style below. No raster diagrams. | `manual/docs/img/diagrams/` |
| **Plots** (sensor curves, table shapes, logged traces) | Generated by a script from real data (schema defaults, bench logs) so they can be regenerated. | `manual/docs/img/plots/` |

**House style for diagrams** (copy the `<style>` block of `boost-signal-flow.svg`): a `viewBox` and no
fixed size, transparent background, colours set by classes with a `prefers-color-scheme: dark` block so
the diagram reads in both themes, one sans font, 2 px strokes, the palette blue = what is asked for,
orange = what is driven, green = closed loop, red = safety, wire colours as the real wire (with the colour named in a
label for readers who cannot distinguish it), connector pins labelled with the real connector and pin
number from the board file, signal names in `mono`.

**Every figure** has a number, a caption that says what to look at, and alt text:

```markdown
<figure markdown>
  ![Boost control loop: target, PID, wastegate duty and measured MAP](img/diagrams/boost-loop.svg)
  <figcaption>Figure 21.2 — The boost controller closes the loop on MAP. The feed-forward table
  sets the starting duty; PID trims it.</figcaption>
</figure>
```

## 7. Settings reference — generated

`manual/tools/gen_settings.py` writes one Markdown table per module from `ecu.schema.yaml`: studio
label, schema path, units, range, default, and the schema's own help text. Chapters include it
(`--8<-- "reference/settings/boost.md"`). Authors do **not** copy settings tables by hand. If a
setting's help text in the schema is missing, wrong or unclear, fix it **in the schema** (that also
fixes the studio's tooltip) and note it in the chapter's report.

## 8. Callouts

Material admonitions, used for these meanings only:

| Callout | Use for |
|---|---|
| `!!! tip` | A better way, a shortcut, a rule of thumb from experience. |
| `!!! note` | Background that helps but is not needed to follow along. |
| `!!! example` | A worked example inline. |
| `!!! warning` | Can cost time, a part, or a bad tune. |
| `!!! danger` | Can damage the engine or hurt someone (fuel, fire, runaway throttle, over-boost). |
| `!!! info "Advanced"` | A deeper aside a Basic reader may skip. |

At most one callout per screen of text; a page of callouts is a page nobody reads.

**Check every figure by rendering it** before handing it back: a diagram whose labels overflow their
boxes, or a screenshot of the wrong page, is worse than none. `chromium --headless --screenshot` on the
SVG, or open the built page.

## 9. What an author hands back

With the chapter files, a short report:

- files written and figures made (with their shoot/generate commands);
- **open-questions**: every point the source did not settle, with the file and function looked at;
- **schema fixes**: help texts corrected or added in `ecu.schema.yaml`;
- **bugs found**: anything in the firmware or studio that behaves differently from its own
  documentation or from common sense, with the file and function — reported, not fixed in the chapter.

## 10. Review checklist

A chapter is accepted when:

- [ ] It follows the template for its kind, sections in order, level badges on every section.
- [ ] Every setting of the module is covered (the generated table is included, and every setting a
      reader must set is explained in the walkthrough).
- [ ] Figure minimums met; every figure numbered, captioned, alt-texted, and reproducible.
- [ ] Every factual paragraph has a source note; nothing contradicts the code.
- [ ] Two or more worked examples with real values.
- [ ] Troubleshooting table present.
- [ ] No lineage language ("based on", "like X"); other products named only for interoperability.
- [ ] Builds without warnings (`mkdocs build --strict`).
