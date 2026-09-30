# Firmware changes

What each firmware version changes, in plain words for the person updating an ECU. The studio shows
the sections newer than the ECU runs before it offers an update, beside the settings the update adds
or retires.

Written WITH the work: a commit that changes something a user would notice adds a line under
Unreleased in the same commit. `make release` turns Unreleased into the next version's section and
raises the version (tools/release_notes.py). A line per change; no commit hashes, no file names.

## Unreleased

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
