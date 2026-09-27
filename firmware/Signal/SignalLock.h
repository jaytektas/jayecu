#pragma once

// ---------------------------------------------------------------------------
// SignalLock — the critical section that makes a low-priority bus writer's
// read-modify-write of a slot atomic against the higher-priority writers.
//
// The bus uses priority-as-lock. On a single core the engine task (the
// highest-priority writer, and the hot path) can never be preempted by any
// other writer, so its gate-then-store sequence is already atomic — it writes
// LOCK-FREE. Every lower-priority writer (the Lua thread; anything that posts
// an override at prio > PRIO_BASE) CAN be preempted mid-RMW by the engine task,
// which could then read a half-updated slot or race the gate. Those writers
// wrap their RMW in this section, which blocks task switching for the ~handful
// of stores so the engine task cannot run in the middle of it.
//
// Scope is deliberately tiny (one gate test + up to five field stores). All bus
// writers run in task context — no ISR writes the bus — so a plain FreeRTOS
// critical section (not the _FROM_ISR variant) is correct and sufficient.
//
// On host builds (no JAYECU_FIRMWARE / no FreeRTOS) this is a no-op: the unit
// tests are single-threaded.
// ---------------------------------------------------------------------------

#if defined(JAYECU_FIRMWARE)
#include "FreeRTOS.h"
#include "task.h"
inline void sig_lock_enter() { taskENTER_CRITICAL(); }
inline void sig_lock_exit()  { taskEXIT_CRITICAL(); }
#else
inline void sig_lock_enter() {}
inline void sig_lock_exit()  {}
#endif

// RAII guard — enter on construct, exit on scope exit (so an early return can't
// leak the section). Conditional: a base-priority (hot-path) write constructs it
// disengaged and pays only a preded-not-taken branch, no BASEPRI traffic.
struct SigLockGuard {
    explicit SigLockGuard(bool engage) : engaged_(engage) { if (engaged_) sig_lock_enter(); }
    ~SigLockGuard() { if (engaged_) sig_lock_exit(); }
    SigLockGuard(const SigLockGuard&) = delete;
    SigLockGuard& operator=(const SigLockGuard&) = delete;
private:
    bool engaged_;
};
