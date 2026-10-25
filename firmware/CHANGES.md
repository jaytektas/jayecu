# Firmware changes

What each firmware version changes, in plain words for the person updating an ECU. The studio shows
the sections newer than the ECU runs before it offers an update, beside the settings the update adds
or retires.

Written WITH the work: a commit that changes something a user would notice adds a line under
Unreleased in the same commit. `make release` turns Unreleased into the next version's section and
raises the version (tools/release_notes.py). A line per change; no commit hashes, no file names.

## Unreleased
- **Overlapping injector pulses no longer cut each other short.** When an async transient squirt landed
  while an injector was in its normal pulse, the injector closed at the squirt's earlier close and the
  rest of the normal pulse was lost: on the bench, a 1 ms squirt cut an 8 ms pulse to 3 ms. That cylinder
  ran lean for the cycle, on exactly the tip-ins the squirt was meant to help. The same happened to a
  normal pulse starting during a squirt, and with the cold-start prime. An injector now stays open until
  the later of its closes.
- **A lift no longer throws away the tip-in enrichment.** With disenrichment off, the enrichment was set
  to zero the moment the throttle started to close, while MAP (which lags the throttle) was still at its
  peak. A quick stab and lift went briefly lean: λ 0.99 to 1.34, about 20 AFR on the 14.7 scale. The lift
  still ends the event, so it cannot grow or re-peak, but what it added now bleeds off at the Enrich Decay
  rate.

## 0.4.21
- **Key-on is a power-on.** With USB holding the ECU up, key-off used to leave every module and sensor as
  it was, and the next key-on carried on from there. Now key-off puts every module back to its power-on
  state and key-on re-reads every sensor from scratch. Only what survives a real power loss carries over:
  the tune, learned trims, knock learning and stored fault codes.
  - Found on a car: the battery terminal let go under the starter mid-crank. At key-on the start switch
    still read "pressed" for its first few samples, the start sequence took that for a press, and the
    engine cranked with nobody on the key.
- **A start press has to be seen.** The start button must read released with the key on before any press
  counts, with Engine Start on or off. A button already pressed at key-on (held, stuck, or stale) never
  starts a crank: let it go and press again.

## 0.4.20
- Trigger inputs use only the edges selected in Edge. A pulse-width cam (the 2JZ / 1JZ front or rear cam, and every
  "+ cam" wheel) used to capture both edges whatever Edge said, and timed itself from the pulse's closing edge.
  On a VR input that edge is where the signal crosses a threshold re-armed from the last pulse, so it moves with
  every change of speed and comes late after a weaker pulse. A quick stab through the cam's phase-sync rpm read
  that as a cam that hadn't arrived: phase lost (P0341), a moment on wasted spark, then a re-sync. Now:
  - With Edge on Rising or Falling, only that edge is captured, and each one is the cam's reference. On a VR
    input that's the zero crossing, which sits on the tooth. WIDTH min/max don't apply.
  - With Edge on Both, the width is measured and picks out the reference, which is timed from the rising edge.
  The VVT angle is measured at the same edge.
- VVT cam angles (VVT Cam 1-4 Advance) are published in degrees. They were in tenths of a degree, so a cam 4.3°
  from nominal read 43°, and cam control would have closed its loop on a value ten times too large.

## 0.4.19
- Engine Start has its own switch, Start Sequence Enabled, like every other feature: at the top of its page and
  on the Configuration and Engine Functions switchboards, and the page shows in the tree only while it is on.
  It is off by default; off, Crank Request is the start button itself, so a starter output reading it cranks
  as before. If you set up Engine Start on 0.4.18, switch it on again after updating. Its fuel settings grey
  out while Prime Before Crank is off.

