#!/usr/bin/env python3
# Re-bench ardustim wheels through the GENERIC trigger decoder. Each wheel is expressed
# as a generic stream config — there is NO crank/cam-specific input config any more, every
# input is a stream and binding is by streams[].capture_index + role. Usage:
#   python3 tools/gen_rebench.py 6 7 9        # test stim indices 6,7,9 from the table
# Requires the generic firmware (config_version >= 11) flashed. No openocd / ST-LINK needed:
# wheels are applied by LIVE reconfigure (no flash burn, no MCU reset) — see main().
import sys, time, struct, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from tools.ts_bench import TsLink, ConfigDict

# ---- config offsets, read from the generated Data Dictionary (shared/tuneit-meta.json) ----
# The dictionary already resolves streams[] to absolute offsets + per-field rel_offsets, so this
# self-adjusts to ANY layout/field change with no schema-structure parsing (that re-derivation is what
# rotted: the schema's Trigger.config_arrays was reshaped and the old code KeyError'd).
#
# Cells are PER STREAM now — a nested `cell` array inside the element, not one shared pool with each
# stream carrying an offset into it. So the cell address is stream base + CELL_REL + 2*k, and there is
# no pool cursor to keep across streams.
def _stream_layout():
    d = ConfigDict()
    streams = d.array('trigger', 'streams')
    foff = {name: f['rel_offset'] for name, f in streams['fields'].items()}
    cell = streams['arrays']['cell']
    return streams['base_offset'], streams['stride'], foff, cell['rel_offset'], cell['stride']
STREAM0, STREAM_STRIDE, F, CELL_REL, CELL_STRIDE = _stream_layout()
# Bench wiring: crank stim → capture pool index 2, cam stim → DIG3 (index 4).
CRANK_CAP, CAM_CAP = 2, 4

# ---- wheel table: stim idx -> generic config ----
# stream dict: rate(0crank/1cam), prim(0gap/1seq/2width), slots, ratio, cell[], wmin,wmax,wtgt
def gap(rate,slots,ratio,cell,repeats=0): return dict(rate=rate,prim=0,slots=slots,ratio=ratio,cell=cell,repeats=repeats)
def seq(rate,cell):             return dict(rate=rate,prim=1,slots=0,ratio=0,cell=cell)
def wid(rate,wmin,wmax,wtgt):   return dict(rate=rate,prim=2,slots=0,ratio=0,cell=[],wmin=wmin,wmax=wmax,wtgt=wtgt)

