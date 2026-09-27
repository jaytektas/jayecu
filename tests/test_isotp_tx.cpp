// Host test for IsoTpTx — ISO 15765-2 multi-frame sender (FF -> FC -> CF pacing).
#include "test_helpers.h"
#include "../firmware/Can/IsoTpTx.h"
#include <vector>

static CanFrame fc(uint8_t fs, uint8_t bs, uint8_t stmin) {
    CanFrame f{}; f.id=0x7E0; f.dlc=8; f.data[0]=0x30|fs; f.data[1]=bs; f.data[2]=stmin; return f;
}
// Drain all frames due at now (block-size sub-blocks emit back-to-back within a tick window).
static std::vector<CanFrame> drain(IsoTpTx& tx, uint32_t now) {
    std::vector<CanFrame> out; CanFrame f;
    while (tx.poll(now, f)) { out.push_back(f); if (!tx.busy()) break; }
    return out;
}
int main() {
    fprintf(stdout, "=== IsoTpTx ===\n");

    // 20-byte VIN-style payload: [0x49][0x02][0x01] + 17 chars
    uint8_t pl[20]; for (int i=0;i<20;i++) pl[i]=(uint8_t)(i+1);

    SECTION("First Frame carries length + first 6 bytes");
    { IsoTpTx tx; tx.start(0x7E8, pl, 20);
      CanFrame f; bool got = tx.poll(100, f);
      CHECK(got);
      CHECK(f.id == 0x7E8);
      CHECK(f.data[0] == 0x10);            // len hi nibble 0 | 0x10
      CHECK(f.data[1] == 20);              // len lo
      CHECK(f.data[2] == 1 && f.data[7] == 6);  // first 6 payload bytes
      CHECK(tx.busy());
      // no more frames until FC arrives
      CHECK(!tx.poll(101, f)); }

    SECTION("no flow control within 1 s (N_Bs) gives the transfer up — it does not wedge for ever");
    { IsoTpTx tx; tx.start(0x7E8, pl, 20);
      CanFrame f; CHECK(tx.poll(100, f));          // FF sent, now waiting for FC
      CHECK(!tx.poll(101, f)); CHECK(tx.busy());
      CHECK(!tx.poll(900, f)); CHECK(tx.busy());   // still inside the second
      tx.on_flow_control(fc(1, 0, 0));             // a WAIT restarts the clock
      CHECK(!tx.poll(1200, f)); CHECK(tx.busy());
      CHECK(!tx.poll(2000, f)); CHECK(tx.busy());  // 800 ms since the WAIT
      CHECK(!tx.poll(2300, f)); CHECK(!tx.busy()); // 1100 ms: abandoned, free for the next request
    }

    SECTION("CTS bs=0 sends all remaining CFs, sequence 1..N wrap");
    { IsoTpTx tx; tx.start(0x7E8, pl, 20);
      CanFrame f; tx.poll(0, f);           // FF (6 bytes)
      tx.on_flow_control(fc(0x00, 0x00, 0x00));  // CTS, unlimited, STmin 0
      // remaining 14 bytes -> CF1 (7) + CF2 (7). STmin 0 -> both due at now.
      auto v = drain(tx, 0);
      CHECK(v.size() == 2);
      CHECK(v[0].data[0] == 0x21);         // CF seq 1
      CHECK(v[1].data[0] == 0x22);         // CF seq 2
      CHECK(v[0].data[1] == 7);            // 7th payload byte
      CHECK(v[1].data[7] == 20);           // last payload byte
      CHECK(!tx.busy()); }

    SECTION("STmin paces consecutive frames");
    { IsoTpTx tx; tx.start(0x7E8, pl, 20);
      CanFrame f; tx.poll(0, f);
      tx.on_flow_control(fc(0x00, 0x00, 10));   // 10 ms separation
      CHECK(tx.poll(0, f));  CHECK(f.data[0]==0x21);   // first CF immediate
      CHECK(!tx.poll(5, f));                            // 5ms < 10ms -> not due
      CHECK(tx.poll(10, f)); CHECK(f.data[0]==0x22);   // due at +10ms
      CHECK(!tx.busy()); }

    SECTION("block size forces re-wait for FC");
    { // 34-byte payload -> FF(6) + 4 CFs (7,7,7,7=28)
      uint8_t big[34]; for(int i=0;i<34;i++) big[i]=(uint8_t)i;
      IsoTpTx tx; tx.start(0x7E8, big, 34);
      CanFrame f; tx.poll(0, f);
      tx.on_flow_control(fc(0x00, 0x02, 0x00));  // block size 2
      auto v = drain(tx, 0);
      CHECK(v.size() == 2);                 // only 2 CFs, then awaits FC
      CHECK(tx.busy());
      tx.on_flow_control(fc(0x00, 0x00, 0x00));  // next block: send the rest
      auto v2 = drain(tx, 0);
      CHECK(v2.size() == 2);
      CHECK(!tx.busy()); }

    SECTION("abort FC (overflow) cancels transfer");
    { IsoTpTx tx; tx.start(0x7E8, pl, 20);
      CanFrame f; tx.poll(0, f);
      tx.on_flow_control(fc(0x02, 0x00, 0x00));  // overflow/abort
      CHECK(!tx.busy());
      CHECK(!tx.poll(0, f)); }

    return test_summary();
}