## 0.4.18
- New start sequence (Configuration ▸ Engine Functions ▸ Engine Start). A press of the start button is held
  until the engine is fit to crank: every electronic throttle has passed its key-on check, and the fuel pump
  has brought the rail up — to 250 kPa when a rail pressure sensor is reading (at once if it is still up;
  cranking anyway after 3 s), or for 1 s when there is none. A rail that bled down after the key-on prime
  used to meet the first crank, and the prime pulse, empty. The starter output now cranks on Crank Request
  (`crank_req`) and the fuel pump also runs on Start Pump (`start_pump`): run the output wizard on the
  Starter Motor and Fuel Pump outputs to take the new conditions (an output set up before keeps reading the
  button). A tap is enough; holding the button still gives another crank after the rest. New channels
  Start Sequence, Crank Request and Start Pump are in the default logs. A start that had to crank on a low
  rail raises P0087 (level 1, cleared by the next start that reaches pressure) and the console says what
  pressure it got to; a press dropped after the wait says what it was still waiting on.

## 0.4.17
- The engine can no longer start on an electronic throttle that hasn't passed its key-on check. Fuel and spark
  stay off while it cranks, whatever is turning it; a check cut short by cranking runs again as soon as the
  engine stops, and every key-off starts the check from scratch. Before, cranking during the check left the
  throttle dead for that key cycle (high idle on its rest opening, no pedal, no fault code). An engine that is
  already running is never cut by this. Gate the starter output on the throttle being ready as well
  (`etb_state_1 == 2`) so the starter waits for the check instead of cranking against it.
- Lifting off the throttle no longer adds fuel. With disenrichment off, the tip-in's enrichment carried on
  through a lift and, when the throttle settled closed, came back at full strength: a rich stumble after every
  lift. A lift now ends the tip-in.
- Each new push of the throttle gets its own tip-in fuel, judged from where that push started. A small
  throttle movement on boost was enriched as if it were a stab from idle, because the enrichment remembered
  where an earlier stab began: far too much fuel and a rich stumble.
- The closed-loop fuel trim now holds still during throttle transients and wall-film corrections, instead of
  chasing the brief rich or lean reading they cause and leaving the mixture off afterwards.
- The closed-loop fuel trim starts from zero after the engine stops, so a restart doesn't begin on the last
  run's correction.
- Log files on the SD card now carry the ECU clock's date and time (the clock Studio sets on connect)
  instead of all reading 1 January 2026.

## 0.4.16
- Vehicle Speed is in the default datalog. It never was, so no log recorded road speed even on a car that has one.
- A custom datalog channel set can now include every channel. The list had room for 456 and there are 519, so the newest 63 (injector duty, lambda overall and per bank, the wastegate and CAN health channels among them) could not be chosen, without any warning. An update keeps the channels you had picked.
- Launch control arms only once the engine is running. Cranking with the pedal down and the car still (how flood clear is asked for) also met launch's built-in rule, so the launch fuel map was applied to the cranking fuel and the launch timeout started before the engine caught.

## 0.4.15
- Stored trouble codes are loaded from the SD card only if this firmware wrote them. A card that had been in an ECU running other firmware could bring back dozens of nonsense codes as stored history, and with them a protection level (one board came up in level 3 limp until the codes were cleared). Such a file is now discarded at power-up, the console says so, and the ECU starts with no stored codes. Codes saved by earlier versions of this firmware still load, and are saved in the checked form from then on.

