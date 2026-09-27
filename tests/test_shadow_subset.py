#!/usr/bin/env python3
"""Shadow generation for PARTIAL modules and the `when` apply policy.

The real schema's Trigger/Engine are all-shadow / all-engine_stop, so this drives
codegen with synthetic modules to prove:
  - partial module → generated subset <Mod>Shadow holding ONLY shadowed members;
  - the engine_stop 'w' watch region covers exactly the engine_stop run;
  - reboot-flagged fields are EXCLUDED from the watch region (a 'w' to one must not
    request a runtime reconfigure) yet still live in the shadow struct;
  - two pulls: <mod>_shadow_pull (FULL, boot) refreshes every shadowed member,
    <mod>_shadow_pull_runtime refreshes engine_stop members only (reboot fields keep
    their boot value);
  - reboot-only module → no watch region at all, struct + pulls still emitted;
  - interleave / bad-when guards fire.
Generated structs/pulls are compiled (-Werror) and run to confirm copy semantics."""

import sys, subprocess, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "codegen"))
import codegen  # noqa: E402

PRIM = {
    "uint8":  {"c_type": "uint8_t",  "size": 1, "ini_type": "U08"},
    "int8":   {"c_type": "int8_t",   "size": 1, "ini_type": "S08"},
    "uint16": {"c_type": "uint16_t", "size": 2, "ini_type": "U16"},
}


def descriptors(modules):
    return {d["mod"]: d for d in codegen.compute_shadow_regions(PRIM, modules)}


def compile_and_run(modules, mod_name, body):
    """Generate the module config header + shadow_meta.h, drop in a main() body, and
    compile+run with g++ -Werror. Returns the program exit code (0 = pass)."""
    cfg_h = codegen.gen_module_config_h(
        PRIM, mod_name,
        modules[mod_name].get("config", []),
        modules[mod_name].get("tables", []),
        modules[mod_name].get("config_arrays", []))
    meta_h = codegen.gen_shadow_meta_h(PRIM, codegen.compute_shadow_regions(PRIM, modules))
    ms = codegen.module_snake(mod_name)
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        (td / "modules").mkdir()
        (td / "modules" / f"{ms}_config.h").write_text(cfg_h)
        (td / "shadow_meta.h").write_text(meta_h)
        src = td / "main.cpp"
        src.write_text('#include "shadow_meta.h"\nint main() {\n' + body + "\n}\n")
        exe = td / "a.out"
        r = subprocess.run(["g++", "-std=c++17", "-Werror", "-I", str(td),
                            str(src), "-o", str(exe)], capture_output=True, text=True)
        assert r.returncode == 0, f"compile failed:\n{r.stderr}"
        return subprocess.run([str(exe)]).returncode, meta_h


# ---- module fixtures -------------------------------------------------------

def mod_partial():
    """Live scalars, then a contiguous engine_stop shadow run (scalar+table+array)."""
    return {
        "System": {"telemetry": [{"name": "uptime", "type": "uint16"}]},
        "Widget": {
            "shadow": {"default": False, "when": "engine_stop"},
            "config": [
                {"name": "live_gain",   "type": "uint16"},
                {"name": "live_offset", "type": "int8"},
                {"name": "n_cells",     "type": "uint8", "shadow": True},
            ],
            "tables": [{"name": "cal", "type": "uint8", "size": 8, "label": "Cal",
                        "shadow": True}],
            "config_arrays": [{"name": "ports", "count": 3, "shadow": True,
                               "element": [{"name": "pin", "type": "uint8"},
                                           {"name": "mode", "type": "uint8"}]}],
        },
    }


