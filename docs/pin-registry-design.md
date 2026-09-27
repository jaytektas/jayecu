# Pin-ownership registry — design sketch

**Status:** design sketch (not wired). Convergence point for the two pin-arbitration
mechanisms that exist today, and a piece of [modular-platform-architecture].

## Problem

Two mechanisms decide "who owns this pin," on disjoint pools, and they can't see each other:

- **`PinArbiter`** (`firmware/Scheduler/PinArbiter.h`) — *output* pins (IGN/LS coils, injectors,
  boost, fans). Owns pin **state**: every pin Hi-Z until `claim()`d, driven by the owner, forced
  back to Hi-Z on `release_owner()`. Real-time, timer-channel backed.
- **`Sensors::compute_conflicts()`** — *input* pins (analog AV/AT, digital DIG). Gates **who reads**:
  first claimant wins, later ones `arb_rejected_` (skipped in `update()`), `input_conflict_` → P1650.
  Config-time, re-derived live on `g_config_generation`.

They solve different jobs (driving vs interpreting) on board pools that are disjoint by `caps`, so
they're not duplicates. But neither can catch a **cross-pool clash** — the same physical pin claimed
as *both* a sensor input and a coil output. On this board that's unlikely (caps separate them); on a
flexible-GPIO board it's real, and today it slips through both.

The registry is **not** "one mega-arbiter." It's a thin **ownership ledger** both register claims
into. Domain logic stays put: `PinArbiter` still drives outputs (Hi-Z/enable), `Sensors` still reads
ADCs. The ledger only answers *"is physical pin P free, and who holds it?"* — one source of truth,
cross-pool conflict detection, and the thing the studio Routing dock queries to enforce uniqueness.

## Pin identity — one global id space

The cross-pool catch only works if every pool's index maps into ONE id space. The board profile
already gives every pin a stable slot; codegen exports a **global board-pin index** per pin, plus a
map from each pool index to it:

```cpp
using PinId = uint8_t;                          // 0 .. BOARD_PIN_COUNT-1; one per physical pin
static constexpr PinId PIN_NONE = 0xFF;

// Generated from the board profile (board_codegen): pool index -> global PinId.
//   ANALOG pool (AV1-16 -> 0..15, AT1-4 -> 16..19), DIGITAL (DIG1-8 -> 0..7), IGN[], LS[] ...
extern const PinId BOARD_ANALOG_PIN[BOARD_ANALOG_V_COUNT + BOARD_ANALOG_T_COUNT];
extern const PinId BOARD_DIGITAL_PIN[BOARD_DIGITAL_IN_COUNT];
extern const PinId BOARD_IGN_PIN[MAX_IGN_CHANNELS];
extern const PinId BOARD_LS_PIN[MAX_INJ_CHANNELS];
```

So `Sensors` claims `BOARD_ANALOG_PIN[c.source]`, `PinArbiter` claims `BOARD_IGN_PIN[idx]`, and if
those ever resolve to the same `PinId`, the registry rejects the second — across pools.

## Owner identity

A (function, instance) token, cheap to compare. Replaces `PinArbiter`'s `PinOwner` enum and the
sensors' implicit "lowest index wins":

```cpp
enum class PinFn : uint8_t { NONE, COIL, INJECTOR, SENSOR, TRIGGER, BOOST, TACH, AUX };
struct PinOwner {
    PinFn   fn   = PinFn::NONE;
    uint8_t inst = 0;                           // which coil / sensor / cam
    bool operator==(const PinOwner& o) const { return fn == o.fn && inst == o.inst; }
    bool valid() const { return fn != PinFn::NONE; }
};
```

## The registry interface