## 0.4.14
- Proteus F7: the Fuel Learn Block channel (above) is published on this board too.
- Overboost (P0234) is recorded at level 1, not level 3. The overboost cut is its reaction; a level-3 limp on top meant every pull that touched the cut ended in a rev limit. Level 3 is left to faults a cut cannot fix: over-temperature, pre-ignition, EGT, a lost trigger.
- A MAP reading that drops out is held at its last good value for MAP Failure Hold (default 500 ms) before it counts as failed. It used to be fuelled as atmosphere at once, so a dropout on a closed-throttle overrun was a full-load pulse (lambda 0.7 on a lift-off). After the hold, MAP Failure Source chooses atmosphere (as before, the default) or the Predicted MAP table. MAP Source now also reads Held and Failed (predicted). An update adds both settings at their defaults.
- New channel Fuel Learn Block: whether the mixture the wideband is reading now can be learned from, with one bit per reason (MAP not measured, wideband, temperatures, fuel or ignition cut, transient, starting, crank-only sync, protection, rail pressure, and settling for one lambda delay after each). The long-term trim learns only when it is 0, and Auto Tune now rejects readings the ECU marks, naming the reason on its status line. It is in the default logs.
- Charge Air Mass is logged up to 6500 mg (it wrapped at 655 mg, so boost logs showed nonsense).
- Cam sync is no longer lost every time rpm crosses the bottom of the Phase Sync rpm band. Below the band the cam is ignored and phase is held by counting crank teeth, but the cam's "overdue" check kept counting too, so the first tooth back inside the band found it hundreds of teeth late and dropped to crank sync (P0341) on a healthy cam.
- Cam sync is filtered on engines whose crank wheel has a gap or mark. A cam edge counts only if it lands where the cam should be within the revolution (Nominal Angle, give or take its band) and exactly one engine cycle after the last one, and phase is granted on the second such sighting. A noise spike on the cam wire is ignored and counted as a noise edge. It used to be believed, and could lock the engine 360° out: spark on the overlap stroke, a stumble and a backfire. A cam that sits in the same wrong place two cycles running still raises phase lost. Phase now takes one engine cycle longer to reach. Set Nominal Angle to the cam's real position to get the full benefit.
- Starter Motor output template: new Release Above setting (default 700 rpm). The starter now lets go once the engine is running and above that speed, instead of at the moment it first counts as running (~450 rpm), so a warm restart is not left to stumble. Run the output wizard on a starter output to take it.
- H-bridge carrier: the default is now 3 kHz (was 20 kHz) and it is capped at the bridge chip's 20 kHz rating. The IFX9201 switches slowly, so its switching loss rises with frequency; at 20 kHz, holding a throttle at idle on a hot day tripped its over-temperature shutdown and dropped the throttle to its spring. An existing tune keeps the carrier it already has (above 20 kHz it is held at 20 kHz) — set 3000 Hz on the H-Bridge page to take the new default.
- Auto Tune ignores readings with the wideband pinned at its lean end (lambda above 1.3): misfire, overrun or free air, which it used to learn as "add fuel".
- Electronic throttle: a key-on check that passes now clears a latched throttle fault. It drives the plate to both stops and checks them against the calibration, so a throttle that has just proved itself is no longer left dead at every key-on until a Find limits. A check that fails still latches, and the cleared code stays stored with its count.
- Idle on drive-by-wire: the closed loop no longer winds its trim below the duty where the throttle's Min TPS stops the plate. It used to keep pulling the trim down while the plate sat on Min TPS, so lowering Min TPS let all of it through at once and could stall the engine.
- Idle: air the closed loop added is let go once the engine is above its target with the loop off — at once off idle, within a second or two while still idling. It used to be held, so a start that stumbled wound the throttle open and the engine, once it lit, ran away on that air (one cold start reached 4700 rpm).

## 0.4.13
- Fuel pressure from a sensor is read as gauge pressure, as a rail sender reports it. The differential across the injectors subtracted the manifold's absolute pressure from a gauge reading, so a 300 kPa rail read as about 200 kPa and every injector pulse came out about a quarter long. A tune whose fuel pressure sensor was calibrated to read absolute needs its calibration taken back to gauge.

