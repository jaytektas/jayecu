# CAN configuration

:material-circle:{ .level-intermediate } Intermediate → :material-circle:{ .level-advanced } Advanced

> **In one sentence:** you describe each CAN frame the ECU sends or reads — its id, its length, and
> which bits carry which channel with what scaling — and the ECU turns channels into frames and frames
> into channels.

Chapter 13 covers wiring a bus, switching it on and loading a device template. Chapter 14 covers the
OBD-II port. This chapter is the frame editor: building a frame yourself, or checking and changing one
a template made.

## What it does

:material-circle:{ .level-basic } Basic

Each bus has two pages under **Configuration ▸ CAN Bus ▸ CAN1** (and **CAN2**):

- **Transmit**: frames the ECU **sends**, each on its own period, built from live channels. A dash or
  logger reads them.
- **Receive**: frames the ECU **reads**. Each field becomes a channel, like a sensor reading, or is
  picked up by a CAN sensor (chapter 17).

The frames are part of the tune, not the firmware. A frame that turns out to be wrong is a tune edit.
The tune holds up to **96 frames** and **512 fields**, shared by both buses.
<!-- src: firmware/Can/GenericCan.h; definition/ecu.schema.yaml (Can: gc_frame count 96, gc_field count 512) -->

## How it works

:material-circle:{ .level-intermediate } Intermediate

### 1 · Frames

| Setting | Meaning |
|---|---|
| **Enabled** | Off parks the frame: kept in the tune, not sent or read |
| **Name** | your own note; the ECU does not use it |
| **ID** and **Standard (11 bit)** / **Extended (29 bit)** | the identifier. 0x100 standard and 0x100 extended are different frames |
| **Bytes** | the payload length, 0–8 |
| **Every** (transmit only) | how often it is sent: 20 Hz is every 50 ms |

A frame only runs while its bus is enabled (**Bus Enabled** on the bus's own page). A transmit frame
on a **Listen Only** bus can never go out: the ECU sets **P1655**.

Frames that share a rate are spread across their period, so the bus sees a steady trickle rather than
a burst. A frame that falls behind skips to the current values rather than sending a backlog of old
ones.
<!-- src: firmware/Can/GenericCan.cpp -->

### 2 · Fields: where the bits are

<figure markdown>
  ![A 16-bit field in bytes 0 and 1: big endian starts at bit 7 with the high byte first; little endian starts at bit 0 with the low byte first](../img/diagrams/can-byte-order.svg)
  <figcaption>Figure 33.1 — The same 16-bit value in the two byte orders.</figcaption>
</figure>

- **Bit numbers** are byte × 8 + bit, with bit 7 the top bit of its byte: byte 0 runs 7 … 0, byte 1
  runs 15 … 8, byte 7 runs 63 … 56. This is the numbering DBC files and CAN tools use, and the grid
  under the table shows it.
- **Format**:
    - **Big Endian (Motorola)**: **Start** is the field's **top** bit. It runs down through the byte
      and carries on at the top of the next byte, so a 16-bit value at Start 7 has its high byte in
      byte 0.
    - **Little Endian (Intel)**: **Start** is the field's **bottom** bit, and the numbers count up, so a
      16-bit value at Start 0 has its low byte in byte 0.
- **Bits**: 1 to 32. Odd widths are fine: a 1-bit flag, a 4-bit gear, a 12-bit reading.
- **Sign**: **Signed** for a two's-complement field that can be negative.

Changing the Format moves Start so the field stays on the same bits. The grid colours each field's
bits and warns when two fields share a bit.
<!-- src: firmware/Can/CanMessageTypes.h; apps/studio-jf/src/ui/GenericCanPanel.h (fmt change keeps the bits) -->

### 3 · Fields: the value

The field sends or reads a **channel**, in the channel's own units (kPa, °C, %, RPM — the studio's
display units make no difference on CAN). The conversion is written once, in the sending direction:

**raw = value × Multiplier + Offset**

and a received field is decoded the other way, value = (raw − Offset) ÷ Multiplier. A document that
gives the decode, such as "kPa = raw ÷ 10 − 101.3", is entered as its inverse: Multiplier 10, Offset
1013.

