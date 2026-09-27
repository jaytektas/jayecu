// ---------------------------------------------------------------------------
// SENT (SAE J2716) fast-channel sender — bench signal source for the JayECU
// SENT decoder (firmware/Sensors/SentDecoder.h). Arduino Uno.
//
// Output: pin 8 (PB0) — the Ardu-Stim "primary/crank" pin, already wired to the
// ECU's DIG1 input. Configure a SENT sensor on DIG1 (source = 0) to read it.
//
// Frame (idle HIGH, FALLING edges are the timing reference, falling→falling
// period = nibble ticks):
//   [ sync/cal : 56 ticks ][ status : 1 nibble ][ data0..5 : 6 nibbles ]
//   [ CRC : 1 nibble ]  then a short idle-high pause.
// A nibble of value v is (12 + v) ticks. The decoder self-clocks off the 56-tick
// sync, so the absolute tick time only needs to be consistent — TICK_US = 10 gives
// the Uno plenty of margin (delayMicroseconds + direct port writes).
//
// Fast-channel value = data[0..2] (MSN first) = 0xABC. The ECU should decode 2748
// -> curve_cal -> ~55.0 on the default 0..5000 -> 0..100 curve.
// ---------------------------------------------------------------------------

#define SENT_HIGH()  (PORTB |=  _BV(0))   // pin 8 = PB0
#define SENT_LOW()   (PORTB &= ~_BV(0))

const uint16_t TICK_US   = 10;            // microseconds per SENT tick
const uint8_t  LOW_TICKS = 5;             // fixed low portion of each nibble

uint8_t status  = 0x0;
uint8_t data[6] = { 0xA, 0xB, 0xC, 0x0, 0x0, 0x0 };   // fast value 0xABC (data[3..5] spare)

// Set the 12-bit fast-channel value -> the first three data nibbles (MSN first).
static void setValue(uint16_t v) {
  v &= 0x0FFF;
  data[0] = (v >> 8) & 0x0F;
  data[1] = (v >> 4) & 0x0F;
  data[2] =  v       & 0x0F;
}

// Live control: send a decimal value 0..4095 + newline over the serial port
// (115200 8N1) to change what the frame carries, without recompiling.
static void readSerial() {
  static char buf[8];
  static uint8_t n = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (n) { buf[n] = 0; setValue((uint16_t)atoi(buf)); n = 0; }
    } else if (n < sizeof(buf) - 1) {
      buf[n++] = c;
    }
  }
}

static uint8_t crcStep(uint8_t crc, uint8_t nib) {
  crc ^= (nib & 0x0F);
  for (uint8_t b = 0; b < 4; b++) {
    uint8_t top = crc & 0x08;
    crc = (uint8_t)((crc << 1) & 0x0F);
    if (top) crc ^= 0x03;                 // poly x^4 + x + 1
  }
  return crc & 0x0F;
}

static uint8_t crc4(const uint8_t* d, uint8_t n) {
  uint8_t crc = 5;                        // J2716 seed
  for (uint8_t i = 0; i < n; i++) crc = crcStep(crc, d[i]);
  crc = crcStep(crc, 0);                  // 2010-spec augmentation
  return crc & 0x0F;
}

// Emit one nibble period: a falling edge, a fixed low time, then high for the
// remainder so the NEXT falling edge lands `periodTicks` away.
static void nibble(uint16_t periodTicks) {
  SENT_LOW();
  delayMicroseconds((uint16_t)(LOW_TICKS * TICK_US));
  SENT_HIGH();
  delayMicroseconds((uint16_t)((periodTicks - LOW_TICKS) * TICK_US));
}

void setup() {
  DDRB |= _BV(0);                         // pin 8 output
  SENT_HIGH();
  // Kill the Timer0 overflow (millis()) interrupt — it's the ONLY periodic ISR and
  // its ~µs firing jitters delayMicroseconds enough to nudge nibbles ±1 tick.
  // delayMicroseconds is a busy-loop (needs no timer) and the USART RX interrupt is
  // separate, so serial control still works while the frame timing stays exact.
  TIMSK0 &= ~_BV(TOIE0);
  Serial.begin(115200);
}

void loop() {
  readSerial();                                  // pick up a new value if one was sent
  const uint8_t crc = crc4(data, 6);
  nibble(56);                                    // sync / calibration pulse
  nibble((uint16_t)(12 + status));               // status & serial-comm nibble
  for (uint8_t i = 0; i < 6; i++) nibble((uint16_t)(12 + data[i]));
  nibble((uint16_t)(12 + crc));                  // CRC nibble
  // PAUSE pulse: its own falling edge TERMINATES the CRC nibble (otherwise the
  // CRC period would absorb the gap and blow past the 12..27-tick nibble window),
  // and its high time is the inter-frame gap. Its length (≠ 56, ≠ 12..27) is never
  // mistaken for a sync or a nibble; the decoder reads only the 8 nibbles after sync.
  nibble(40);
}
