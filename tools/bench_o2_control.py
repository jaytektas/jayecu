#!/usr/bin/env python3
"""O2 CONTROL on the real ECU: which sensor drives the loop, and what the two banks do.

Everything here runs with the engine STOPPED, deliberately — the fast loop needs a readable sensor and
nothing else, so all of it is reachable on a bench with signals injected. What is NOT here is LEARNING:
the learn gate requires a running engine (see bench_ltft.py), and no amount of injection substitutes
for teeth on the trigger input.

The claims, none of which a static read can settle:

  a role is INDIRECTION   O2 Control names a job ("Wideband Overall"), the wideband list says which
                          sensor holds it. Move the job to another sensor and the loop must follow
                          without the control page being touched — that is the whole point of naming
                          the job instead of the wire.
  a name is not a role    "Wideband 2" names one sensor outright and ignores the assignment entirely.
  one job, one sensor     two enabled widebands claiming a role is a configuration error the ECU
                          catches on a config change (o2_assign_fault).
  a refused pair is HELD  a source resolving to nothing, a banked pair of two kinds, or the same
                          sensor on both banks: P1754, and the loop must correct NOTHING while it holds
                          — a fault that does not change behaviour is a warning light wired to nothing.
  two banks telescope     banked, each cylinder gets its bank's whole answer as (common + deviation),
                          so the shared part rides once in the global term and the bank terms carry
                          only the disagreement. Unbanked collapses to exactly one loop, bank terms 0.

  python3 tools/bench_o2_control.py
"""
import struct, sys, time

sys.path.insert(0, ".")
from tools.ts_bench import TsLink, Meta

TTL_MS  = 500
IDX_WB1, IDX_WB2, IDX_NB1, IDX_NB2 = 1, 2, 52, 53
# O2 Control option ordinals: 3 is this selector's own bank, 4.. names a wideband, 19 is overall.
SRC_OFF, SRC_NB1, SRC_NB2, SRC_OWN_BANK, SRC_WB1, SRC_WB2, SRC_OVERALL = 0, 1, 2, 3, 4, 5, 19
# Wideband list assignment: 0 Unassigned, 1 Overall, 2..3 Bank 1..2, 4.. Cylinder 1..12
A_NONE, A_OVERALL, A_BANK1, A_BANK2 = 0, 1, 2, 3


def lua(vals):
    body = "\n".join(f'  signalWrite("{k}", {v}, {TTL_MS})' for k, v in vals.items())
    return f"function onTick()\n{body}\nend\nsetTickRate(200)\n"