def mod_mixed_when():
    """All members shadowed, but a contiguous engine_stop run then a reboot run."""
    return {
        "System": {"telemetry": [{"name": "uptime", "type": "uint16"}]},
        "Widget": {
            "shadow": {"default": True, "when": "engine_stop"},
            "config": [
                {"name": "teeth",    "type": "uint8"},                          # engine_stop (default)
                {"name": "offset",   "type": "uint8"},                          # engine_stop
                {"name": "can_id",   "type": "uint16", "shadow": {"when": "reboot"}},  # reboot
                {"name": "log_mask", "type": "uint8",  "shadow": {"when": "reboot"}},  # reboot
            ],
        },
    }


def mod_reboot_only():
    return {
        "System": {"telemetry": [{"name": "uptime", "type": "uint16"}]},
        "Widget": {
            "shadow": {"default": True, "when": "reboot"},
            "config": [{"name": "can_id", "type": "uint16"},
                       {"name": "node",   "type": "uint8"}],
        },
    }


# ---- tests -----------------------------------------------------------------

def test_partial_region_and_struct():
    m = mod_partial()
    d = descriptors(m)["Widget"]
    assert d["all_shadow"] is False
    assert d["has_reboot"] is False
    lo, hi = d["es_span"]
    # span = n_cells(1) + cal(8) + ports(3*2=6) = 15 bytes
    assert hi - lo == 15, (lo, hi)
    rc, meta = compile_and_run(
        m, "Widget",
        "WidgetConfig live{}; live.n_cells=5; live.cal[0]=42; live.ports[1].pin=9;"
        " live.live_gain=1234;\n"
        "WidgetShadow sh{}; widget_shadow_pull(sh, live);\n"
        "return (sh.n_cells==5 && sh.cal[0]==42 && sh.ports[1].pin==9) ? 0 : 1;")
    assert "struct WidgetShadow {" in meta and "using WidgetShadow" not in meta
    assert "live_gain" not in meta and "live_offset" not in meta
    assert rc == 0


def test_reboot_excluded_from_region_but_in_shadow():
    m = mod_mixed_when()
    d = descriptors(m)["Widget"]
    assert d["all_shadow"] is True       # every member shadowed
    assert d["has_reboot"] is True
    lo, hi = d["es_span"]
    # region covers only teeth(1)+offset(1) = 2 bytes; reboot can_id/log_mask excluded
    assert hi - lo == 2, (lo, hi)
    # FULL pull copies reboot fields; RUNTIME pull leaves them at their boot value.
    rc, meta = compile_and_run(
        m, "Widget",
        "WidgetConfig boot{}; boot.teeth=36; boot.can_id=0x111; boot.log_mask=7;\n"
        "WidgetShadow sh{}; widget_shadow_pull(sh, boot);   // boot: full\n"
        "WidgetConfig live = boot; live.teeth=60; live.can_id=0x222; live.log_mask=9;\n"
        "widget_shadow_pull_runtime(sh, live);              // runtime: engine_stop only\n"
        "// teeth refreshed (60); can_id/log_mask must stay at boot values\n"
        "return (sh.teeth==60 && sh.can_id==0x111 && sh.log_mask==7) ? 0 : 1;")
    # all_shadow → alias; runtime pull is field-wise (engine_stop only)
    assert "using WidgetShadow = WidgetConfig;" in meta
    assert "dst.teeth = live.teeth;" in meta            # runtime field-wise
    assert "dst.can_id" not in meta.split("_runtime")[1] if "_runtime" in meta else True
    assert rc == 0


def test_reboot_only_module_has_no_region():
    m = mod_reboot_only()
    d = descriptors(m)["Widget"]
    assert d["es_span"] is None, "reboot-only module must have no engine_stop watch region"
    assert d["has_reboot"] is True
    rc, meta = compile_and_run(
        m, "Widget",
        "WidgetConfig boot{}; boot.can_id=0x1234; boot.node=7;\n"
        "WidgetShadow sh{}; widget_shadow_pull(sh, boot);\n"
        "WidgetConfig live = boot; live.can_id=0xABCD;\n"
        "widget_shadow_pull_runtime(sh, live);   // no engine_stop members → no-op\n"
        "return (sh.can_id==0x1234 && sh.node==7) ? 0 : 1;")
    # not in the watch table / enum
    assert "JAYECU_SHADOW_WIDGET" not in meta
    assert "JAYECU_SHADOW_MODULE_COUNT = 0" in meta
    assert rc == 0