```cpp
class PinRegistry {
public:
    static constexpr uint8_t MAX_PINS = BOARD_PIN_COUNT;   // derive from the board, never hardcode

    // Reconfigure pattern (mirrors PinArbiter): release_all() / release(owner), everyone re-claims,
    // a pin dropped from the new map is simply never re-claimed and goes FREE.
    void release_all() noexcept;                // all pins FREE; call before a full claim sweep
    void release(PinOwner owner) noexcept;      // free every pin this owner holds

    // Grant pin P to `owner`. True if FREE, or a re-grant to the SAME owner. False if held by a
    // DIFFERENT owner (conflict) — owner_of(P) then names the incumbent. PIN_NONE is a no-op true.
    [[nodiscard]] bool claim(PinId p, PinOwner owner) noexcept;

    PinOwner owner_of(PinId p) const noexcept;  // {NONE} if free / out of range
    bool     is_free(PinId p)  const noexcept { return !owner_of(p).valid(); }

    // Diagnostics: was any claim rejected since the last release_all()? + iterate the clashes so
    // each loser can raise its DTC (sensor -> P1650, output driver -> P0645-range).
    bool any_conflict() const noexcept;
    template <class Fn> void for_each_conflict(Fn&&) const;   // (PinId, incumbent, rejected_owner)

private:
    PinOwner owner_[MAX_PINS];                   // FREE = {NONE}
    // a small conflict log (PinId + the two owners) for for_each_conflict / DTC reporting
};
```

`claim()` is the whole contract: free → grant, same owner → idempotent re-grant, other owner →
reject + log. No pin driving, no ADC — that's the caller's job.

## How the two current mechanisms fold in

**Output (`PinArbiter`)** keeps `claim(Class, idx, owner) -> ITimerChannel*` as its public face, but
internally asks the registry first:

```cpp
ITimerChannel* PinArbiter::claim(Class cls, uint8_t idx, PinOwner owner) {
    const PinId p = (cls == Class::IGN) ? BOARD_IGN_PIN[idx] : BOARD_LS_PIN[idx];
    if (!reg_->claim(p, owner)) return nullptr;          // registry says taken -> conflict (Hi-Z)
    return channel(cls, idx);                            // still Hi-Z; owner configures/drives it
}
```

**Input (`Sensors::compute_conflicts`)** drops its local `analog_owner[]/digital_owner[]` and claims
into the shared registry instead — so the loser is `arb_rejected_` exactly as now, but a clash with
an *output* pin is also caught:

```cpp
reg_->release(PinOwner{PinFn::SENSOR, 0xFF});            // free all sensor claims (sweep)
for (i in sensors, enabled) {
    PinId p = (analog) ? BOARD_ANALOG_PIN[c.source] : BOARD_DIGITAL_PIN[c.source];
    if (!reg_->claim(p, PinOwner{PinFn::SENSOR, i})) arb_rejected_[i] = true;   // lost the pin
}
input_conflict_ = reg_->any_conflict();                 // -> P1650
```

Sweep ordering still gives "lowest catalog index wins" for sensors; cross-pool order is whoever
claims first at composition (the scheduler binds outputs before the sensor sweep runs).

## What does NOT change

- **Domain behavior.** Outputs still Hi-Z/enable via `PinArbiter`; inputs still ADC-read in `Sensors`.
  The registry is ownership-only.
- **The DTC path.** `any_conflict()` feeds the existing pin-conflict signal (`epos_hal_.pin_conflict()`
  / `sensors_.input_conflict()` → P1650). Could later split: P1650 input clash vs a P0645-range
  output-driver clash, surfaced from `for_each_conflict`.
- **The studio.** The Routing dock does the *config-time* equivalent in Python over the same board
  pin map — so what it prevents and what the firmware enforces are the same `PinId` space. One truth.

## Open choices (decide when wiring)

- **Static array vs the existing per-pool masks.** `owner_[MAX_PINS]` is simplest; `MAX_PINS` is small.
- **Single registry instance.** One `g_pin_registry` at composition root, handed to `PinArbiter` and
  `Sensors` (like `g_pins` is handed in today).
- **Re-claim trigger for inputs.** Already solved: `g_config_generation` drives the sensor re-sweep;
  the registry release/claim happens inside `recompute_live()`.