WHEELS = {
 0:('4cyl dizzy',      [gap(0,2,0,[])],                         None,        'CRANK'),
 1:('6cyl dizzy',      [gap(0,3,0,[])],                         None,        'CRANK'),
 2:('8cyl dizzy',      [gap(0,4,0,[])],                         None,        'CRANK'),
 3:('60-2',            [gap(0,60,3,[0])],                       None,        'CRANK'),
 6:('36-1',            [gap(0,36,2,[0])],                       None,        'CRANK'),
 7:('24-1',            [gap(0,24,2,[0])],                       None,        'CRANK'),
 9:('8-1',             [gap(0,8,2,[0])],                        None,        'CRANK'),
 12:('40-1',           [gap(0,40,2,[0])],                       None,        'CRANK'),
 13:('dizzy 4cyl 50/40',[gap(0,2,0,[])],                        None,        'CRANK'),
 14:('odd-fire 0/135', [seq(0,[1350,2250])],                    None,        'CRANK'),
 16:('12-3',           [gap(0,12,4,[0])],                       None,        'CRANK'),
 17:('36-2-2-2 H4',    [gap(0,36,3,[0,1,14])],                  None,        'CRANK'),
 18:('36-2-2-2 H6',    [gap(0,36,3,[0,19,29])],                 None,        'CRANK'),
 20:('GM 4200',        [seq(0,[500,600,600,100,500,600,700])],  None,        'CRANK'),
 # ---- cam (PHASE) ----  cam edge: WIDTH→Both(2), SEQUENCE→Rising(0)
 4:('60-2 + cam',      [gap(0,60,3,[0]), wid(1,0,7200,0)],      2,           'PHASE'),
 5:('60-2 half-moon',  [gap(0,60,3,[0]), wid(1,0,7200,0)],      2,           'PHASE'),
 8:('4-1 + cam',       [gap(0,4,2,[0]),  wid(1,0,7200,0)],      2,           'PHASE'),
 10:('6-1 + cam',      [gap(0,6,2,[0]),  wid(1,0,7200,0)],      2,           'PHASE'),
 11:('12-1 + cam',     [gap(0,12,2,[0]), wid(1,0,7200,0)],      2,           'PHASE'),
 19:('36-2-2-2 + cam', [gap(0,36,3,[0,1,17]), seq(1,[2100,3000,2100])], 0,   'PHASE'),
 21:('FE3 36-1 + cam', [gap(0,36,2,[0]), seq(1,[3300,300,3600])],        0,   'PHASE'),
 # Nissan 360 CAS — the densest wheel on the rig. An optical disc at CAM speed with two tracks:
 # a 360-slit fine ring on dig1 (180 slits per CRANK revolution, even, no gaps) and 6 window slots
 # on dig3. Both edges of everything is ~452 records in one engine cycle, so it is the case that
 # exercises multi-page paging and the ring's depth.
 #
 # PHASE comes from the WINDOWS, decoded on RISING edges only as a sequence of inter-event angles.
 # Five intervals are ~1276 and one is 800: the wide slot starts sooner, so the current gap is
 # SHORTER than the last — that asymmetry is what lets SequenceMatcher lock absolute rather than
 # relative. Widths are not used; the ratio of consecutive gaps is.
 #
 # NOTE the stim index is 34, NOT the position of THREE_SIXTY_NISSAN_CAS in the wheel_defs.h enum.
 # Ardu-Stim's Wheels[] array is ordered differently from that enum, and index 33 is a Fiat 1.8 16V
 # (60-2 crank + 1 cam) which looks plausible on a scope and syncs at CRANK — an entire session was
 # lost measuring the Nissan against it. Ask the stim: 'L' lists the names in true array order.
 34:('Nissan 360 CAS',  [gap(0,180,0,[]), seq(1,[1276,1276,1276,1296,1276,800])], 0, 'PHASE'),
 22:('6g72',           [gap(0,3,0,[]), seq(1,[1900,1700,1950,1650])],    0,   'PHASE'),
}

import serial
def stim_halt(sp):
    """Stop the wheel DEAD — no edges at all, which 'F' cannot express (fixed_rpm is constrained to a
    100 rpm floor, so F+0 just runs it slowly). Use this to test what the ECU does with no trigger:
    sync loss, cold start before the first tooth, stale captures."""
    sp.reset_input_buffer(); sp.write(b"H\x5A"); time.sleep(0.3)   # magic byte: see comms.cpp
    return sp.readline().decode(errors="replace").strip()


def stim_resume(sp):
    """Restart the pattern from edge 0."""
    sp.reset_input_buffer(); sp.write(b"h\x5A"); time.sleep(0.3)
    return sp.readline().decode(errors="replace").strip()


def open_stim(port=None):
    """Open the ardustim, FINDING it rather than assuming where it is.

    Open ONCE per batch: the port-open DTR-resets the Uno; wait out the bootloader so it's alive, then
    switch wheels over the live port (no per-wheel reset -> no Uno-hang race).

    The port was hard-coded to ttyUSB0, which is only where the stim lands when it is the first USB
    serial device to enumerate. Plug anything else in first and it becomes ttyUSB1, at which point
    every bench script opened whatever WAS on ttyUSB0, sent 'F' into it, and reported that the decoder
    never reached sync — a wiring fault that is not a wiring fault. So each candidate is asked 'N'
    (which wheel are you on) and the one that answers is the stim.
    """
    import glob
    for cand in ([port] if port else sorted(glob.glob('/dev/ttyUSB*'))):
        try:
            sp = serial.Serial(cand, 115200, timeout=1.0)
        except OSError:
            continue
        time.sleep(3.5); sp.reset_input_buffer()
        if port:
            return sp                      # named explicitly: take it on trust
        sp.write(b"N"); time.sleep(0.4)
        # IT MUST ANSWER WITH A WHEEL NUMBER, not merely answer. "Anything non-empty is the stim" picks
        # the first device that happens to be chattering: the slcan CAN adapter sits on ttyUSB0, runs at
        # 2 Mbaud, and read at 115200 returns a screenful of noise — which is non-empty, so the probe
        # took it, `select` went into the CAN adapter, and the stim never moved. A wheel index is a
        # small decimal integer and nothing else is.
        reply = sp.read(64).strip()
        try:
            idx = int(reply)
        except ValueError:
            idx = -1
        if 0 <= idx < 128:
            sp.reset_input_buffer()
            return sp
        sp.close()
    raise SystemExit("no ardustim found on /dev/ttyUSB* — none answered 'N'")
