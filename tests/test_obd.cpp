// Host test for ObdResponder — OBD-II over CAN, focus on Mode 09 (VIN/CVN) incl. ISO-TP multi-frame.
#include "test_helpers.h"
#include "../firmware/Can/CanBroker.h"
#include "../firmware/Can/ICanChannel.h"
#include "../firmware/Diagnostics/DtcManager.h"
#include "signal_ids.h"
#include <vector>

struct MockCan : ICanChannel {
    std::vector<CanFrame> rx;   // frames the tester sends to the ECU
    std::vector<CanFrame> tx;   // frames the ECU sends back (captured)
    bool send(const CanFrame& f) override { tx.push_back(f); return true; }
    bool receive(CanFrame& f) override {
        if (rx.empty()) return false;
        f = rx.front(); rx.erase(rx.begin()); return true; }
    bool is_up() const override { return true; }
};
static CanFrame req(uint8_t mode, uint8_t pid) {
    CanFrame f{}; f.id=0x7DF; f.dlc=8; f.data[0]=0x02; f.data[1]=mode; f.data[2]=pid; return f;
}
static CanFrame fc(uint8_t bs, uint8_t stmin) {
    CanFrame f{}; f.id=0x7E0; f.dlc=8; f.data[0]=0x30; f.data[1]=bs; f.data[2]=stmin; return f;
}
int main() {
    fprintf(stdout, "=== ObdResponder / Mode 09 ===\n");

    SECTION("Mode 09 PID 00 -> single-frame supported bitmask");
    { MockCan m; CanBroker br; br.add_bus(0,&m); br.enable_obd(0);
      m.rx.push_back(req(0x09, 0x00)); br.update(0);
      CHECK(m.tx.size()==1);
      auto& r=m.tx[0];
      CHECK(r.id==0x7E8);
      CHECK(r.data[0]==6);        // SF length
      CHECK(r.data[1]==0x49);     // 0x40|0x09
      CHECK(r.data[2]==0x00);     // pid
      // bit for PID 0x02 set (VIN) -> top byte 0x40
      CHECK(r.data[3]==0x54); }   // 0x02|0x04|0x06|0x0A -> bits(32-n): 0x40|0x10|0x04|0x00 = 0x54

    SECTION("Mode 09 PID 06 CVN -> single frame");
    { MockCan m; CanBroker br; br.add_bus(0,&m); br.enable_obd(0);
      m.rx.push_back(req(0x09, 0x06)); br.update(0);
      CHECK(m.tx.size()==1);
      auto& r=m.tx[0];
      CHECK(r.data[0]==7);        // len: 0x49,0x06,0x01 + 4 CVN bytes
      CHECK(r.data[1]==0x49);
      CHECK(r.data[2]==0x06);
      CHECK(r.data[3]==0x01); }   // NODI

    SECTION("Mode 09 PID 02 VIN -> First Frame then CFs after Flow Control");
    { MockCan m; CanBroker br; br.add_bus(0,&m); br.enable_obd(0);
      m.rx.push_back(req(0x09, 0x02)); br.update(0);
      // payload = 0x49,0x02,0x01 + 17 VIN = 20 bytes -> FF + 2 CFs
      CHECK(m.tx.size()==1);
      CanFrame ff=m.tx[0];        // copy: m.tx reallocates when the CFs are pushed below
      CHECK(ff.data[0]==0x10);    // FF PCI, len hi
      CHECK(ff.data[1]==20);      // total length
      CHECK(ff.data[2]==0x49);
      CHECK(ff.data[3]==0x02);
      CHECK(ff.data[4]==0x01);
      CHECK(ff.data[5]=='J');     // first VIN char
      // tester grants flow control (bs 0, stmin 0) -> ECU bursts the CFs on next tick
      m.rx.push_back(fc(0x00,0x00)); br.update(1);
      CHECK(m.tx.size()==3);
      CHECK(m.tx[1].data[0]==0x21);   // CF seq 1
      CHECK(m.tx[2].data[0]==0x22);   // CF seq 2
      // reconstruct the VIN from the 3 frames
      char vin[17]; int k=0;
      for(int i=5;i<8;i++) vin[k++]=ff.data[i];              // FF: bytes 3..5 of payload = VIN[0..2]
      for(int i=1;i<8 && k<17;i++) vin[k++]=m.tx[1].data[i];
      for(int i=1;i<8 && k<17;i++) vin[k++]=m.tx[2].data[i];
      CHECK(k==17);
      CHECK(vin[0]=='J' && vin[1]=='A' && vin[2]=='Y'); }

    SECTION("Mode 03: two bytes per code, every active code (multi-frame past two)");
    { MockCan m; CanBroker br; br.add_bus(0,&m); br.enable_obd(0);
      DtcManager dtc; dtc.init(1); dtc.set_active(true); br.set_dtc(&dtc);
      dtc.raise(0x0171, DtcSource::MODULE, 2, 10, 0);            // three active codes
      dtc.raise(0x0300, DtcSource::MODULE, 2, 10, 0);
      dtc.raise(0x0234, DtcSource::MODULE, 2, 10, 0);
      CanFrame q{}; q.id=0x7DF; q.dlc=8; q.data[0]=0x01; q.data[1]=0x03;
      m.rx.push_back(q); br.update(0);
      CHECK(m.tx.size()==1);
      CanFrame ff=m.tx[0];
      CHECK(ff.data[0]==0x10);                                    // multi-frame: 2 + 3*2 = 8 bytes
      CHECK(ff.data[1]==8);
      CHECK(ff.data[2]==0x43);
      CHECK(ff.data[3]==3);                                       // the COUNT: all three, not one
      m.rx.push_back(fc(0x00,0x00)); br.update(1);
      CHECK(m.tx.size()==2);                                      // the rest in one CF
      // Reassemble and check every code is present, two bytes each.
      uint8_t b[8]; for (int i=0;i<6;i++) b[i]=ff.data[2+i]; b[6]=m.tx[1].data[1]; b[7]=m.tx[1].data[2];
      bool has171=false, has300=false, has234=false;
      for (int i=2;i<8;i+=2) { const uint16_t c=(uint16_t)((b[i]<<8)|b[i+1]);
        has171 |= c==0x0171; has300 |= c==0x0300; has234 |= c==0x0234; }
      CHECK(has171 && has300 && has234); }

    SECTION("Mode 03 with one code: a single frame, [len][0x43][1][hi][lo]");
    { MockCan m; CanBroker br; br.add_bus(0,&m); br.enable_obd(0);
      DtcManager dtc; dtc.init(1); dtc.set_active(true); br.set_dtc(&dtc);
      dtc.raise(0x0171, DtcSource::MODULE, 2, 10, 0);
      CanFrame q{}; q.id=0x7DF; q.dlc=8; q.data[0]=0x01; q.data[1]=0x03;
      m.rx.push_back(q); br.update(0);
      CHECK(m.tx.size()==1);
      CHECK(m.tx[0].data[0]==4);
      CHECK(m.tx[0].data[1]==0x43 && m.tx[0].data[2]==1);
      CHECK(m.tx[0].data[3]==0x01 && m.tx[0].data[4]==0x71); }

    SECTION("Mode 01 PID 0C RPM still works (regression)");
    { MockCan m; CanBroker br; br.add_bus(0,&m); br.enable_obd(0);
      m.rx.push_back(req(0x01, 0x0C)); br.update(0);
      CHECK(m.tx.size()==1);
      CHECK(m.tx[0].data[1]==0x41);   // 0x40|0x01
      CHECK(m.tx[0].data[2]==0x0C); }

    // Mode 01 answers from the broker's telemetry cache, and that cache is only filled by the
    // SignalBus the broker was given. The shape checks above passed for a long time while every
    // live PID answered out of a zeroed struct: 0 rpm, -40 C coolant. These check the VALUE.
    SECTION("Mode 01 PIDs carry the live signal, not a zeroed cache");
    { MockCan m; SignalBus bus; CanBroker br;
      br.add_bus(0,&m); br.set_signal_bus(&bus); br.enable_obd(0);
      bus.set(SIG_RPM, 3000.0f, true, 0);
      bus.set(SIG_CLT,   80.0f, true, 0);
      bus.set(SIG_MAP,   95.0f, true, 0);

      m.rx.push_back(req(0x01, 0x0C)); br.update(0);        // RPM: (256A+B)/4
      CHECK(m.tx.size()==1);
      CHECK(((m.tx[0].data[3]<<8)|m.tx[0].data[4]) == 3000*4);

      m.tx.clear();
      m.rx.push_back(req(0x01, 0x05)); br.update(1);        // coolant: A = degC + 40
      CHECK(m.tx.size()==1);
      CHECK(m.tx[0].data[3] == 80+40);

      m.tx.clear();
      m.rx.push_back(req(0x01, 0x0B)); br.update(2);        // MAP: A = kPa
      CHECK(m.tx.size()==1);
      CHECK(m.tx[0].data[3] == 95);

      // And it tracks — a second request must not replay the first answer.
      bus.set(SIG_RPM, 1500.0f, true, 3);
      m.tx.clear();
      m.rx.push_back(req(0x01, 0x0C)); br.update(3);
      CHECK(((m.tx[0].data[3]<<8)|m.tx[0].data[4]) == 1500*4);

      // Road speed: it used to answer 0 whatever the car was doing.
      bus.set(SIG_VEHICLE_SPD, 87.0f, true, 4);
      m.tx.clear();
      m.rx.push_back(req(0x01, 0x0D)); br.update(4);        // speed: A = km/h
      CHECK(m.tx.size()==1);
      CHECK(m.tx[0].data[3] == 87); }

    // The supported-PID chain: 0x00 must point on to 0x20, 0x20 on to 0x40, and 0x40 list 0x44/0x46,
    // or a scan tool never asks for commanded lambda or ambient temperature.
    SECTION("supported-PID bitmasks chain through to 0x44 and 0x46");
    { MockCan m; CanBroker br; br.add_bus(0,&m); br.enable_obd(0);
      auto mask = [&](uint8_t pid) {
          m.tx.clear(); m.rx.push_back(req(0x01, pid)); br.update(0);
          CHECK(m.tx.size()==1);
          return (uint32_t(m.tx[0].data[3])<<24)|(uint32_t(m.tx[0].data[4])<<16)
               | (uint32_t(m.tx[0].data[5])<<8)|m.tx[0].data[6]; };
      auto has = [](uint32_t mk, int n) { return (mk >> (32 - n)) & 1u; };
      CHECK(has(mask(0x00), 0x20));
      CHECK(has(mask(0x20), 0x20));                 // 0x40 is bit 0x20 of the 0x21-0x40 page
      const uint32_t m40 = mask(0x40);
      CHECK(has(m40, 0x04) && has(m40, 0x06)); }    // 0x44, 0x46

    SECTION("stray Flow Control with no active transfer is ignored");
    { MockCan m; CanBroker br; br.add_bus(0,&m); br.enable_obd(0);
      m.rx.push_back(fc(0x00,0x00)); br.update(0);
      CHECK(m.tx.size()==0); }

    return test_summary();
}