A value too big for the field is sent as the field's largest value, not wrapped round to a small one.
<!-- src: firmware/Can/CanMessageTypes.h -->

### 4 · Transmit: If Absent

A frame is a fixed set of bytes, so a field whose channel has no value (the sensor is not fitted, or
has failed) still needs something in its bits. **If Absent** decides what:

| If Absent | Sends |
|---|---|
| **Send zero** (default) | raw 0. Through an offset that is usually an obviously wrong reading, which is the point |
| **Hold last** | the last value sent |
| **Skip frame** | nothing: the **whole frame** is not sent this time |

Use **Skip frame** for a value whose zero looks real (a trim, an acceleration) and a receiver that
would act on it.
<!-- src: firmware/Can/CanMessageTypes.h; firmware/Can/GenericCan.cpp -->

### 5 · Receive: Valid For, No Reading and sensors

- **Valid (ms)**: how long a decoded value stays valid. If the frame stops arriving, the channel goes
  missing after this time instead of freezing at its last value. Set it to a few times the sender's
  period (default 500 ms). 0 means it never goes missing, which is almost never what you want.
- **No Reading**: the raw code a sender uses to mean "nothing to report" (a wideband in free air, a
  gearbox between gears). A frame carrying it is ignored for that field, so the value goes missing
  after Valid (ms) rather than reading as a number. Leave it empty if the sender has no such code.
- A field only decodes if the frame that arrived is long enough to hold it.
- Two receive frames may share an id and read different fields of it.
  <!-- src: firmware/Can/GenericCan.cpp -->

**Channel or sensor.** A receive field with a **Signal** writes that channel directly. A field with
no Signal (the ✕) is one a **sensor** reads: set that sensor's **Interface** to **CAN Bus** and pick
the field in its **CAN Field** (chapter 13, step 4). The sensor then adds its own diagnostics and trouble
codes. Only sensors that list **CAN Bus** among their interfaces can do this. Device templates for sensors leave their fields empty on purpose.