def select(sp, idx, reps=4):
    """Put the stim on wheel `idx` and CONFIRM it took. Returns True if the stim agrees.

    A select is genuinely sometimes lost — right after the port-open DTR reset the Uno is still
    coming up and swallows it — which is why this used to send the byte `reps` times blind, at 0.4 s
    a go. On the sparse wheels reps is 12, so selecting one wheel cost 4.8 seconds of hoping, and
    bench_stim_sweep does that 23 times: most of its five minutes was this.

    The stim can simply be ASKED which wheel it is on ('N'), so hoping is unnecessary. Send, read
    back, stop as soon as it agrees — one round on a healthy link, and still up to `reps` tries when
    a select really is dropped. Faster AND more certain than the blind version, because a lost
    select is now detected rather than merely made unlikely.

    `reps` is kept as the retry budget so existing callers pass it unchanged.
    """
    for _ in range(max(reps, 3)):
        sp.reset_input_buffer(); sp.write(b'S' + bytes([idx])); time.sleep(0.12)
        sp.reset_input_buffer(); sp.write(b'N'); time.sleep(0.12)
        try:
            got = sp.read(sp.in_waiting or 1).decode(errors="replace").strip()
        except Exception:
            got = ""
        if got.isdigit() and int(got) == idx:
            return True
    return False

def crank_teeth(streams):
    """Effective crank tooth count for a wheel — drives how long to wait for lock.
    GAP → slots (the wheel's physical tooth count); SEQUENCE → cell length; even
    dizzy is a GAP with slots = tooth count. Used only to size the lock window."""
    for st in streams:
        if st['rate'] == 0:                       # the crank-rate stream
            if st['prim'] == 1:                   # SEQUENCE (odd-fire / GM4200)
                return max(len(st['cell']), 2)
            return max(st['slots'], len(st['cell']), 2)   # GAP / even-tooth
    return 12                                     # cam-only fallback (shouldn't happen)

def lock_window(teeth):
    """(sample_count, select_reps) scaled by tooth count. A sparse wheel emits few
    teeth per rev, so the gap matcher + PLL need more revolutions — and more (re)stim
    attempts — to lock than a dense 36-1. Empirically a 4-1 needs ~20 s; a 36-1 ~7 s."""
    if teeth >= 12: return 7,  4
    if teeth >= 6:  return 12, 6
    return 30, 12                                 # 4-1 and friends (need a long uninterrupted lock-in)

