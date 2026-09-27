#pragma once
//
// GenericCanFieldValue — one decoded generic-CAN receive field, and the seam between the CAN task
// and the sensor pipeline.
//
// Its own header because the two sides must agree on the layout without depending on each other:
// GenericCan fills these, the pipeline's acquire_can_field reads them, and neither needs the other's
// header. That is what lets a CAN-interface sensor take its reading from the same decode that feeds
// the signal bus, rather than a second one with its own addressing.
//
#include <cstdint>

// `at_ms` is 0 until the first frame carrying this field arrives, which is what "no reading yet"
// means — distinct from a reading of zero, and the difference between an absent sensor and a cold one.
//
// WRITTEN ON THE CAN TASK, READ ON THE ENGINE TASK, deliberately unlocked. Each member is a
// naturally-aligned 32-bit word so neither is torn; the PAIR can be read across an update, and the
// worst that costs is one frame's staleness decision made against the previous timestamp. A lock
// would be paid on every frame the wire carries to remove a one-millisecond ambiguity in a value
// that is refreshed in milliseconds.
struct GenericCanFieldValue {
    float    v     = 0.0f;
    uint32_t at_ms = 0;
};