**One producer per channel.** A receive field that writes a channel an enabled sensor also produces,
or two receive fields that write the same channel, set **P1656**. The CAN value wins over the sensor
(it is written at a higher **priority**, 10 against the sensors' 0), but fix the clash: switch the
sensor off, or read the field through the sensor instead.
<!-- src: firmware/Can/GenericCan.cpp; definition/ecu.schema.yaml (gc_field.priority default 10) -->

## Before you start

:material-circle:{ .level-basic } Basic

- The bus wired, terminated and switched on at the right bit rate (chapter 13).
- The other device's frame layout: ids, byte order, which bytes carry what, and the scaling. Its
  documentation or a DBC file has these.

## Setting it up

:material-circle:{ .level-intermediate } Intermediate

### Sending a frame

Open **Configuration ▸ CAN Bus ▸ CAN1 ▸ Transmit**.

1. **Add Frame**. Set the **ID**, **Bytes** and **Every**.
2. **Add Signal** for each value. Pick the channel with **⋯**, then set **Format**, **Start**, **Bits**,
   **Sign**, **Multiplier** and **Offset**. Each new signal starts at the byte after the last one.
3. Check the grid: every field in the right bits, and no clash warning.
4. Watch the receiving device, then burn.

![A transmit frame 0x600 with RPM, coolant, MAP and throttle in bytes 0–6](../img/studio/can-transmit.png)

### Reading a frame

Open **Configuration ▸ CAN Bus ▸ CAN1 ▸ Receive**.

1. **Add Frame** and set the **ID** and **Bytes** to match the sender.
2. **Add Signal** for each value: channel (or none, for a sensor to read), bits and scaling as the
   sender documents them. Set **Valid (ms)** from the sender's rate, and **No Reading** if it has one.
3. Check the channel on a gauge or in the log with the sender running, then unplug the sender: the
   channel should go missing after Valid (ms).

![A receive frame 0x650 with a gear field in byte 0, Valid 500 ms and No Reading 0xFF](../img/studio/can-receive.png)

**Remove** takes out every selected frame (shift or ctrl to select several). **Remove Template…**
takes out the frames a template added, as a set.

## Worked examples

:material-circle:{ .level-intermediate } Intermediate

!!! example "Example 1 — four gauges for a dash (Figure above)"
    Frame 0x600, 8 bytes, 20 Hz, big endian:

    | Signal | Start | Bits | Sign | Multiplier | The dash reads |
    |---|---|---|---|---|---|
    | `rpm` | 7 | 16 | Unsigned | 1 | RPM |
    | `clt` | 23 | 16 | Signed | 10 | raw ÷ 10 = °C |
    | `map` | 39 | 16 | Unsigned | 10 | raw ÷ 10 = kPa |
    | `tps` | 55 | 8 | Unsigned | 2 | raw ÷ 2 = % |

    At 3000 RPM bytes 0–1 are 0B B8. At 92.5 °C, bytes 2–3 are 925 = 03 9D.

!!! example "Example 2 — a gear from a sequential gearbox controller"
    Frame 0x650 sent at 20 Hz; byte 0 is the gear, 0xFF between gears. Receive frame 0x650, `gear`
    at Start 7, 8 bits, Valid 500 ms, No Reading 0xFF. Gear Detection (chapter 32) is off, so nothing
    else writes `gear`.

!!! example "Example 3 — a little-endian sender"
    A device documents "oil pressure: bytes 2–3, little endian, 0.1 kPa per bit". Start is the lowest
    bit, byte 2 bit 0 = **16**; Bits 16; Format Little Endian; Multiplier 10; Signal `oil_pressure`.
    The **Oil Pressure** sensor stays switched off, so only the CAN field writes that channel.

## Tuning it

:material-circle:{ .level-advanced } Advanced

- **Bus load** (on the bus's own page): keep it under about 50 %. Lower the rate of frames that
  do not need it; a dash gauge is fine at 10–20 Hz.
- **Refused** climbing: the ECU is trying to send more than the bus takes. Lower rates, or check the
  bus is not in error.
- The ECU sends at most 8 frames per millisecond from this list, so the OBD-II replies that share the
  controller are never starved.
  <!-- src: firmware/Can/GenericCan.h -->

## Diagnostics

:material-circle:{ .level-intermediate } Intermediate

| Code | Meaning | What sets it |
|---|---|---|
| **P1655** | A CAN frame cannot run | A transmit frame on a Listen Only bus; frames or fields beyond the pool (severity 1) |
| **P1656** | Two producers for one channel | A receive field writes a channel an enabled sensor, or another receive field, also writes (severity 1) |

Live channels for each bus: `can1_load_pct`, `can1_tx_fps`, `can1_rx_fps`, `can1_state`,
`can1_tec`, `can1_rec`, `can1_bus_off`, `can1_tx_fail`, `can1_last_err` (and the same for `can2_`).

## Troubleshooting

:material-circle:{ .level-basic } Basic → :material-circle:{ .level-advanced } Advanced

| Symptom | Likely causes | Check |
|---|---|---|
| Dash shows nonsense | Wrong byte order; Start one byte off; wrong Multiplier | The grid against the dash's document; swap Format |
| Dash value is 0 or off-scale | The channel has no value (If Absent Send zero) | The channel on a gauge |
| Received value never appears | Wrong id or standard/extended; frame too short for the field; bus off or wrong rate | The sender's id; Bytes; chapter 13 |
| Received value flickers missing | Valid (ms) shorter than the sender's period | Raise Valid (ms) |
| Received value stays after the sender stops | Valid (ms) is 0 | Set it |
| P1656 | A sensor and a receive field both write one channel | Switch one off, or read the field through the sensor |
| P1655 | Transmit frame on a Listen Only bus | Turn Listen Only off, or park the frame |

## Settings reference

The frame and field settings above are edited on the Transmit and Receive pages. The bus and OBD-II
settings:

--8<-- "reference/settings/_can.table.md"

## Related

- [Chapter 13 — CAN bus](../part2/13-can-bus.md) (wiring, bit rates, templates)
- [Chapter 14 — Vehicle integration](../part2/14-vehicle-integration.md) (OBD-II)
- [Chapter 17 — Sensors and calibration](17-sensors.md) (CAN sensors)
- [Chapter 32 — Vehicle functions](32-vehicle-functions.md) (gear)
