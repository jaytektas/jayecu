#pragma once

#include "../Engine/EngineModule.h"
#include "../Signal/SignalBus.h"

// ---------------------------------------------------------------------------
// HardwareInput — the physical-input producer (INPUT phase, runs FIRST, ahead of Sensors).
//
// Reads each board analog input ONCE per frame from the HAL (the DMA-filtered ADC ring) and
// publishes it onto the SignalBus as raw u32 ADC counts (SIG_HW_AV* / SIG_HW_AT*). Everything
// downstream reads the counts FROM THE BUS — telemetry today (the old raw_* telem special case is
// gone), and, incrementally, the sensor pipeline's Acquire — so a pin is touched in exactly one
// place. The publish body is generated per board: see codegen gen_hw_input_publish ->
// generated/hw_input_publish.inc.
//
// Stateless: no config, no init. Counts are the source of truth the host display-adjusts to mV/V;
// the firmware never scales them (ADC-counts-native, see platform_read_ain_raw).
// ---------------------------------------------------------------------------

class HardwareInput : public EngineModule {
public:
    // Producer: reads the physical analog inputs and WRITES them onto the bus.
    // pos/frame unused; now_ms is read internally.
    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;
};