## 0.4.12
- With the key off the ECU is quiet on CAN: nothing is transmitted and anything still queued is dropped, so an ECU kept awake by USB does not talk into a sleeping car or keep its other modules awake. It still listens, and starts transmitting again at key-on.
- CAN1/CAN2 "cannot complete a frame" (P1657/P1658) is no longer logged with the key off, or in the first two seconds after key-on. With the ignition off the rest of the car's bus is asleep, and at key-on it is still booting, so nobody answers; with USB connected the ECU logged it at key-off and again at power-up.
- No more phantom trouble codes when the ignition is switched off with USB connected (Boost: MAP signal missing, Transient Throttle: load-source signal missing, and the like). The key is now read every millisecond; it used to be read 20 times a second, and for those few frames every sensor was dead with the key still on.
- The fuel pump prime and the check engine light's bulb check are timed from key-on, not from when the ECU powered up. With USB connected the ECU is already awake, so the pump did not prime. Run the output wizard again on a Fuel Pump or Check Engine Light output to take the change. The new channel is Time Since Key On (`key_on_s`).
- Every trouble code has a description. The codes a sensor gets from the manufacturer range (for example P1000, the battery's circuit-low code), its precondition and band codes, and the flex fuel temperature's codes showed with no text.
- The flex fuel sensor's temperature can be calibrated: its page has a Fuel Temperature section with Measure (High Time, Low Time or Duty Cycle), the raw value live, and its own curve. The default is now Low Time, which is how a GM/Continental sensor sends it — reading the high time instead gave a temperature that was wrong and moved with the ethanol content (one car read 134 °C at 20 °C and raised P0183). A raw past either end of the curve raises P0182/P0183.

## 0.4.11
- The Trigger Library has 2JZ 36-2 + Front Cam, for a 2JZ whose cam signal comes from the front (G1) sensor instead of the rear (G2) one. It is the rear-cam wheel with the cam a crank revolution on, as Toyota places G1.

## 0.4.10
- Temperature sensors can be calibrated as thermistors: pick the sensor (GM, Bosch/Hella NTC, Chrysler/Dodge, Ford, Toyota, Mazda Miata, Mercedes M111) or enter three temperature/resistance points from its data sheet, and the ECU converts every reading exactly. Offered on temperature sensors and Aux inputs on analogue pins; existing voltage curves are kept until you switch.
- Temperature inputs are read at 50 Hz instead of 5 Hz, and the fuel model reads air and coolant temperature as often as it calculates fuel (every engine cycle).

## 0.4.9
- Cranking fuel moved to Fuel Tuning ▸ Corrections ▸ Cranking, beside Post Start and Coolant Temp: it is a correction like them. Like them, it shows in the tree only while it is switched on.
- The Overall Fuel Trim now has its on/off switch beside it on Fuel Setup and on the Fuel Tuning page, and greys out while it is off. The switch used to be only on the Corrections page, so a trim set on Fuel Setup could be silently ignored.
- Two more tachometer set-ups: fixed pulse width (for tachos that read a coil-style pulse) and RPM as duty on a fixed carrier (for dashes and converters). The plain tachometer now asks for its duty. Needs studio 0.3.16.
- The flex fuel sensor's two readings have their own names: Flex Fuel Ethanol and Flex Fuel Temperature. Its page shows both, and says to set the interface to Frequency (it reads both from the one pin).
- Sensor Diagnostics pages: each check's explanation now sits under its row instead of being drawn over the check's name, threshold and severity.
- Boost Control Correction pages: Applies To is no longer hidden behind the Reads picker. Knock Control: the Auto note no longer runs into the Sensor Health panel.
- Drop-downs that cut off their longest choice are wider: Knock Input ("Auto (from cylinder bank)"), pedal Track Sense, Boost Output Mode, Traction Cap, the Coolant and MAP sources on Fuel Setup (its MAP row is now called MAP), and the Transient Throttle pressure source.

## 0.4.8
- The trigger stream pages no longer have a wheel picker. It could show the wrong wheel (two wheels with the same crank pattern, like the 2JZ with and without its cam, looked the same to it) and it only set one stream. Install a wheel from the Trigger Library, which sets every stream at once.

## 0.4.7
- A Lua script or a CAN template that writes 0 to Fuel Cut or Ignition Cut no longer cuts the engine, and it cannot switch off a cut the ECU is making either (rev limiter, engine protection): writing 0 there is ignored. Writing 1 still adds a cut. Before, any write counted as a cut, and a script write with no timeout held it until the script was reloaded.
- The rev limiter, launch and traction control soft cuts now cut the share they report. Each cut decision was held one frame too long, so a 50% soft cut cut about 60%.
- VVT and knock learned values stay in their own cells when the VVT coolant axis or the knock rpm axis is resized. Before, shrinking the axis moved each cam's (or each load row's) learned values into another's cells, and the studio showed different cells from the ones the ECU used. Values learned while the axis was shrunk may be in the wrong cells; clear them and let them re-learn.
- Throttle feed-forward table: with the Y axis turned on, rows after the first now hold what the studio shows. The ECU and the studio used to lay those rows out differently. Re-run the feed-forward sweep (fillff) for any row past the first.
- Taking the SD card back from a PC (key on) no longer cuts into a long USB copy partway through a transfer.

