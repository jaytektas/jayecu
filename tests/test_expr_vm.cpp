#include "test_helpers.h"
#include "../firmware/Signal/Expr.h"
#include "../firmware/Signal/SignalBus.h"
#include "../generated/signal_ids.h"
#include <vector>

using namespace expr;

// ---------------------------------------------------------------------------
// A minimal assembler, so a test reads like the source it would be compiled
// from. This is NOT the studio compiler — it is deliberately dumb, and emits
// exactly what it is told to, including nonsense, so the validator can be
// tested against programs a real compiler would never produce.
// ---------------------------------------------------------------------------
struct Asm {
    std::vector<uint8_t> b;

    Asm& op(Op o) { b.push_back((uint8_t)o); return *this; }
    Asm& u8(uint8_t v) { b.push_back(v); return *this; }
    Asm& u16(uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); return *this; }
    Asm& u24(uint32_t v) { b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF);
                           b.push_back((v >> 16) & 0xFF); return *this; }
    Asm& i32(int32_t v) { uint32_t u = (uint32_t)v;
                          for (int k = 0; k < 4; k++) b.push_back((u >> (8 * k)) & 0xFF);
                          return *this; }

    Asm& sig(SignalId s) { return op(OP_PUSH_SIG).u16((uint16_t)(s + 1)); }   // options_from:signals
    Asm& sig_raw(uint16_t sel) { return op(OP_PUSH_SIG).u16(sel); }
    Asm& age(SignalId s) { return op(OP_AGE).u16((uint16_t)(s + 1)); }
    Asm& konst(float v) { return op(OP_PUSH_F).i32((int32_t)(v * 100.0f)); }  // x100 literal
    Asm& i8v(int8_t v) { return op(OP_PUSH_I8).u8((uint8_t)v); }
    Asm& i16v(int16_t v) { return op(OP_PUSH_I16).u16((uint16_t)v); }
    Asm& cfg(uint32_t off, CfgType t) { return op(OP_PUSH_CFG).u24(off).u8((uint8_t)t); }
    Asm& bcfg(uint32_t off, CfgType t, uint8_t lo, uint8_t width) {
        return op(OP_PUSH_BCFG).u24(off).u8((uint8_t)t)
                 .u8((uint8_t)((lo & 0x1F) | (((width - 1) & 0x07) << 5)));
    }
    Asm& interp(uint16_t id) { return op(OP_INTERP).u16(id); }
    Asm& table(uint16_t id) { return op(OP_TABLE).u16(id); }
    Asm& end() { return op(OP_END); }

    const uint8_t* p() const { return b.data(); }
    uint16_t len() const { return (uint16_t)b.size(); }
};

// A fake config image — the VM only ever sees bytes and offsets, so a real
// EcuConfig is not needed to prove addressing works.
//
// Deliberately HEAP allocated rather than a global array: an out-of-range read
// from a global lands in whatever global sits next to it, which a sanitizer
// cannot distinguish from a legitimate access. On the heap it hits a redzone
// and ASan names it. That is what makes the VM's runtime range check testable
// rather than merely present.
static constexpr uint32_t CFG_SIZE = 64;
static std::vector<uint8_t> g_cfg_store(CFG_SIZE);
static uint8_t* const g_cfg = g_cfg_store.data();

static Ctx make_ctx(const SignalBus& bus, uint32_t now_ms = 1000) {
    Ctx c;
    c.bus = &bus;
    c.cfg = g_cfg;
    c.cfg_size = CFG_SIZE;
    c.now_ms = now_ms;
    return c;
}

static Invalid val(const Asm& a) { return validate(a.p(), a.len(), CFG_SIZE, 0); }
static Result  run(const Asm& a, const Ctx& c) { return exec(a.p(), a.len(), c); }

