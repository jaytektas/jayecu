#pragma once
#include "ITimerChannel.h"
#include "SchedulerTypes.h"

// ---------------------------------------------------------------------------
// PinArbiter — single-owner arbitration over the board's output pins.
//
// ONE POOL, ONE INDEX SPACE: the pool is the output rows (outputs.output[i] IS pin i — the ignition
// pins, then the low-side, then the high-side; see OutputMap.h). A row can only hold one function, so
// the tune cannot ask for two owners on one pin; what the arbiter still guards is the moment in between —
// a row edited from a generic output to a coil is released by one owner before the other claims it,
// and a reconfigure that has not happened yet must not have both driving.
//
//   * Every pin starts (and ends, when released) in Hi-Z — an unowned pin never drives a line.
//   * claim(row, owner) grants a FREE pin (or re-grants to the same owner) and returns the channel STILL
//     in Hi-Z. The owner then configures it — the arbiter does not presume the pin's use.
//   * A pin already owned by a DIFFERENT owner → claim returns nullptr (conflict recorded).
//   * release_owner(owner) forces every pin that owner holds back to Hi-Z and marks it FREE — the
//     arbiter, NOT the module, guarantees the safe boundary state.
// ---------------------------------------------------------------------------

// TEST is the bench output test (OutputTest.cpp). It is an owner like any other precisely so a test
// cannot race the scheduler: a pin the firing layer holds is REFUSED and the conflict recorded, rather
// than two writers fighting over a coil. It claims for the length of one test and releases.
enum class PinOwner : uint8_t { FREE = 0, IGNITION, INJECTION, AUX, TEST };

class PinArbiter {
public:
    // Generous fixed bound; bind() clamps to the populated count (jaytek_v1: 42).
    static constexpr uint8_t MAX_PINS = 48;

    // Bind the output pool, in row order. All pins start FREE/Hi-Z. Call once at composition.
    void bind(ITimerChannel** pool, uint8_t n) noexcept {
        pool_ = pool;
        n_    = (n <= MAX_PINS) ? n : MAX_PINS;
        for (uint8_t i = 0; i < MAX_PINS; ++i) owner_[i] = PinOwner::FREE;
        clear_conflict();
    }

    [[nodiscard]] uint8_t size() const noexcept { return n_; }

    // Grant row `row` to `owner`. Returns the channel (still Hi-Z) if free or already this owner's;
    // nullptr on conflict / out of range / unpopulated.
    [[nodiscard]] ITimerChannel* claim(uint8_t row, PinOwner owner) noexcept {
        if (row >= n_ || !pool_ || !pool_[row]) return nullptr;
        if (owner_[row] != PinOwner::FREE && owner_[row] != owner) {
            record_conflict(owner, owner_[row]);
            return nullptr;
        }
        owner_[row] = owner;
        return pool_[row];
    }

    // BORROW one pin from whoever holds it, and say who that was so it can be handed back.
    //
    // This exists for the bench output test and nothing else. A coil's pin is claimed by the firing
    // layer for as long as the tune says that row is a coil — which is the correct behaviour, and it
    // means claim() can NEVER grant it. A test that cannot touch a coil or an injector cannot do the
    // one job it has, so the borrow is explicit rather than the arbiter being loosened for everyone.
    //
    // ONLY SAFE WHILE THE ENGINE IS STOPPED, which the caller enforces; the firing layer is not
    // driving these pins then. Hand it back with restore() — releasing it to FREE instead would leave
    // the coil unowned until the next rebuild, i.e. one cylinder silently not firing on the next start.
    [[nodiscard]] ITimerChannel* borrow(uint8_t row, PinOwner owner, PinOwner& previous) noexcept {
        if (row >= n_ || !pool_ || !pool_[row]) return nullptr;
        previous = owner_[row];
        owner_[row] = owner;
        return pool_[row];
    }

    // Give a borrowed pin back to the owner borrow() reported. No-op if someone else took it meanwhile.
    void restore(uint8_t row, PinOwner borrower, PinOwner previous) noexcept {
        if (row >= n_) return;
        if (owner_[row] == borrower) owner_[row] = previous;
    }

    // Release every pin held by `owner` → force Hi-Z + mark FREE.
    void release_owner(PinOwner owner) noexcept {
        for (uint8_t i = 0; i < n_; ++i)
            if (owner_[i] == owner) { if (pool_[i]) pool_[i]->disable_output(); owner_[i] = PinOwner::FREE; }
    }

    [[nodiscard]] PinOwner owner_of(uint8_t row) const noexcept {
        return (row < n_) ? owner_[row] : PinOwner::FREE;
    }

    // ---- Conflict record (last rejected claim since clear_conflict) ----------
    [[nodiscard]] bool     has_conflict()      const noexcept { return conflict_; }
    [[nodiscard]] PinOwner conflict_denied()   const noexcept { return conflict_denied_; }  // who was rejected
    [[nodiscard]] PinOwner conflict_holder()   const noexcept { return conflict_holder_; }  // who owns it
    void clear_conflict() noexcept { conflict_ = false; }

private:
    void record_conflict(PinOwner denied, PinOwner holder) noexcept {
        conflict_ = true; conflict_denied_ = denied; conflict_holder_ = holder;
    }

    ITimerChannel** pool_ = nullptr;
    uint8_t         n_    = 0;
    PinOwner        owner_[MAX_PINS] = { PinOwner::FREE };

    bool     conflict_        = false;
    PinOwner conflict_denied_ = PinOwner::FREE;
    PinOwner conflict_holder_ = PinOwner::FREE;
};