## 0.4.6
- Drive-by-wire pedal: turning the key off and on no longer leaves the pedal faulted (P1780 "pedal A missing", no throttle until the ECU restarts). The pedal is not judged with the key off, a missing pedal track has to stay missing for APP Match Debounce before it becomes a fault, and pedal calibration is cancelled if the key goes off mid-sweep instead of saving a calibration read with the key off.
- Cruise control: turning the key off and on no longer puts cruise into Fault with a vehicle-speed code that needed a button press to clear. Key-off now turns cruise off, and key-on starts it in its power-on state.

## 0.4.5
- Knock: the window now opens Window Start before each cylinder's own spark (default 10°), so it follows the timing and always covers the moment before the spark, where pre-ignition shows. Before, it opened at a fixed angle after TDC, after the spark, so pre-ignition could only be caught by its loudness. Window Duration now defaults to 80°, so the window still reaches well past TDC for knock. Both are new settings: updating drops a tune's old Window Start and Window Duration and uses the new defaults (an old 40° duration would now end the window near TDC, before the knock).
- After a firmware update the studio no longer says the ECU has no real-time clock. The clock kept time, but the ECU could not read it until its next restart, so the first connect after an update left it unset.
- The ECU no longer restarts (reported as a stack overflow in 'MSC') when a PC writing to its SD card over USB falls out of step with it, as when a write stalls part-way.