int main() {
    fprintf(stdout, "=== Expression VM (pure stack evaluator) ===\n");

    SECTION("arithmetic and precedence — the thing the four flat slots could not do");
    {
        SignalBus bus;
        bus.set(SIG_RPM, 3000.0f, true, 1000);
        bus.set(SIG_MAP, 40.0f,   true, 1000);
        bus.set(SIG_TPS, 85.0f,   true, 1000);
        Ctx c = make_ctx(bus);

        // rpm > 2500 AND (map > 50 OR tps > 80)  — the doc's worked example.
        Asm a; a.sig(SIG_RPM).konst(2500).op(OP_GT)
                .sig(SIG_MAP).konst(50).op(OP_GT)
                .sig(SIG_TPS).konst(80).op(OP_GT)
                .op(OP_OR).op(OP_AND).end();
        CHECK(val(a) == Invalid::None);
        CHECK(run(a, c).truthy());                       // map fails, tps carries the OR

        // The SAME tests with the other grouping must differ — that is the
        // whole point of having precedence: (rpm>2500 AND map>50) OR tps>80.
        bus.set(SIG_TPS, 10.0f, true, 1000);             // now nothing carries it
        CHECK(!run(a, c).truthy());
    }

    SECTION("literals: x100 float, small int, zero/one");
    {
        SignalBus bus; Ctx c = make_ctx(bus);
        Asm a; a.konst(12.34f).konst(12.34f).op(OP_EQ).end();
        CHECK(run(a, c).truthy());

        Asm b; b.i16v(2500).konst(25.0f).op(OP_GT).end();    // ints are UNSCALED
        CHECK(run(b, c).truthy());                           // 2500 > 25

        Asm z; z.op(OP_PUSH_ZERO).op(OP_PUSH_ONE).op(OP_LT).end();
        CHECK(run(z, c).truthy());

        Asm n; n.i8v(-5).op(OP_ABS).konst(5).op(OP_EQ).end();
        CHECK(run(n, c).truthy());
    }

    SECTION("config addressing — whole fields, every type, unaligned offsets");
    {
        SignalBus bus; Ctx c = make_ctx(bus);
        memset(g_cfg, 0, CFG_SIZE);

        // Deliberately land a u16 and a float on ODD offsets: the config image is
        // #pragma pack(1), so this is the normal case, not an exotic one.
        const uint16_t v16 = 4000;  memcpy(g_cfg + 3, &v16, 2);
        const float    vf  = 87.5f; memcpy(g_cfg + 9, &vf, 4);
        const int8_t   vs8 = -12;   memcpy(g_cfg + 1, &vs8, 1);
        const int32_t  v32 = -70000; memcpy(g_cfg + 20, &v32, 4);

        Asm a; a.cfg(3, CT_U16).i16v(4000).op(OP_EQ).end();
        CHECK(val(a) == Invalid::None);
        CHECK(run(a, c).truthy());

        Asm b; b.cfg(9, CT_F32).konst(87.5f).op(OP_EQ).end();
        CHECK(run(b, c).truthy());

        Asm s; s.cfg(1, CT_S8).konst(-12).op(OP_EQ).end();
        CHECK(run(s, c).truthy());                       // signed, not 244

        Asm w; w.cfg(20, CT_S32).konst(-70000).op(OP_EQ).end();
        CHECK(run(w, c).truthy());
    }

    SECTION("config addressing — packed bit groups (the DTC/diagnostics case)");
    {
        SignalBus bus; Ctx c = make_ctx(bus);
        memset(g_cfg, 0, CFG_SIZE);
        // One byte holding: bit0 = a flag, bits 2..3 = a 2-bit severity (value 3).
        g_cfg[5] = (uint8_t)(0x01 | (3u << 2));

        Asm flag; flag.bcfg(5, CT_U8, 0, 1).end();
        CHECK(val(flag) == Invalid::None);
        CHECK(run(flag, c).truthy());                    // bit 0 set

        Asm sev; sev.bcfg(5, CT_U8, 2, 2).i8v(3).op(OP_EQ).end();
        CHECK(run(sev, c).truthy());                     // two-bit group reads 3

        Asm clear; clear.bcfg(5, CT_U8, 1, 1).end();
        CHECK(!run(clear, c).truthy());                  // bit 1 clear

        // A bit group inside a SIGNED byte must extract from the raw pattern,
        // not from a sign-extended value.
        g_cfg[6] = 0x80;                                 // top bit of an s8 = -128
        Asm top; top.bcfg(6, CT_S8, 7, 1).end();
        CHECK(run(top, c).truthy());

        // A 32-bit field whose value cannot survive a float round trip. 0xABCDEF12
        // is 2882400018 — a float holds ~24 bits of mantissa, so going through
        // one rounds the low byte away and every group below bit 8 reads wrong.
        const uint32_t big = 0xABCDEF12u; memcpy(g_cfg + 11, &big, 4);
        Asm lo8; lo8.bcfg(11, CT_U32, 0, 8).i16v(0x12).op(OP_EQ).end();
        CHECK(val(lo8) == Invalid::None);
        CHECK(run(lo8, c).truthy());                     // 0x12, not a rounded neighbour
        Asm mid; mid.bcfg(11, CT_U32, 8, 8).i16v(0xEF).op(OP_EQ).end();
        CHECK(run(mid, c).truthy());
        // ...and the same field read WHOLE still goes through float, which is
        // fine because a comparison against a >2^24 threshold is approximate on
        // both sides. Pinned so the difference is deliberate, not accidental.
        Asm whole; whole.cfg(11, CT_U32).op(OP_PUSH_ZERO).op(OP_GT).end();
        CHECK(run(whole, c).truthy());
    }

    SECTION("validity: fail-to-false per operand, and OR can still rescue");
    {
        SignalBus bus;
        bus.set(SIG_TPS, 85.0f, true, 1000);             // TPS live, MAP never written
        Ctx c = make_ctx(bus);

        Asm dead; dead.sig(SIG_MAP).konst(50).op(OP_GT).end();
        const Result r = run(dead, c);
        CHECK(r.ran);                                    // it executed...
        CHECK(!r.v.ok);                                  // ...but the answer is untrustworthy
        CHECK(!r.truthy());                              // and a gate reads it as false

        // OR rescues: dead MAP, live TPS over threshold -> armed. This is why
        // validity is per-operand rather than a whole-program flag.
        Asm rescue; rescue.sig(SIG_MAP).konst(50).op(OP_GT)
                          .sig(SIG_TPS).konst(80).op(OP_GT).op(OP_OR).end();
        CHECK(run(rescue, c).truthy());

        // AND cannot rescue.
        Asm both; both.sig(SIG_MAP).konst(50).op(OP_GT)
                      .sig(SIG_TPS).konst(80).op(OP_GT).op(OP_AND).end();
        CHECK(!run(both, c).truthy());

        // NOT must NOT invert an untrustworthy value into TRUE — the one trap
        // that would catch someone writing a safety gate. Pinned on the FLAG,
        // not just on truthy(): the flag is what AND/OR and the site all read,
        // so a NOT that cleared it would leak a true through any of them.
        Asm inv; inv.sig(SIG_MAP).konst(50).op(OP_GT).op(OP_NOT).end();
        const Result ri = run(inv, c);
        CHECK(ri.ran && !ri.v.ok);                       // still untrustworthy after NOT
        CHECK(!ri.truthy());
        // ...and it cannot be laundered into a true by a following OR either.
        Asm launder; launder.sig(SIG_MAP).konst(50).op(OP_GT).op(OP_NOT)
                            .op(OP_PUSH_ZERO).op(OP_OR).end();
        CHECK(!run(launder, c).truthy());
        // ...while NOT of a trustworthy false is genuinely true.
        Asm inv2; inv2.sig(SIG_TPS).konst(200).op(OP_GT).op(OP_NOT).end();
        CHECK(run(inv2, c).truthy());
    }

    SECTION("validity: a stale channel is as untrustworthy as a missing one");
    {
        SignalBus bus;
        bus.set(SIG_MAP, 90.0f, true, 1000, /*ttl_ms=*/50);
        Ctx c = make_ctx(bus, 1000);
        Asm a; a.sig(SIG_MAP).konst(50).op(OP_GT).end();
        CHECK(run(a, c).truthy());                       // fresh

        bus.expire_stale(2000);                          // ttl 50 ms, now 1000 ms later
        Ctx c2 = make_ctx(bus, 2000);
        CHECK(!run(a, c2).truthy());                     // stale -> fails to false
    }

    SECTION("validity: arithmetic propagates, divide-by-zero is untrustworthy");
    {
        SignalBus bus;
        bus.set(SIG_RPM, 3000.0f, true, 1000);
        Ctx c = make_ctx(bus);

        Asm a; a.sig(SIG_MAP).sig(SIG_RPM).op(OP_ADD).konst(0).op(OP_GT).end();
        CHECK(!run(a, c).truthy());                      // junk + live = junk

        Asm d; d.sig(SIG_RPM).op(OP_PUSH_ZERO).op(OP_DIV).konst(0).op(OP_GE).end();
        const Result r = run(d, c);
        CHECK(r.ran && !r.v.ok);                         // not an inf, not a trap value
        CHECK(!r.truthy());
    }

    SECTION("helpers: MIN/MAX/CLAMP/SELECT/BIT");
    {
        SignalBus bus;
        bus.set(SIG_RPM, 3000.0f, true, 1000);
        Ctx c = make_ctx(bus);

        Asm mn; mn.konst(10).konst(4).op(OP_MIN).konst(4).op(OP_EQ).end();
        CHECK(run(mn, c).truthy());
        Asm mx; mx.konst(10).konst(4).op(OP_MAX).konst(10).op(OP_EQ).end();
        CHECK(run(mx, c).truthy());
        Asm cl; cl.sig(SIG_RPM).konst(0).konst(2000).op(OP_CLAMP).konst(2000).op(OP_EQ).end();
        CHECK(val(cl) == Invalid::None);
        CHECK(run(cl, c).truthy());

        // SELECT: c ? a : b
        Asm se; se.op(OP_PUSH_ONE).konst(7).konst(9).op(OP_SELECT).konst(7).op(OP_EQ).end();
        CHECK(run(se, c).truthy());
        Asm se2; se2.op(OP_PUSH_ZERO).konst(7).konst(9).op(OP_SELECT).konst(9).op(OP_EQ).end();
        CHECK(run(se2, c).truthy());
        // An untrustworthy CONDITION takes the false branch rather than poisoning.
        Asm se3; se3.sig(SIG_MAP).konst(7).konst(9).op(OP_SELECT).konst(9).op(OP_EQ).end();
        CHECK(run(se3, c).truthy());

        // BIT — status words compare as words today.
        Asm bt; bt.i16v(0b1010).i8v(3).op(OP_BIT).end();
        CHECK(run(bt, c).truthy());
        Asm bt2; bt2.i16v(0b1010).i8v(2).op(OP_BIT).end();
        CHECK(!run(bt2, c).truthy());
        Asm bt3; bt3.i16v(1).i8v(40).op(OP_BIT).end();   // out of range
        CHECK(!run(bt3, c).truthy());
    }

    SECTION("AGE — a pure function of the passed-in clock, not a read of one");
    {
        SignalBus bus;
        bus.set(SIG_MAP, 90.0f, true, /*now=*/1000);
        Asm a; a.age(SIG_MAP).i16v(500).op(OP_GT).end();
        CHECK(val(a) == Invalid::None);
        CHECK(!run(a, make_ctx(bus, 1200)).truthy());    // 200 ms old
        CHECK( run(a, make_ctx(bus, 2000)).truthy());    // 1000 ms old
    }

    SECTION("INTERP — routed through the caller's hook, ids checked at validate");
    {
        SignalBus bus;
        bus.set(SIG_RPM, 3000.0f, true, 1000);
        Ctx c = make_ctx(bus);
        c.curve_count = 2;
        c.curve = [](uint16_t id, float x, float& out, void*) {
            if (id != 1) return false;
            out = x / 100.0f;                            // "boost target" curve
            return true;
        };

        Asm a; a.sig(SIG_RPM).interp(1).konst(25).op(OP_GT).end();
        CHECK(validate(a.p(), a.len(), CFG_SIZE, c.curve_count) == Invalid::None);
        CHECK(run(a, c).truthy());                       // 3000/100 = 30 > 25

        // A hook that refuses makes the value untrustworthy, not zero-and-armed.
        Asm b; b.sig(SIG_RPM).interp(0).konst(-1).op(OP_GT).end();
        CHECK(!run(b, c).truthy());

        // An id the site never offered is rejected before it can run.
        Asm bad; bad.sig(SIG_RPM).interp(5).end();
        CHECK(validate(bad.p(), bad.len(), CFG_SIZE, c.curve_count) == Invalid::BadCurve);
    }

    SECTION("TABLE — the tune's own map, read at its own axes");
    {
        // The difference from INTERP is the whole point: a table knows where its coordinates come
        // from, so the program supplies none. Nothing is pushed in, one value comes out.
        SignalBus bus; Ctx c = make_ctx(bus);
        c.curve_count = 3;                                    // one registry, both opcodes
        c.table = [](uint16_t id, float& out, void*) {
            if (id != 2) return false;
            out = 42.0f;                                      // "what the fan duty map says right now"
            return true;
        };
        Asm a; a.table(2).konst(40).op(OP_GT).end();
        CHECK(validate(a.p(), a.len(), CFG_SIZE, c.curve_count) == Invalid::None);
        CHECK(run(a, c).truthy());                            // 42 > 40

        // A table that cannot be read is UNTRUSTWORTHY, not zero: a gate must not fire because a
        // lookup failed, and it must not silently read as "nothing".
        Asm b; b.table(1).konst(-1).op(OP_GT).end();
        CHECK(!run(b, c).truthy());

        // An id the site never offered is refused before it runs, exactly like a curve id.
        Asm bad; bad.table(9).end();
        CHECK(validate(bad.p(), bad.len(), CFG_SIZE, c.curve_count) == Invalid::BadCurve);

        // It PUSHES — a program may compare two tables, or a table against a channel.
        Asm two; two.table(2).table(2).op(OP_EQ).end();
        CHECK(validate(two.p(), two.len(), CFG_SIZE, c.curve_count) == Invalid::None);
        CHECK(run(two, c).truthy());
    }

    SECTION("the shipped output templates, as the ECU will actually run them");
    {
        // THE FUEL PUMP, in bytecode: "uptime_s < prime_s OR age(trigger_teeth) < stop_ms". Prime for
        // three seconds at key-on, then run only while teeth are arriving — and stop 1.5 s after the
        // last one. This is the condition a wizard writes, evaluated by the VM the firmware runs.
        SignalBus bus;
        Ctx c = make_ctx(bus);
        c.now_ms = 1000;
        bus.set(SIG_UPTIME_S, 1.0f, true, c.now_ms);          // one second after key-on
        Asm pump;
        pump.sig(SIG_UPTIME_S).konst(3).op(OP_LT)             // uptime_s < 3
            .age(SIG_TRIGGER_TEETH).konst(1500).op(OP_LT)     // age(trigger_teeth) < 1500
            .op(OP_OR).end();
        CHECK(validate(pump.p(), pump.len(), CFG_SIZE, 0) == Invalid::None);
        CHECK(run(pump, c).truthy());                          // priming

        // Priming over, and no tooth has ever arrived: the pump must be OFF. age() of a channel that
        // was never written is enormous, which is exactly the answer wanted.
        bus.set(SIG_UPTIME_S, 30.0f, true, c.now_ms);
        c.now_ms = 30000;
        CHECK(!run(pump, c).truthy());

        // The engine is turning: a tooth just landed, so the pump runs whatever the uptime says.
        bus.set(SIG_TRIGGER_TEETH, 120.0f, true, c.now_ms);
        CHECK(run(pump, c).truthy());

        // …and two seconds after the last tooth, it stops.
        c.now_ms = 32000;
        CHECK(!run(pump, c).truthy());

        // THE THERMO FAN: "clt > on_c" / "clt < off_c" — the two halves of a deadband, which is what
        // makes the output latch between them rather than chatter at one threshold.
        Asm fan_on;  fan_on .sig(SIG_CLT).konst(95).op(OP_GT).end();
        Asm fan_off; fan_off.sig(SIG_CLT).konst(90).op(OP_LT).end();
        bus.set(SIG_CLT, 98.0f, true, c.now_ms);
        CHECK(run(fan_on,  c).truthy());
        CHECK(!run(fan_off, c).truthy());
        bus.set(SIG_CLT, 92.0f, true, c.now_ms);              // between the two: neither asks
        CHECK(!run(fan_on,  c).truthy());
        CHECK(!run(fan_off, c).truthy());                     // the latch is what holds it on here
        bus.set(SIG_CLT, 85.0f, true, c.now_ms);
        CHECK(run(fan_off, c).truthy());
    }

    SECTION("empty program = always armed (the common case: no expression)");
    {
        SignalBus bus; Ctx c = make_ctx(bus);
        uint8_t blank[PROGRAM_MAX] = {0};
        CHECK(is_empty(blank, sizeof(blank)));
        CHECK(eval_bool(blank, sizeof(blank), c, /*on_invalid=*/true));
    }

    SECTION("validator rejects every structural fault");
    {
        // Truncated operand
        Asm t; t.op(OP_PUSH_SIG).u8(1);
        CHECK(val(t) == Invalid::Truncated);

        // Unknown opcode
        Asm u; u.op((Op)200).end();
        CHECK(val(u) == Invalid::UnknownOp);

        // Underflow: a binary op with one operand
        Asm uf; uf.konst(1).op(OP_AND).end();
        CHECK(val(uf) == Invalid::StackUnderflow);

        // Underflow on a ternary
        Asm uf3; uf3.konst(1).konst(2).op(OP_CLAMP).end();
        CHECK(val(uf3) == Invalid::StackUnderflow);

        // Overflow: STACK_MAX+1 pushes
        Asm ov;
        for (int k = 0; k <= STACK_MAX; k++) ov.op(OP_PUSH_ONE);
        ov.end();
        CHECK(val(ov) == Invalid::StackOverflow);

        // No END inside the block
        Asm nt; nt.konst(1);
        CHECK(val(nt) == Invalid::NotTerminated);

        // END with two values left — an expression must yield exactly one
        Asm two; two.konst(1).konst(2).end();
        CHECK(val(two) == Invalid::BadResultCount);

        // Config offset past the end of the image
        Asm oo; oo.cfg(CFG_SIZE - 1, CT_U32).end();
        CHECK(val(oo) == Invalid::OffsetOutOfRange);

        // Unknown config type
        Asm bt; bt.op(OP_PUSH_CFG).u24(0).u8(99).end();
        CHECK(val(bt) == Invalid::BadType);

        // Bit group running past the top of its field
        Asm bb; bb.bcfg(0, CT_U8, 6, 4).end();
        CHECK(val(bb) == Invalid::BadBitRange);
        Asm bf; bf.bcfg(0, CT_F32, 0, 1).end();          // bits of a float are meaningless
        CHECK(val(bf) == Invalid::BadType);

        // Signal selector past the catalog
        Asm bs; bs.sig_raw((uint16_t)(SIG_COUNT + 1)).end();
        CHECK(val(bs) == Invalid::BadSignal);
        Asm none; none.sig_raw(0).end();                 // 0 = None is LEGAL (runs untrustworthy)
        CHECK(val(none) == Invalid::None);

        // A valid program stays valid
        Asm ok; ok.sig(SIG_RPM).konst(2500).op(OP_GT).end();
        CHECK(val(ok) == Invalid::None);
    }

    SECTION("exec is safe on UNVALIDATED bytecode (defence in depth)");
    {
        // exec() assumes validate() passed, but must not corrupt the frame if a
        // caller forgets or a program arrives from a partial write. These run
        // straight through exec with no validate, and the guarantee under test
        // is memory safety — run this file under ASan and the assertions below
        // are the least of what is being checked.
        SignalBus bus; Ctx c = make_ctx(bus);

        Asm ov;                                          // overflow the stack
        for (int k = 0; k < STACK_MAX * 4; k++) ov.op(OP_PUSH_ONE);
        ov.end();
        CHECK(!exec(ov.p(), ov.len(), c).ran);

        Asm uf; uf.op(OP_ADD).op(OP_ADD).end();          // underflow
        CHECK(!exec(uf.p(), uf.len(), c).ran);

        Asm tr; tr.op(OP_PUSH_SIG);                      // operand runs off the end
        CHECK(!exec(tr.p(), tr.len(), c).ran);

        Asm nt; nt.konst(1);                             // never terminates
        CHECK(!exec(nt.p(), nt.len(), c).ran);

        // Config reads outside the image are refused at RUN time too, not only
        // by the validator — an unvalidated program must not read past the end.
        Asm oo; oo.cfg(CFG_SIZE + 100, CT_U32).end();
        CHECK(!exec(oo.p(), oo.len(), c).truthy());
        Asm ob; ob.bcfg(CFG_SIZE - 1, CT_U32, 0, 4).end();
        CHECK(!exec(ob.p(), ob.len(), c).truthy());
    }

    SECTION("a rejected program never executes; the site decides what it is worth");
    {
        SignalBus bus; Ctx c = make_ctx(bus);
        Asm bad; bad.konst(1).konst(2).end();            // two values at END
        CHECK(val(bad) == Invalid::BadResultCount);
        CHECK(!run(bad, c).ran);                         // exec agrees, independently
        // A DTC gate must fail SAFE: a broken expression leaves detection ON.
        CHECK(eval_bool(bad.p(), bad.len(), c, /*on_invalid=*/true));
        CHECK(!eval_bool(bad.p(), bad.len(), c, /*on_invalid=*/false));
    }

    SECTION("the whole thing fits the block, and cannot run away");
    {
        // The doc's sizing claim, checked rather than asserted in prose:
        // three clauses with arithmetic and a packed config read inside 64 bytes.
        SignalBus bus;
        bus.set(SIG_MAP, 90.0f, true, 1000);
        bus.set(SIG_BARO_KPA, 60.0f, true, 1000);
        bus.set(SIG_RPM, 3000.0f, true, 1000);
        bus.set(SIG_CLT, 80.0f, true, 1000);
        Ctx c = make_ctx(bus);
        memset(g_cfg, 0, CFG_SIZE);
        g_cfg[5] = 0x01;

        // map > baro + 20 AND rpm > 2500 AND clt > 60 AND <config flag>
        Asm a; a.sig(SIG_MAP).sig(SIG_BARO_KPA).konst(20).op(OP_ADD).op(OP_GT)
                .sig(SIG_RPM).i16v(2500).op(OP_GT).op(OP_AND)
                .sig(SIG_CLT).i8v(60).op(OP_GT).op(OP_AND)
                .bcfg(5, CT_U8, 0, 1).op(OP_AND).end();
        CHECK(a.len() <= PROGRAM_MAX);
        CHECK(val(a) == Invalid::None);
        CHECK(run(a, c).truthy());

        fprintf(stdout, "  (realistic four-clause gate: %u bytes of %u)\n",
                (unsigned)a.len(), (unsigned)PROGRAM_MAX);
    }

    return test_summary();
}
