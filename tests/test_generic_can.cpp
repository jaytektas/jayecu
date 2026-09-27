// GENERIC CAN — the tune's own frames, both directions.
//
// Driven through a real CanBroker with a mock ICanChannel underneath, so the frames counted here are
// the frames that would reach a transceiver, and the signals checked are the ones a module would read.
//
//   cmake --build build --target test_generic_can && ./build/test_generic_can
#include "test_helpers.h"
#include "Can/CanBroker.h"
#include "Can/GenericCan.h"
#include "Can/CanMessageTypes.h"
#include "Signal/SignalBus.h"
#include "ecu_config.h"      // g_config — the SHIPPED default for a field's channel

#include <cstdio>
#include <cstring>
#include <vector>

using namespace canmsg;

namespace {

struct MockChannel final : ICanChannel {
    std::vector<CanFrame> tx;
    std::vector<CanFrame> rx;      // frames to hand up, oldest first
    size_t   rx_at  = 0;
    int      accept = -1;          // -1 = unlimited; else how many more sends succeed
    bool send(const CanFrame& f) override {
        if (accept == 0) return false;
        if (accept > 0) accept--;
        tx.push_back(f);
        return true;
    }
    bool receive(CanFrame& f) override {
        if (rx_at >= rx.size()) return false;
        f = rx[rx_at++];
        return true;
    }
    bool is_up() const override { return true; }
};

// A tune builder: the config is a plain struct, so a test says exactly what a studio would write.
struct T {
    CanConfig cfg{};
    uint16_t  n_frame = 0, n_field = 0;
    int       cur = -1;
    T() {
        std::memset(&cfg, 0, sizeof(cfg));
        for (auto& b : cfg.bus) b.enabled = 1;
    }
    T& frame(uint32_t id, bool tx, uint16_t period_ms, uint8_t bus = 0, uint8_t dlc = 8,
             bool ext = false) {
        cur = n_frame;
        GcFrameConfig& f = cfg.gc_frame[n_frame++];
        f.flags = static_cast<uint8_t>(FRAME_USED | (tx ? FRAME_TX : 0) | (ext ? FRAME_EXT : 0));
        f.bus = bus; f.id = id; f.dlc = dlc; f.period_ms = period_ms;
        f.first_field = n_field; f.field_count = 0;
        return *this;
    }
    T& field(uint16_t sig, uint16_t bit_off, uint8_t width, float scale, float offset,
             uint8_t flags = 0, uint8_t policy = ABSENT_ZERO, uint16_t ttl_ms = 500,
             uint8_t priority = PRIO_CAN) {
        GcFieldConfig& d = cfg.gc_field[n_field++];
        d.sig = sig; d.bit_off = bit_off; d.width = width;
        d.flags = flags; d.scale = scale; d.offset = offset;
        d.policy = policy; d.ttl_ms = ttl_ms; d.priority = priority;
        cfg.gc_frame[cur].field_count++;
        return *this;
    }
};

int count_id(const std::vector<CanFrame>& v, uint32_t id) {
    int n = 0;
    for (const CanFrame& f : v) if (f.id == id) n++;
    return n;
}
const CanFrame* first_id(const std::vector<CanFrame>& v, uint32_t id) {
    for (const CanFrame& f : v) if (f.id == id) return &f;
    return nullptr;
}
uint16_t be16(const CanFrame& f, int at) {
    return static_cast<uint16_t>((f.data[at] << 8) | f.data[at + 1]);
}

}  // namespace