def test_interleave_guard_fires():
    m = mod_partial()
    m["Widget"]["config"] = [
        {"name": "a_shadow", "type": "uint8", "shadow": True},
        {"name": "b_live",   "type": "uint8"},                    # live, inside the run
        {"name": "c_shadow", "type": "uint8", "shadow": True},
    ]
    m["Widget"]["tables"] = []
    m["Widget"]["config_arrays"] = []
    try:
        codegen.compute_shadow_regions(PRIM, m)
    except SystemExit as e:
        assert "engine_stop watch span" in str(e), str(e)
        return
    raise AssertionError("expected SystemExit for a live field inside the engine_stop span")


def test_reboot_inside_engine_stop_span_rejected():
    """A reboot field BETWEEN two engine_stop fields breaks the watch span → error."""
    m = mod_partial()
    m["Widget"]["config"] = [
        {"name": "es_a", "type": "uint8", "shadow": {"when": "engine_stop"}},
        {"name": "rb",   "type": "uint8", "shadow": {"when": "reboot"}},   # inside ES span
        {"name": "es_b", "type": "uint8", "shadow": {"when": "engine_stop"}},
    ]
    m["Widget"]["tables"] = []
    m["Widget"]["config_arrays"] = []
    try:
        codegen.compute_shadow_regions(PRIM, m)
    except SystemExit as e:
        assert "engine_stop watch span" in str(e), str(e)
        return
    raise AssertionError("expected SystemExit for a reboot field inside the engine_stop span")


def test_bad_when_rejected():
    m = mod_partial()
    m["Widget"]["config"] = [{"name": "x", "type": "uint8", "shadow": {"when": "someday"}}]
    m["Widget"]["tables"] = []
    m["Widget"]["config_arrays"] = []
    try:
        codegen.compute_shadow_regions(PRIM, m)
    except SystemExit as e:
        assert "someday" in str(e), str(e)
        return
    raise AssertionError("expected SystemExit for an unknown shadow when")


def test_allshadow_engine_stop_uses_alias_for_both_pulls():
    """All engine_stop + all shadow → both pulls collapse to `dst = live` (cheap)."""
    m = mod_mixed_when()
    for f in m["Widget"]["config"]:
        f.pop("shadow", None)   # inherit default (engine_stop) for all
    d = descriptors(m)["Widget"]
    assert d["all_shadow"] is True and d["has_reboot"] is False
    meta = codegen.gen_shadow_meta_h(PRIM, codegen.compute_shadow_regions(PRIM, m))
    assert "using WidgetShadow = WidgetConfig;" in meta
    assert "inline void widget_shadow_pull(WidgetShadow& dst, const WidgetConfig& live) noexcept { dst = live; }" in meta
    assert "inline void widget_shadow_pull_runtime(WidgetShadow& dst, const WidgetConfig& live) noexcept { dst = live; }" in meta


if __name__ == "__main__":
    tests = [test_partial_region_and_struct,
             test_reboot_excluded_from_region_but_in_shadow,
             test_reboot_only_module_has_no_region,
             test_interleave_guard_fires,
             test_reboot_inside_engine_stop_span_rejected,
             test_bad_when_rejected,
             test_allshadow_engine_stop_uses_alias_for_both_pulls]
    passed = failed = 0
    for t in tests:
        try:
            t()
            print(f"  PASS  {t.__name__}")
            passed += 1
        except FileNotFoundError:
            print(f"  SKIP  {t.__name__} (g++ not found)")
        except AssertionError as e:
            print(f"  FAIL  {t.__name__}: {e}")
            failed += 1
        except Exception as e:
            print(f"  ERROR {t.__name__}: {e}")
            failed += 1
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(0 if failed == 0 else 1)
