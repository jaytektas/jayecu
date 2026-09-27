# TunerStudio control → jayecu widget mapping

How the TS `[UiDialogs]` components (spec §15.4) map onto jayecu dashboard widgets. The `.ini` importer
targets this table; the studio's own authoring reuses the same widgets. TS is a fallback on-ramp — we
expose a richer set, but every TS control must land on something usable.

## Row-item components

| TS control | jayecu widget | Status | Spec features to honour |
|---|---|---|---|
| `field` (scalar) | `ConfigEditWidget` | exists | label; **units appended in parens**; label `#`prefix = blue, `!`prefix = red; enable/visible = NodeCondition |
| `field` (bit) | `EnumPickerWidget` | exists | dropdown of the bit's options; **caption label** |
| `field` (string) | `ConfigEditWidget` (string mode) | later | free-text entry, length-enforced |
| `displayOnlyField` | `ConfigEditWidget` **readOnly** | add flag | same as field but read-only label, not editable |
| `radio` | **`RadioWidget`** (new) | new | `orientation` vertical/horizontal; bit/enum constant as radio buttons; label |
| `slider` | **`SliderWidget`** (new) | new | scalar constant as horizontal/vertical slider; value shown as label |
| `checkbox` | `CheckboxWidget` (literal tickbox) | done | "Verbiage" label; **flipValue**; bit ↔ 0/1. `ToggleWidget` = switch-styled variant of the same |
| `gauge` | `DialWidget` | exists | round dial; (also `LinearWidget`/`NeedleWidget` = jayecu gauge styles) |
| `runtimeValue` | `ValueWidget` | exists | live OutputChannel readout; label + units |
| `indicator` (in panel) | **`IndicatorWidget`** (new) | new | on/off from a condition/signal; off/on titles + off/on bg+fg colours |
| `array1D` | `CurveWidget` / table | exists* | 1D array editor (Curve preferred per spec) |
| `commandButton` | `CommandButtonWidget` | done | fires a CLI command with two sigil-resolved args. Richer than TS's fire-and-forget: `command_state` reports RUNNING/OK/FAIL, and the command's `@refresh` regions re-read automatically (no close-and-reopen). In use for ETB `findlimits` / `fillff` / `autotune` and `pedalcal`. |
| `settingSelector` | **`SettingSelectorWidget`** | deferred | preset dropdown that writes N constants (needs preset-set model) |
| `liveGraph` | **`LiveGraphWidget`** | deferred | multi-line time-series graph |
| `logFieldSelector`, `canDeviceSelector`, `canClientIdSelector`, `channelSelector`, `userPassword` | — | skip | MegaSquirt-specific; not applicable to jayecu |

## Container / layout components

| TS control | jayecu | Status |
|---|---|---|
| `dialog` / `panel` | `PanelWidget` (Free + yAxis/xAxis/border/card/indexCard) | done |
| `indicatorPanel` | `PanelWidget` (grid) of `IndicatorWidget` | via Panel |
| `readoutPanel` | `PanelWidget` of `ValueWidget` | via Panel |
| Table Editor | `TableWidget` | exists |
| Curve Editor | `CurveWidget` | exists |

## Cross-cutting rules (apply to all field-like controls)
- **Label**: every field-like control carries a caption. `#`text = blue, `!`text = red (a leading marker
  on the label string). Units (from the bound config's display unit) are appended in parens: `Label(deg)`.
- **enable / visible**: already universal via `NodeCondition` (`enableCondition` / `visibleCondition`).
- **Natural size**: every widget needs a `defaultElementSize()` entry so panels lay it out at a sane size.