def configure(l, spec):
    streams, cam_edge, exp = spec[1], spec[2], spec[3]
    def rw(off,p): l.write_raw(off, p)   # absolute offset -> page id + 16-bit page-relative (multi-page)
    # THE STREAM INDEX IS THE ROLE: slots 0-1 are the cranks, 2-5 the cams. There is no stream count
    # and no role field — a slot is live iff `enabled`, so every slot is written explicitly and the
    # unused ones are turned OFF, or a previous wheel's cam would survive under the new one.
    crank_n = cam_n = 0
    used = set()
    for st in streams:
        slot = crank_n if st['rate']==0 else 2 + cam_n
        crank_n += st['rate']==0; cam_n += st['rate']==1
        used.add(slot)
        base = STREAM0 + STREAM_STRIDE*slot
        # capture_index + edge are AUTHORITATIVE (binding is by slot, not crank_*/cam_*):
        # crank-rate → pool idx 2, cam-rate → DIG3 (idx 4). cam WIDTH wants Both(2), else Rising(0).
        rw(base+F['enabled'], bytes([1]))
        # 0 = take the default from the slot; non-zero for a SYMMETRICAL wheel whose
        # pattern recurs more than once per revolution (Renix 4 and 6).
        rw(base+F['repeats'], bytes([st.get('repeats', 0)]))
        # 'cap' lets a stream name its PIN independently of its rate. Normally crank-rate sits on
        # dig1 and cam-rate on dig3, but a Nissan CAS puts the fine 360 track and the variable-width
        # window track on whichever pins the loom happens to give it, and the fine track must stay
        # the velocity source whichever pin it lands on.
        rw(base+F['capture_index'], bytes([st.get('cap', CRANK_CAP if st['rate']==0 else CAM_CAP)]))
        rw(base+F['edge'],          bytes([2 if st['rate']==1 and cam_edge==2 else 0]))
        rw(base+F['primitive'],     bytes([st['prim']]))
        rw(base+F['slots'],         struct.pack('<H', st['slots']))
        rw(base+F['gap_ratio'],     bytes([st['ratio'] if st['ratio'] else 2]))
        rw(base+F['cell_len'],      bytes([len(st['cell'])]))
        rw(base+F['window_pct'],    bytes([25]))
        if st['prim']==2:
            rw(base+F['width_min'],    struct.pack('<h', st['wmin']))
            rw(base+F['width_max'],    struct.pack('<h', st['wmax']))
            rw(base+F['width_target'], struct.pack('<h', st['wtgt']))
        for k, v in enumerate(st['cell']):
            rw(base+CELL_REL+CELL_STRIDE*k, struct.pack('<h', v))
    for s in range(6):
        if s not in used:
            rw(STREAM0 + STREAM_STRIDE*s + F['enabled'], bytes([0]))

def main():
    idxs = [int(x) for x in sys.argv[1:]]
    sp = open_stim()                          # boot the stim ONCE for the whole batch
    lvlmap={0:'NONE',1:'CRANK',2:'PHASE'}
    for idx in idxs:
        spec = WHEELS[idx]
        # ---- LIVE reconfigure: no flash burn, no MCU reset, no openocd. ----
        # configure() 'w'-writes the wheel into g_config RAM; the 'reconfig' text command then
        # forces EnginePositionHal.reconfigure() (stop()+start()) on the firmware's save-task,
        # fully re-initialising the decoder + scheduler + angle clock from the new config. We
        # FORCE it (vs the auto engine-stopped gate) because a decoder still limping on the
        # previous wheel never drops below the rpm gate, so it'd otherwise never apply.
        l = TsLink(); configure(l, spec); l.cmd(b"E", b"reconfig"); l.close()
        teeth = crank_teeth(spec[1])
        samples, reps = lock_window(teeth)
        select(sp, idx, reps)                  # switch the stim to this wheel; the fresh decoder locks it
        l = TsLink(); rpms=[]; lvl=0
        restim_every = max(3, samples // 4)
        for k in range(samples):
            time.sleep(1.0); e=l.telem_all(); rpms.append(e['rpm']); lvl=e['sync_level']
            # re-stim periodically WHILE still unlocked (not just once): the select can be
            # lost and sparse wheels may need several attempts before the stim takes.
            if lvl==0 and k>0 and k % restim_every == 0:
                l.close(); select(sp, idx, reps); l=TsLink()
            # early-exit once we've reached the expected level with a few stable samples —
            # so dense wheels don't sit out the long window reserved for sparse ones.
            if lvlmap.get(lvl)==spec[3] and k>=3 and len([r for r in rpms[-3:] if r>0])>=3:
                break
        l.close()
        good=[r for r in rpms[-3:] if r>0]; avg=sum(good)/len(good) if good else 0
        stable = (max(good)-min(good)) if good else '-'
        ok = (lvlmap.get(lvl)==spec[3])
        print(f"idx{idx:2d} {spec[0]:18s} -> {lvlmap.get(lvl,lvl):5s} (exp {spec[3]}) rpm~{avg:.0f} spread={stable} [{teeth}t/{samples}s] {'PASS' if ok else 'FAIL'}")
    sp.close()

if __name__=='__main__': main()
