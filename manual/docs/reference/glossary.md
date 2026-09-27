# Glossary

The words this manual uses, in the sense it uses them. The chapter in brackets explains each in full.

| Term | Meaning |
|---|---|
| **Advance** | How many degrees before top dead centre the spark fires (20). |
| **AFR** | Air–fuel ratio: mass of air per mass of fuel. |
| **Alpha-N** | Estimating the air from throttle position and speed rather than manifold pressure (19). |
| **Async (injection)** | An extra injector pulse outside the normal schedule, for a sudden throttle opening (19). |
| **Auto Tune** | The studio's tool that proposes VE corrections from the wideband (40). |
| **Bank** | One side of a V engine, or a group of cylinders sharing a lambda sensor (15, 23). |
| **Bootloader** | The processor's built-in USB loader, used to install firmware (47). |
| **BTDC** | Before top dead centre. |
| **Burn** | Store the ECU's working tune so it survives power-off (36). |
| **CAN** | The two-wire network the ECU uses to talk to dashes, sensors and scan tools (13, 33). |
| **Channel** | A live value the ECU publishes: `rpm`, `clt`, `lambda_1` … (reference: Output channels). |
| **Closed loop** | Correcting the fuel from the wideband's reading, as it happens (23). |
| **Connect point** | The copy of the ECU's tune the studio saves at each connect; a restore point (48). |
| **Crank sync / Phase sync** | Knowing the angle within a revolution / also knowing which revolution of the cycle (16). |
| **Dead time** | The time an injector takes to open, added to every pulse (19). |
| **DFCO** | Deceleration fuel cut: no fuel on a closed-throttle overrun (29). |
| **DTC** | Diagnostic trouble code, such as P0117 (44). |
| **Duty** | The share of time a pulsed output is on, in % (18). |
| **Dwell** | How long an ignition coil charges before the spark (20). |
| **ETB** | Electronic throttle body: a throttle moved by a motor (22). |
| **Expression** | A short formula typed into a setting, such as `clt > 95` (34). |
| **Freeze frame** | Engine speed, MAP, coolant and battery recorded when a code went active (44). |
| **Fuel Load** | The load value the VE, Target Lambda and Advance tables are read against (19). |
| **Generic output** | An output whose conditions and value you choose (18). |
| **H-bridge** | An output that drives a motor both ways (7, 22). |
| **Hall sensor** | A trigger or speed sensor with a switched, square output (11). |
| **High-side / low-side** | An output that switches a load to 12 V / to ground (7, 12). |
| **Key on / key off** | Whether the ECU sees battery voltage: on above 8 V, off below 7 V (10). |
| **Kit** | A firmware image with its meta and pages, for one board (46). |
| **Knock** | Part of the mixture exploding after the spark; it can break pistons (30, 41). |
| **Lambda** | Actual AFR ÷ the stoichiometric AFR: 1.00 is exact, below 1 rich, above 1 lean (37). |
| **Layout** | The studio's pages, tree and tabs for an ECU (49). |
| **LTFT / STFT** | Long-term (learned) and short-term fuel trim (23). |
| **MAP** | Manifold absolute pressure, in kPa; about 100 at atmosphere (17). |
| **MBT** | Minimum advance for best torque (37). |
| **Meta** | The file that tells the studio what settings and channels a firmware has (48). |
| **Noise floor** | What a cylinder normally sounds like to the knock sensor, learned (30). |
| **OBD-II** | The standard scan-tool interface (14). |
| **Pre-ignition** | The mixture lighting before the spark (30). |
| **Protection level** | The reaction to a trouble code of a given severity (29). |
| **PWM** | Pulse-width modulation: an output switched on and off fast, its duty setting the average (12). |
| **Restore point** | See *Connect point*. |
| **Sensor type** | What a sensor measures, and in what units (17; reference: Sensor types). |
| **Sequential / semi-sequential** | Each injector fired once per cycle at its own cylinder / in pairs, twice per cycle (19). |
| **Setting** | A value stored in the tune (reference: Every setting). |
| **Speed-density** | Estimating the air from manifold pressure, speed and temperature (19). |
| **Stoichiometric (stoich)** | The AFR that burns all the fuel with all the air: 14.7 for petrol (19). |
| **Surface** | A tab of pages or instruments in the studio (4, 49). |
| **TDC** | Top dead centre. |
| **Trigger offset** | The angle from the trigger's reference to cylinder 1's TDC (16). |
| **Tune** | Every setting and table of an ECU (48). |
| **VE** | Volumetric efficiency: how well a cylinder fills, in % of its volume (19, 37). |
| **VR sensor** | A two-wire magnetic trigger or speed sensor with a sine-wave output (11). |
| **Wasted spark** | One coil firing two cylinders, one on compression and one on exhaust (20). |
| **Wideband** | A lambda sensor and controller that measure the mixture over a wide range (11, 23). |