## 0.4.4
- Fuel Tuning ▸ MAP Prediction & Fuel Film: the readouts under the Predicted MAP and Transient TPS Scaling tables no longer run off the bottom of the page.
- The knock event count no longer stops at 65535. It keeps counting, and the Knock Event Count channel wraps back to 0 after 65535, so a count taken before and after still shows the knocks in between.
- Turning the key off and on with the engine still turning no longer raises P0335 (sync lost) or P1750 (knock sensor missing). Key off counts as a stop, and a knock sensor is judged only after the engine has run over the knock learn floor for a second.
- The Blend air model is two maps: an Alpha-N VE table (RPM × throttle) below Blend Start RPM, the VE table on measured MAP above Blend End RPM, and their air masses crossfaded between. Each range has its own cells, so tuning low-RPM fuel no longer moves the cells the upper range uses. It no longer reads the Predicted MAP table, which is now used only by MAP prediction, during throttle transients. A Blend tune needs its Alpha-N VE table filled in after updating.
- New Charge Load channel: the air in the cylinder as a % of a full charge, from every air model and smooth through Blend's crossover. Changing the Air Model offers to point Target Lambda and the Ignition map at the load that model suits (Charge Load for Blend and MAF), converting their load rows.
- The VE autotune leaves the VE table alone where Blend's Alpha-N map carries the charge.
- A failed or disabled MAP sensor no longer fuels from the Predicted MAP table (which was used even with MAP prediction switched off). It reads atmosphere and raises its fault; MAP Source says "Failed (baro)". Predicted MAP is only ever used during a throttle transient, with prediction on.
- Fuel is per injection stage. Each stage has its own stoich ratio, its own ethanol content (fixed, or measured by a flex sensor that several stages can share), its own fallback when the sensor has not read, its own fuel density table and its own fuel-composition correction. A secondary stage can run a different fuel from the primary; the firmware mixes them by the fuel each delivers. The old single Ethanol Source, Stoich AFR, Specific Gravity and Fuel Composition settings move to Stage 1 — check them after updating.
- Each stage has a Fuel page (Fuel Tuning ▸ Stage N ▸ Fuel) with those settings and tables together.
- A protection fault that stays true (boost held over the limit, coolant or air temperature held over its cut) now stays active for as long as it lasts. It used to go inactive about a second in, while the engine was still over the limit.
- Readings everywhere show at your unit's precision: MAP and fuel load read 0.0 kPa, not 0.
- Diagnostics: the Fuel & Spark view is two views, Fuel and Spark, and every channel now has a place (charge ethanol and MAF failover had none).
- Every tick box's name sits level with its box (1,835 rows across the pages, text was 1-4 px high).
- Fuel Setup: Ethanol If No Reading is a number you can enter again; it was an empty drop-down.
- The fuel film (port wall wetting) uses two tables instead of two single numbers: Film Pooling Percentage (coolant × MAP) and Film Evaporation Time Constant (RPM × coolant). It runs per injection stage, and each stage has a Port Film switch so a direct-injected stage has no film. The old Deposit Fraction and Evaporation Tau are retired — fill in the tables after updating.
- MAP prediction's Transient TPS Scaling table can have up to 16 throttle rows. Below the table's rate, prediction now scales straight with the throttle rate (half the rate, half way to predicted MAP); it used to ignore anything under a quarter of the rate (now under a tenth, as noise).
- Transient TPS Scaling's default rises with RPM, from 150 %/s at 500 rpm through 300 at 2000 to 700 at 6000 (was 100 everywhere): at speed the MAP sensor lags less, so it takes a faster stab to need prediction.
- MAP prediction can also predict a fast throttle LIFT (Predict Tip-Out, off by default): the estimate moves down towards Predicted MAP, as a tip-in moves it up.
- A throttle sensor that drops out no longer reads as the pedal snapping shut and then stabbing: MAP prediction is not triggered by a TPS dropout.
- Output wizard: a Check Engine Light — on for any current trouble code or protection level 3; once the codes have cleared, a level still holding flashes it (fast for level 2, slow for level 1). Bulb check at key-on; it fails lit if the ECU cannot read its state.
- New channels: Protection Level (0–3, the level in force, held levels included) and two flashers (1 Hz, 4 Hz) any output condition can use. The studio's trouble-code dock shows the protection level in force.
- With the key off (or the ECU only on USB), coil and injector outputs are no longer driven: their pins are released to high impedance, like every other output, and claimed again at key-on. They used to be held driven low whatever the key said.
- The studio greys out settings that only apply when the engine stops (the trigger streams, the engine's shape, an output's coil or injector assignment) while it is connected and the engine is turning. Trigger Offset BTDC and the full-sync RPM band stay editable: the ECU reads them at once.
- The bench output test needs the key on: with the key off it is refused and a running test stops (nothing is driven with the key off). On the output pages Test is greyed out unless the key is on and the engine stopped; Stop and Stop All always work.
- Output wizard: the selected template's highlight no longer runs under the list's scrollbar.
- Knock no longer reports its sensor missing (P1750) while fuel or spark is cut: with no combustion no knock window is sampled, so a protection cut after another fault raised P1750 as a false second fault.
- Closed-loop O2 holds its trim while MAP prediction is active; the new Closed-Loop Hold channel says why the trim is held (fuel cut, after cut, settling, cold, off range or transient).
- Fuel Tuning ▸ MAP Prediction & Fuel Film holds the transient strategy's switches and all four of its tables. One button chooses it (and turns Classic Transient Fuel off); Transient Throttle has the matching button, and both pages warn while the two strategies are on together.
- Fuel Tuning ▸ Corrections points to the fuel composition correction, which is per stage (Stage N ▸ Fuel).
- The MAP fuel correction table (Fuel Tuning ▸ Corrections ▸ MAP) is removed. It was a MAP × RPM trim on top of the VE table, indexed on measured MAP rather than the fuel model's load; put anything it held into the VE table or a Generic correction.
- Each stage's fuel-composition correction has up to 8 load rows (was 16), to make room for the film tables.

## 0.4.3
- Diagnostics ▸ Sensors — Raw: a pin that is not on a connector says so on one line instead of being cut off.

## 0.4.2
- Diagnostics shows every reading in the units you chose in Preferences (psi, °F, a raw pin in volts,
  millivolts or counts), at that unit's precision. A raw analog pin read "3" beside "ADC"; it now reads
  "3.214 V".
- Diagnostics ▸ Sensors — Raw shows each input pin's connector and wire colours beside its reading, and
  a digital pin's frequency, pulse width, SENT and level across one row.

## 0.4.1
- Output test: the repeat count goes up to 100,000, and a long test is no longer stopped after
  10 minutes — it runs for as long as its count and timings say.
- Output test: the Count, On Time and Off Time boxes show their units and fit the larger count.

## 0.4.0
- First public firmware.