def main():
    M, L = Meta(), TsLink()
    print(L.hello())
    fails = []

    def check(ok, what, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {what}" + (f"   — {detail}" if detail else ""))
        if not ok:
            fails.append(what)

    def tel(c):
        return L.telem(c) * M.t(c)["scale"]

    S = M.array("sensors", "sensor")
    def sensor_en(i, on):
        L.write_raw(S["base_offset"] + i * S["stride"] + S["fields"]["enabled"]["rel_offset"],
                    bytes([1 if on else 0]))
    WB = M.array("lambda", "wb")
    def assign(k, job):
        L.write_raw(WB["base_offset"] + k * WB["stride"] + WB["fields"]["assign"]["rel_offset"],
                    bytes([job]))

    # Gains flat and gentle: this bench is about WHICH sensor and WHERE the answer goes, not about
    # loop dynamics, and a shipped gain saturates the 20 % authority in a tenth of a second.
    def flat(table, pct):
        t = M.config["lambda"][table]
        n = t.get("cols_max", 8) * t.get("rows_max", 8)
        saved = L.read_config_chunked(t["offset"], n * 2, chunk=200)
        L.write_raw(t["offset"], struct.pack("<" + "H" * n,
                                             *([int(round(pct / t["scale"]))] * n)))
        return t["offset"], saved
    kp_off, kp_saved = flat("stft_kp_table", 2.0)
    ki_off, ki_saved = flat("stft_ki_table", 4.0)

    saved = {n: L.get_config(n) for n in ("lambda_enabled", "lambda_ltft_enabled",
                                          "lambda_o2_src_1", "lambda_o2_src_2")}

    # WAIT FOR A CONDITION, not for a value to land in a window. The first version of this waited for
    # fuel_corr_stft to come within 1.0 of 0.0 — which is true the instant you read it, since the
    # correction lives around 1.0 — so it returned immediately and caught the loop mid-transit while
    # the integrator was still unwinding from lean to rich, and read the crossing as the answer.
    def wait_until(pred, secs=20.0):
        t0 = time.time()
        while time.time() - t0 < secs:
            if pred():
                return True
            time.sleep(0.02)
        return False

    # THERE IS NO SETTLED VALUE HERE, so do not write a wait that pretends there is. With a constant
    # lambda error the STFT integrator ramps until it hits the authority clamp; it does not converge
    # on anything in the few seconds these sections allow. The sleeps this file used to carry were
    # therefore sampling a RAMP at a fixed time, and the numbers they produced (x1.0100, x1.0110,
    # bank +1.59 %, x1.0340) are "how far it got in N seconds", not equilibrium.
    #
    # That matters for how they are replaced. Polling for the bare assertion — corr > 1.0 — returns on
    # the first frame that crosses and reports x1.0020, which is the same CLAIM with a fifth of the
    # margin: still true, but no longer evidence of a loop that is really driving. So each wait below
    # polls for a magnitude of the same order as the constant it replaces, with that constant as the
    # timeout. Fast when the ECU is quick, and never quietly weaker.
    #
    # (A settle detector was tried here first and was worse than useless: a slow ramp moves less than
    # any sensible epsilon between samples, so it declared "settled" almost immediately and handed
    # back a value a seventh of the reference.)
    MOVED = 0.005      # x1.005 — half the reference deflection, far above the exact-1.0000 idle
    BANK  = 0.5        # %     — likewise against a reference of 1.59 %

    def reset_rig():
        for i in (IDX_WB1, IDX_WB2, IDX_NB1, IDX_NB2):
            sensor_en(i, False)
        for k in range(15):
            assign(k, A_NONE)
        L.set_config("lambda_o2_src_1", SRC_OFF)
        L.set_config("lambda_o2_src_2", SRC_OFF)
        L.set_config("lambda_ltft_enabled", 0)     # the surface is not what this bench is about
        L.set_config("lambda_enabled", 1)

    # ---------------------------------------------------------------- a role is indirection
    print("\n-- a role names the JOB; the list says which sensor holds it --")
    reset_rig()
    sensor_en(IDX_WB1, True); sensor_en(IDX_WB2, True)
    assign(0, A_OVERALL)                                   # sensor 1 is Overall
    L.set_config("lambda_o2_src_1", SRC_OVERALL)
    # sensor 1 lean, sensor 2 rich: the sign of the correction says which one is being heard
    L.set_script(lua({"lambda_1": 1.08, "lambda_2": 0.94}))
    wait_until(lambda: tel("fuel_corr_stft") >= 1.0 + MOVED, 2.5)
    a = tel("fuel_corr_stft")
    check(a > 1.0, "Overall follows the sensor holding that job (sensor 1, lean -> adds fuel)",
          f"x{a:.4f}")

    # MOVE THE JOB. Nothing on O2 Control changes; the loop must follow the assignment.
    assign(0, A_NONE); assign(1, A_OVERALL)                # sensor 2 is Overall now
    # The integrator has to unwind from the lean answer, through zero, to the rich one — so this is a
    # condition to wait for, not a value to sample at an arbitrary moment.
    got = wait_until(lambda: tel("fuel_corr_stft") < 0.999, 25.0)
    b = tel("fuel_corr_stft")
    check(got, "…move the job to sensor 2 and the loop follows, control page untouched",
          f"x{b:.4f} (rich -> takes fuel)")

    # ---------------------------------------------------------------- a name is not a role
    print("\n-- naming a sensor outright ignores the assignment --")
    L.set_config("lambda_o2_src_1", SRC_WB1)               # sensor 1, whatever job it holds
    wait_until(lambda: tel("fuel_corr_stft") >= 1.0 + MOVED, 3.0)
    c = tel("fuel_corr_stft")
    check(c > 1.0, "Wideband 1 follows sensor 1 though sensor 2 holds Overall", f"x{c:.4f}")

    # ---------------------------------------------------------------- one job, one sensor
    print("\n-- one job belongs to one sensor --")
    assign(0, A_OVERALL); assign(1, A_OVERALL)             # both claim Overall
    wait_until(lambda: tel("o2_assign_fault") == 1, 1.5)
    check(tel("o2_assign_fault") == 1, "two sensors claiming Overall raises the clash")
    assign(0, A_NONE)
    wait_until(lambda: tel("o2_assign_fault") == 0, 1.5)
    check(tel("o2_assign_fault") == 0, "…and it clears when one lets go")

    # ---------------------------------------------------------------- refused pairs are HELD OFF
    print("\n-- a refused configuration corrects nothing --")
    sensor_en(IDX_NB1, True)
    for name, s1, s2 in (("a source that resolves to nothing", SRC_OWN_BANK, SRC_OFF),
                         ("a banked pair of two KINDS",        SRC_NB1,      SRC_WB2),
                         ("the same sensor on both banks",     SRC_WB1,      SRC_WB1)):
        assign(0, A_NONE); assign(1, A_NONE)
        L.set_config("lambda_o2_src_1", s1); L.set_config("lambda_o2_src_2", s2)
        wait_until(lambda: tel("o2_src_fault") == 1
                           and abs(tel("fuel_corr_stft") - 1.0) < 0.001, 2.5)
        f, corr = tel("o2_src_fault"), tel("fuel_corr_stft")
        check(f == 1 and abs(corr - 1.0) < 0.001, name,
              f"fault={f:.0f} correction=x{corr:.4f}")

    # ---------------------------------------------------------------- two banks telescope
    print("\n-- banked: the global carries the agreement, the banks carry the difference --")
    assign(0, A_NONE); assign(1, A_NONE)
    L.set_config("lambda_o2_src_1", SRC_WB1)
    L.set_config("lambda_o2_src_2", SRC_WB2)
    L.set_script(lua({"lambda_1": 1.08, "lambda_2": 0.96}))
    wait_until(lambda: tel("stft_bank_1_pct") >= BANK and tel("stft_bank_2_pct") <= -BANK, 6.0)
    common = tel("fuel_corr_stft") - 1.0
    d1, d2 = tel("stft_bank_1_pct"), tel("stft_bank_2_pct")
    print(f"    common {common*100:+.2f}%   bank1 {d1:+.2f}%   bank2 {d2:+.2f}%")
    check(tel("o2_src_fault") == 0, "two widebands, one per bank, is a valid pair")
    check(d1 > 0.0 and d2 < 0.0, "the lean bank asks for fuel, the rich bank gives it back")
    check(abs(d1 + d2) < 0.2, "…and the deviations are equal and opposite about the common part",
          f"sum {d1 + d2:+.3f}%")

    print("\n-- unbanked collapses to one loop --")
    L.set_config("lambda_o2_src_2", SRC_OFF)
    wait_until(lambda: tel("stft_bank_1_pct") == 0.0 and tel("stft_bank_2_pct") == 0.0
                       and tel("fuel_corr_stft") >= 1.0 + MOVED, 4.0)
    u1, u2, uc = tel("stft_bank_1_pct"), tel("stft_bank_2_pct"), tel("fuel_corr_stft")
    check(u1 == 0.0 and u2 == 0.0, "bank terms are exactly zero", f"{u1:+.2f}% / {u2:+.2f}%")
    check(uc > 1.0, "…and the one loop follows bank 1's sensor, ignoring the other", f"x{uc:.4f}")

    # ---- leave the rig as found ----
    L.write_raw(kp_off, kp_saved); L.write_raw(ki_off, ki_saved)
    for i in (IDX_WB1, IDX_WB2, IDX_NB1, IDX_NB2):
        sensor_en(i, False)
    for k in range(15):
        assign(k, A_NONE)
    for n, v in saved.items():
        L.set_config(n, v)
    L.restore_script()

    print(f"\n{'ALL PASSED' if not fails else str(len(fails)) + ' FAILED: ' + ', '.join(fails)}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