int main() {
    fprintf(stdout, "=== generic CAN ===\n");

    // -----------------------------------------------------------------------------------------
    SECTION("transmit frames fire at their own rate");
    {
        T t;
        t.frame(0x360, true, 20).field(SIG_RPM, 7, 16, 1.0f, 0.0f)
                                .field(SIG_MAP, 23, 16, 10.0f, 0.0f);
        t.frame(0x3E0, true, 20).field(SIG_CLT, 7, 16, 10.0f, 2731.5f);
        t.frame(0x362, true, 10).field(SIG_TPS, 7, 16, 10.0f, 0.0f);

        MockChannel ch; CanBroker br; SignalBus bus;
        br.add_bus(0, &ch); br.set_signal_bus(&bus);
        bus.set(SIG_RPM, 3000.0f); bus.set(SIG_MAP, 101.3f);
        bus.set(SIG_CLT, 85.0f);   bus.set(SIG_TPS, 42.0f);
        br.reconfigure_generic(t.cfg, 0);

        for (uint32_t ms = 0; ms < 1000; ms++) br.update(ms);

        const int a = count_id(ch.tx, 0x360), b = count_id(ch.tx, 0x3E0), c = count_id(ch.tx, 0x362);
        fprintf(stdout, "    0x360=%d 0x3E0=%d 0x362=%d in 1000 ms\n", a, b, c);
        CHECK(a >= 49 && a <= 51);
        CHECK(b >= 49 && b <= 51);
        CHECK(c >= 99 && c <= 101);
    }

    // -----------------------------------------------------------------------------------------
    SECTION("the payload is what the bus says, through the field's own transform");
    {
        T t;
        t.frame(0x360, true, 20).field(SIG_RPM, 7, 16, 1.0f, 0.0f)
                                .field(SIG_MAP, 23, 16, 10.0f, 0.0f);
        t.frame(0x3E0, true, 20).field(SIG_CLT, 7, 16, 10.0f, 2731.5f);

        MockChannel ch; CanBroker br; SignalBus bus;
        br.add_bus(0, &ch); br.set_signal_bus(&bus);
        bus.set(SIG_RPM, 3000.0f); bus.set(SIG_MAP, 101.3f); bus.set(SIG_CLT, 85.0f);
        br.reconfigure_generic(t.cfg, 0);
        for (uint32_t ms = 0; ms < 40; ms++) br.update(ms);

        const CanFrame* f = first_id(ch.tx, 0x360);
        CHECK(f != nullptr);
        CHECK(f->dlc == 8);
        CHECK(be16(*f, 0) == 3000);                       // rpm, raw = value
        CHECK(be16(*f, 2) == 1013);                       // map, raw = kPa * 10
        // …and coolant as Kelvin x10, the conversion that trips people up: 85 C -> 358.15 K -> 3582.
        const CanFrame* g = first_id(ch.tx, 0x3E0);
        CHECK(g != nullptr);
        CHECK(be16(*g, 0) == 3582);
    }

    // -----------------------------------------------------------------------------------------
    SECTION("deadlines are SPREAD — a rate tier does not fire as one burst");
    {
        T t;
        t.frame(0x360, true, 20).field(SIG_RPM, 7, 16, 1.0f, 0.0f);
        t.frame(0x3E0, true, 20).field(SIG_CLT, 7, 16, 10.0f, 2731.5f);

        MockChannel ch; CanBroker br; SignalBus bus;
        br.add_bus(0, &ch); br.set_signal_bus(&bus);
        bus.set(SIG_RPM, 1000.0f); bus.set(SIG_CLT, 80.0f);
        br.reconfigure_generic(t.cfg, 0);

        int same_tick = 0;
        for (uint32_t ms = 0; ms < 200; ms++) {
            const size_t before = ch.tx.size();
            br.update(ms);
            int a = 0, b = 0;
            for (size_t i = before; i < ch.tx.size(); i++) {
                if (ch.tx[i].id == 0x360) a++;
                if (ch.tx[i].id == 0x3E0) b++;
            }
            if (a && b) same_tick++;
        }
        fprintf(stdout, "    ticks carrying BOTH 20 ms frames: %d\n", same_tick);
        CHECK(same_tick == 0);
    }

    // -----------------------------------------------------------------------------------------
    SECTION("an absent channel follows the field's policy, not one blanket rule");
    {
        // ZERO writes RAW zero, which through an offset is a visibly broken reading — that is the
        // point of it. HOLD leaves the previous bytes. SKIP suppresses the whole frame.
        T t;
        t.frame(0x400, true, 10).field(SIG_RPM, 7, 16, 1.0f, 0.0f)
                                .field(SIG_OIL_TEMP, 23, 16, 10.0f, 2731.5f, 0, ABSENT_HOLD);
        t.frame(0x401, true, 10).field(SIG_OIL_PRESSURE, 7, 16, 10.0f, 1013.0f, 0, ABSENT_SKIP);

        MockChannel ch; CanBroker br; SignalBus bus;
        br.add_bus(0, &ch); br.set_signal_bus(&bus);
        bus.set(SIG_RPM, 2000.0f);          // oil temp and oil pressure are never written: absent
        br.reconfigure_generic(t.cfg, 0);
        for (uint32_t ms = 0; ms < 60; ms++) br.update(ms);

        const CanFrame* f = first_id(ch.tx, 0x400);
        CHECK(f != nullptr);
        CHECK(be16(*f, 0) == 2000);                       // the live field still goes out
        CHECK(be16(*f, 2) == 0);                          // HOLD over a never-written buffer = zero
        // SKIP means the frame does not appear at all — not that it appears full of zeros.
        CHECK(count_id(ch.tx, 0x401) == 0);
        fprintf(stdout, "    0x400=%d (live) 0x401=%d (skipped)\n",
                count_id(ch.tx, 0x400), count_id(ch.tx, 0x401));
    }

    // -----------------------------------------------------------------------------------------
    SECTION("a value beyond the field CLAMPS — it does not wrap into a plausible lie");
    {
        T t;
        t.frame(0x402, true, 10).field(SIG_RPM, 7, 16, 10.0f, 0.0f);   // 7000 rpm * 10 = 70000
        MockChannel ch; CanBroker br; SignalBus bus;
        br.add_bus(0, &ch); br.set_signal_bus(&bus);
        bus.set(SIG_RPM, 7000.0f);
        br.reconfigure_generic(t.cfg, 0);
        for (uint32_t ms = 0; ms < 30; ms++) br.update(ms);

        const CanFrame* f = first_id(ch.tx, 0x402);
        CHECK(f != nullptr);
        // Unclamped this wraps to 4464, which reads as a perfectly believable 446 rpm.
        CHECK(be16(*f, 0) == 65535);
    }

    // -----------------------------------------------------------------------------------------
    SECTION("receive decodes into the signal bus, and the value expires on its TTL");
    {
        T t;
        t.frame(0x200, false, 0).field(SIG_VEHICLE_SPD, 7, 16, 10.0f, 0.0f, 0, 0, 100);

        MockChannel ch; CanBroker br; SignalBus bus;
        br.add_bus(0, &ch); br.set_signal_bus(&bus);
        br.reconfigure_generic(t.cfg, 0);

        CHECK(!bus.valid(SIG_VEHICLE_SPD));               // nothing has said anything yet

        CanFrame in{};
        in.id = 0x200; in.dlc = 8; in.ext = false;
        in.data[0] = 0x03; in.data[1] = 0xE8;             // 1000 raw / 10 = 100.0
        ch.rx.push_back(in);
        br.update(10);

        CHECK(bus.valid(SIG_VEHICLE_SPD));
        fprintf(stdout, "    decoded vehicle_spd = %.2f\n",
                static_cast<double>(bus.get(SIG_VEHICLE_SPD)));
        CHECK_NEAR(bus.get(SIG_VEHICLE_SPD), 100.0f, 0.01f);

        // The sender goes quiet. Without a TTL the last number sits there for ever and a dash shows
        // a speed that stopped being true minutes ago.
        bus.expire_stale(200);
        CHECK(!bus.valid(SIG_VEHICLE_SPD));
    }

    // -----------------------------------------------------------------------------------------
    SECTION("a signed receive field sign-extends rather than reading as a huge positive");
    {
        T t;
        t.frame(0x201, false, 0).field(SIG_ADVANCE, 7, 16, 10.0f, 0.0f, FIELD_SIGNED);

        MockChannel ch; CanBroker br; SignalBus bus;
        br.add_bus(0, &ch); br.set_signal_bus(&bus);
        br.reconfigure_generic(t.cfg, 0);

        CanFrame in{};
        in.id = 0x201; in.dlc = 8;
        in.data[0] = 0xFF; in.data[1] = 0x9C;             // -100 raw / 10 = -10.0 deg (retard)
        ch.rx.push_back(in);
        br.update(10);
        fprintf(stdout, "    decoded advance = %.2f\n", static_cast<double>(bus.get(SIG_ADVANCE)));
        CHECK_NEAR(bus.get(SIG_ADVANCE), -10.0f, 0.01f);
    }

    // -----------------------------------------------------------------------------------------
    SECTION("a SHORT frame does not decode the field it did not carry (no zeros read as a value)");
    {
        T t;
        t.frame(0x202, false, 0).field(SIG_ADVANCE, 55, 16, 10.0f, 0.0f);   // Motorola, bytes 6..7
        MockChannel ch; CanBroker br; SignalBus bus;
        br.add_bus(0, &ch); br.set_signal_bus(&bus);
        br.reconfigure_generic(t.cfg, 0);
        CanFrame in{}; in.id = 0x202; in.dlc = 4;            // only 4 bytes on the wire
        ch.rx.push_back(in);
        br.update(10);
        CHECK(!bus.valid(SIG_ADVANCE));                       // not published as 0.0
    }

    SECTION("a SIGNED field's no-reading code matches (it was compared after sign extension)");
    {
        T t;
        t.frame(0x203, false, 0).field(SIG_ADVANCE, 7, 16, 10.0f, 0.0f, FIELD_SIGNED | FIELD_SENTINEL);
        t.cfg.gc_field[0].sentinel = 0x8000;
        MockChannel ch; CanBroker br; SignalBus bus;
        br.add_bus(0, &ch); br.set_signal_bus(&bus);
        br.reconfigure_generic(t.cfg, 0);
        CanFrame in{}; in.id = 0x203; in.dlc = 8;
        in.data[0] = 0x80; in.data[1] = 0x00;                 // the sender's "no reading"
        ch.rx.push_back(in);
        br.update(10);
        CHECK(!bus.valid(SIG_ADVANCE));                       // not -3276.8 degrees
    }

    SECTION("Intel and Motorola bit order are different layouts, and both round-trip");
    {
        // The same 16-bit value, same start bit, the two conventions. Motorola puts the MSB at the
        // start bit and runs down; Intel puts the LSB there and runs up into the NEXT byte — which
        // is why the payload looks byte-swapped. A flag that nothing honoured would make these equal.
        T t;
        t.frame(0x300, true, 10).field(SIG_RPM, 7, 16, 1.0f, 0.0f);                  // Motorola
        t.frame(0x301, true, 10).field(SIG_RPM, 0, 16, 1.0f, 0.0f, FIELD_LITTLE);    // Intel

        MockChannel ch; CanBroker br; SignalBus bus;
        br.add_bus(0, &ch); br.set_signal_bus(&bus);
        bus.set(SIG_RPM, 0x1234);
        br.reconfigure_generic(t.cfg, 0);
        for (uint32_t ms = 0; ms < 30; ms++) br.update(ms);

        const CanFrame* m = first_id(ch.tx, 0x300);
        const CanFrame* i = first_id(ch.tx, 0x301);
        CHECK(m != nullptr && i != nullptr);
        fprintf(stdout, "    motorola %02X %02X   intel %02X %02X\n",
                m->data[0], m->data[1], i->data[0], i->data[1]);
        CHECK(m->data[0] == 0x12 && m->data[1] == 0x34);
        CHECK(i->data[0] == 0x34 && i->data[1] == 0x12);

        // …and each decodes back to what it encoded, which is the property that actually matters.
        for (bool little : { false, true }) {
            const uint8_t flags = little ? FIELD_LITTLE : 0;
            T r;
            r.frame(0x302, false, 0).field(SIG_VEHICLE_SPD, little ? 0 : 7, 16, 1.0f, 0.0f, flags);
            MockChannel c2; CanBroker b2; SignalBus s2;
            b2.add_bus(0, &c2); b2.set_signal_bus(&s2);
            b2.reconfigure_generic(r.cfg, 0);
            CanFrame in{};
            in.id = 0x302; in.dlc = 8;
            in.data[0] = little ? i->data[0] : m->data[0];
            in.data[1] = little ? i->data[1] : m->data[1];
            c2.rx.push_back(in);
            b2.update(10);
            CHECK_NEAR(s2.get(SIG_VEHICLE_SPD), 4660.0f, 0.5f);   // 0x1234
        }
    }

    // -----------------------------------------------------------------------------------------
    SECTION("an odd-width field addresses the bits it says and leaves its neighbours alone");
    {
        // 12 bits spanning a byte boundary, with a 1-bit flag before it — the layout a hand-rolled
        // shift-and-mask gets wrong and a per-bit loop does not.
        T t;
        t.frame(0x303, true, 10).field(SIG_RPM, 7, 1, 1.0f, 0.0f)        // byte 0 bit 7, alone
                                .field(SIG_MAP, 3, 12, 1.0f, 0.0f);      // byte0 bit3 .. byte1 bit0

        MockChannel ch; CanBroker br; SignalBus bus;
        br.add_bus(0, &ch); br.set_signal_bus(&bus);
        bus.set(SIG_RPM, 1.0f); bus.set(SIG_MAP, 0xABC);
        br.reconfigure_generic(t.cfg, 0);
        for (uint32_t ms = 0; ms < 30; ms++) br.update(ms);

        const CanFrame* f = first_id(ch.tx, 0x303);
        CHECK(f != nullptr);
        fprintf(stdout, "    odd layout: %02X %02X\n", f->data[0], f->data[1]);
        // the lone bit is byte0 bit7 = 0x80; then 0xABC from bit 3 down -> byte0 gets 0xA, byte1 0xBC
        CHECK(f->data[0] == 0x8A);
        CHECK(f->data[1] == 0xBC);
    }

    // -----------------------------------------------------------------------------------------
    SECTION("frames on a bus that is switched off do not run");
    {
        T t;
        t.frame(0x360, true, 10).field(SIG_RPM, 7, 16, 1.0f, 0.0f);
        t.cfg.bus[0].enabled = 0;

        MockChannel ch; CanBroker br; SignalBus bus;
        br.add_bus(0, &ch); br.set_signal_bus(&bus);
        bus.set(SIG_RPM, 3000.0f);
        br.reconfigure_generic(t.cfg, 0);
        for (uint32_t ms = 0; ms < 100; ms++) br.update(ms);
        CHECK(ch.tx.empty());

        // …and switching it on takes effect without a reset, which is the contract reconfigure has.
        t.cfg.bus[0].enabled = 1;
        br.reconfigure_generic(t.cfg, 100);
        for (uint32_t ms = 100; ms < 200; ms++) br.update(ms);
        CHECK(!ch.tx.empty());
        fprintf(stdout, "    off=0 frames, on=%zu frames\n", ch.tx.size());
    }

    // -----------------------------------------------------------------------------------------
    SECTION("a standard and an extended frame with the same number are different frames");
    {
        T t;
        t.frame(0x100, false, 0, 0, 8, false).field(SIG_VEHICLE_SPD, 7, 16, 1.0f, 0.0f);
        t.frame(0x100, false, 0, 0, 8, true ).field(SIG_RPM, 7, 16, 1.0f, 0.0f);

        MockChannel ch; CanBroker br; SignalBus bus;
        br.add_bus(0, &ch); br.set_signal_bus(&bus);
        br.reconfigure_generic(t.cfg, 0);

        CanFrame in{};
        in.id = 0x100; in.dlc = 8; in.ext = true;
        in.data[0] = 0x0B; in.data[1] = 0xB8;             // 3000
        ch.rx.push_back(in);
        br.update(10);
        CHECK(bus.valid(SIG_RPM));
        CHECK_NEAR(bus.get(SIG_RPM), 3000.0f, 0.5f);
        CHECK(!bus.valid(SIG_VEHICLE_SPD));               // the standard-id frame never arrived
    }

    // -----------------------------------------------------------------------------------------
    SECTION("a received value outranks a sensor's publish, including an INVALID one");
    {
        // The failure this priority exists to stop: a sensor publishes even when invalid (the
        // publish-invalid rule), so at equal priority a configured-but-failing sensor marks a
        // perfectly good CAN channel invalid and nothing anywhere says why.
        T t;
        t.frame(0x210, false, 0).field(SIG_CLT, 7, 16, 10.0f, 2731.5f);

        MockChannel ch; CanBroker br; SignalBus bus;
        br.add_bus(0, &ch); br.set_signal_bus(&bus);
        br.reconfigure_generic(t.cfg, 0);

        CanFrame in{};
        in.id = 0x210; in.dlc = 8;
        in.data[0] = 0x0D; in.data[1] = 0xFE;             // 3582 raw -> 85.0 C
        ch.rx.push_back(in);
        br.update(10);
        CHECK(bus.valid(SIG_CLT));
        CHECK_NEAR(bus.get(SIG_CLT), 85.0f, 0.1f);

        // A sensor now publishes INVALID at base priority, exactly as a failing one does.
        bus.set(SIG_CLT, -40.0f, /*valid=*/false, 11, 3000, PRIO_BASE);
        CHECK(bus.valid(SIG_CLT));                        // out-voted: the CAN value stands
        CHECK_NEAR(bus.get(SIG_CLT), 85.0f, 0.1f);

        // …and a Lua override still wins, because a person at the keyboard outranks a wire.
        bus.set(SIG_CLT, 50.0f, true, 12, 3000, PRIO_LUA);
        CHECK_NEAR(bus.get(SIG_CLT), 50.0f, 0.1f);
    }

    // -----------------------------------------------------------------------------------------
    SECTION("dropping a field to base priority lets a sensor take the channel back");
    {
        T t;
        t.frame(0x211, false, 0).field(SIG_CLT, 7, 16, 10.0f, 2731.5f, 0, 0, 500, PRIO_BASE);

        MockChannel ch; CanBroker br; SignalBus bus;
        br.add_bus(0, &ch); br.set_signal_bus(&bus);
        br.reconfigure_generic(t.cfg, 0);

        CanFrame in{};
        in.id = 0x211; in.dlc = 8;
        in.data[0] = 0x0D; in.data[1] = 0xFE;
        ch.rx.push_back(in);
        br.update(10);
        CHECK_NEAR(bus.get(SIG_CLT), 85.0f, 0.1f);
        bus.set(SIG_CLT, 20.0f, true, 11, 3000, PRIO_BASE);
        CHECK_NEAR(bus.get(SIG_CLT), 20.0f, 0.1f);        // equal priority: last writer, as documented
    }

    // -----------------------------------------------------------------------------------------
    // A SENSOR NAMES ITS FIELD BY FRAME AND START BIT, and the point of that is what happens when
    // the pool is repacked. The studio rewrites the WHOLE field pool on any structural edit, so a
    // stored pool index follows the repack onto whatever field has landed there — silently, because
    // a valid index decodes to a plausible number. The test inserts a frame ahead of the one a
    // sensor reads and checks the reference still lands on the same bits.
    SECTION("a repack moves the pool index; frame + start bit still resolves");
    {
        const uint32_t k361 = GenericCan::frame_key(0, false, 0x361);

        T before;
        before.frame(0x360, false, 0).field(SIG_RPM, 7, 16, 1.0f, 0.0f)
                                     .field(SIG_MAP, 23, 16, 10.0f, 0.0f);
        before.frame(0x361, false, 0).field(SIG_NONE, 7, 16, 0.001f, 0.0f);   // channel-less: a sensor reads it

        const int32_t was = GenericCan::find_field(before.cfg, k361, 7);
        CHECK(was == 2);

        // The same tune after the studio adds a frame ABOVE it and repacks: 0x300 now owns fields
        // 0..2, so everything later moved up by three.
        T after;
        after.frame(0x300, false, 0).field(SIG_TPS, 7, 8, 1.0f, 0.0f)
                                    .field(SIG_CLT, 15, 8, 1.0f, 0.0f)
                                    .field(SIG_IAT, 23, 8, 1.0f, 0.0f);
        after.frame(0x360, false, 0).field(SIG_RPM, 7, 16, 1.0f, 0.0f)
                                    .field(SIG_MAP, 23, 16, 10.0f, 0.0f);
        after.frame(0x361, false, 0).field(SIG_NONE, 7, 16, 0.001f, 0.0f);

        const int32_t now = GenericCan::find_field(after.cfg, k361, 7);
        CHECK(now == 5);                                  // it moved…
        CHECK(now != was);                                // …which is the whole hazard

        // WHAT A STORED POOL INDEX WOULD HAVE READ INSTEAD: field 2 is now the third field of 0x300,
        // a different frame on different bits. This is the assertion that would have gone red before
        // the reference changed — the sensor would have been reading IAT and calling it lambda.
        CHECK(after.cfg.gc_field[was].sig == SIG_IAT);

        // A frame that is gone, and a start bit no field in the frame has: both "no field", which is
        // what makes the sensor read nothing instead of reading somebody else's bits.
        CHECK(GenericCan::find_field(after.cfg, GenericCan::frame_key(0, false, 0x999), 7) == -1);
        CHECK(GenericCan::find_field(after.cfg, k361, 39) == -1);
        CHECK(GenericCan::find_field(after.cfg, k361, -1) == -1);          // -1 = nothing named

        // The bus and the extended flag are part of the identity: the same id elsewhere is not it.
        CHECK(GenericCan::find_field(after.cfg, GenericCan::frame_key(1, false, 0x361), 7) == -1);
        CHECK(GenericCan::find_field(after.cfg, GenericCan::frame_key(0, true,  0x361), 7) == -1);

        // A TRANSMIT frame is never a candidate — nothing fills a transmit field's decoded value, so
        // a sensor pointed at one would wait for a reading that cannot arrive.
        T tx;
        tx.frame(0x362, true, 20).field(SIG_NONE, 7, 16, 1.0f, 0.0f);
        CHECK(GenericCan::find_field(tx.cfg, GenericCan::frame_key(0, false, 0x362), 7) == -1);
    }

    // -----------------------------------------------------------------------------------------
    // A FIELD'S CHANNEL HAS TO HAVE AN "UNSET", and it cannot be 0 — 0 is SIG_ABS_MODE, a real
    // channel. The studio's Receive page offers "none (a sensor reads it)" as the first answer and
    // the tune has to be able to say it, because a channel-less field is the whole mechanism by
    // which a CAN SENSOR reads a frame. Stored as 0, every such field also published its decoded
    // value onto abs_mode at PRIO_CAN, and two of them counted as two producers of one channel.
    SECTION("a field with no channel publishes nothing");
    {
        CHECK(g_config.can.gc_field[0].sig == SIG_NONE);   // the shipped default means UNSET

        T t;
        t.frame(0x4B0, false, 0).field(g_config.can.gc_field[0].sig, 7, 16, 1.0f, 0.0f);

        MockChannel ch; CanBroker br; SignalBus bus;
        br.add_bus(0, &ch); br.set_signal_bus(&bus);
        br.reconfigure_generic(t.cfg, 0);

        CanFrame in{};
        in.id = 0x4B0; in.dlc = 8;
        in.data[0] = 0x12; in.data[1] = 0x34;
        ch.rx.push_back(in);
        br.update(10);

        CHECK(!bus.valid(SIG_ABS_MODE));                   // nothing was published anywhere…
        CHECK(br.generic().decoded() == 1);                // …but the frame WAS decoded, for the sensor
        const GenericCan::FieldValue* fv = br.generic().value(0);
        CHECK(fv != nullptr && fv->at_ms == 10);
    }

    return test_summary();
}
