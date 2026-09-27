cmake_minimum_required(VERSION 3.16)

# Native host build for unit tests -- no MCU toolchain.
project(jayecu_tests CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

set(ROOT_DIR    ${CMAKE_SOURCE_DIR}/..)
set(GEN_DIR     ${ROOT_DIR}/generated)
set(SCHED_DIR   ${ROOT_DIR}/firmware/Scheduler)
set(FW_DIR      ${ROOT_DIR}/firmware)
set(LUA_DIR     ${ROOT_DIR}/third_party/lua/src)

# ---------------------------------------------------------------------------
# Lua 5.4 -- compiled directly from source, no system dependency.
# ---------------------------------------------------------------------------

file(GLOB LUA_SRCS ${LUA_DIR}/*.c)
# Exclude the Lua standalone interpreter and compiler.
list(FILTER LUA_SRCS EXCLUDE REGEX ".*(lua|luac)\\.c$")

# Host tests have no flash banks, so g_config must carry the schema defaults. The FIRMWARE
# deliberately leaves it uninitialised (.bss) and loads a validated record from bank A/B or
# fail-stops — see generated/default_config.cpp.
add_compile_definitions(JAYECU_COMPILED_DEFAULTS)

add_library(lua54 STATIC ${LUA_SRCS})
target_include_directories(lua54 PUBLIC ${LUA_DIR})
target_compile_options(lua54 PRIVATE -w)  # suppress Lua's own warnings

# ---------------------------------------------------------------------------
# Platform stub -- provides platform_get_tick_ms() etc. for native builds.
# ---------------------------------------------------------------------------

add_library(platform_stub STATIC
    ${FW_DIR}/Platform/stubs/platform_hal_stub.cpp
    ${FW_DIR}/Platform/boards/stubs/board_hal_stub.cpp
    ${FW_DIR}/Scheduler/EngineSyncSampler.cpp
)
target_include_directories(platform_stub PUBLIC
    ${FW_DIR}
    ${GEN_DIR}/boards
)

# ---------------------------------------------------------------------------
# Shared include paths
# ---------------------------------------------------------------------------

include_directories(
    ${GEN_DIR}
    ${GEN_DIR}/modules
    ${SCHED_DIR}
    ${FW_DIR}
    ${LUA_DIR}
    ${CMAKE_SOURCE_DIR}
)

# ---------------------------------------------------------------------------
# Engine module tests (no Lua, no FreeRTOS)
# ---------------------------------------------------------------------------

add_executable(test_rev_limiter
    test_rev_limiter.cpp
    ${FW_DIR}/Engine/Modules/RevLimiter.cpp
)
target_include_directories(test_rev_limiter PRIVATE ${GEN_DIR} ${SCHED_DIR})
add_test(NAME rev_limiter COMMAND test_rev_limiter)
target_link_libraries(test_rev_limiter PRIVATE platform_stub)

add_executable(test_alternator_control
    test_alternator_control.cpp
    ${FW_DIR}/Engine/Modules/Alternator.cpp
)
target_include_directories(test_alternator_control PRIVATE ${GEN_DIR} ${SCHED_DIR})
add_test(NAME alternator_control COMMAND test_alternator_control)

# The learned-region stub backs the long-term trim: the module maps its trim onto a fixed slice of
# that region, so without it the test links against nothing and the trim has nowhere to live.
add_executable(test_boost_control
    test_boost_control.cpp
    ${FW_DIR}/Engine/Modules/Boost.cpp
    ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
    ${FW_DIR}/Platform/stubs/learned_region_stub.cpp
)
target_include_directories(test_boost_control PRIVATE ${GEN_DIR} ${SCHED_DIR})
target_link_libraries(test_boost_control PRIVATE platform_stub)
add_test(NAME boost_control COMMAND test_boost_control)

add_executable(test_launch_control
    test_launch_control.cpp
    ${FW_DIR}/Engine/Modules/Launch.cpp
    ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
    ${GEN_DIR}/default_config.cpp                      # the shipped launch maps: the tables must be real
    ${FW_DIR}/Platform/stubs/learned_region_stub.cpp
)
target_include_directories(test_launch_control PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR} ${SCHED_DIR})
target_link_libraries(test_launch_control PRIVATE platform_stub)
add_test(NAME launch_control COMMAND test_launch_control)

# Cadence ttl derivation (header-only — ttl_for additive period+grace formula)
add_executable(test_ttl_cadence test_ttl_cadence.cpp)
target_include_directories(test_ttl_cadence PRIVATE ${FW_DIR} ${GEN_DIR})
add_test(NAME ttl_cadence COMMAND test_ttl_cadence)

# Learned-region totem codec + rotation pick (header-only — LearnedHeader integrity + LearnedStore::load)
add_executable(test_learned_store test_learned_store.cpp)
target_include_directories(test_learned_store PRIVATE ${FW_DIR} ${GEN_DIR})
add_test(NAME learned_store COMMAND test_learned_store)

# PinArbiter cross-domain output claim (header-only — the OutputManager safety interlock)
add_executable(test_pin_arbiter test_pin_arbiter.cpp)
target_include_directories(test_pin_arbiter PRIVATE ${FW_DIR} ${SCHED_DIR})
add_test(NAME pin_arbiter COMMAND test_pin_arbiter)

add_executable(test_signal_priority test_signal_priority.cpp)
target_include_directories(test_signal_priority PRIVATE ${FW_DIR} ${GEN_DIR})
add_test(NAME signal_priority COMMAND test_signal_priority)

add_executable(test_knock test_knock.cpp
    ${FW_DIR}/Engine/Modules/Knock.cpp
    ${FW_DIR}/Engine/Modules/Ignition.cpp
    ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
    ${GEN_DIR}/default_config.cpp
    ${FW_DIR}/Platform/stubs/learned_region_stub.cpp
)
target_include_directories(test_knock PRIVATE ${FW_DIR} ${GEN_DIR} ${GEN_DIR}/modules ${SCHED_DIR})
target_link_libraries(test_knock PRIVATE platform_stub)
add_test(NAME knock COMMAND test_knock)

add_executable(test_knock_dsp test_knock_dsp.cpp
    ${FW_DIR}/Engine/Modules/KnockDsp.cpp
    ${FW_DIR}/Engine/Modules/Biquad.cpp)
target_include_directories(test_knock_dsp PRIVATE ${FW_DIR})
add_test(NAME knock_dsp COMMAND test_knock_dsp)

add_executable(test_knock_detector test_knock_detector.cpp
    ${FW_DIR}/Engine/Modules/KnockDetector.cpp
    ${FW_DIR}/Engine/Modules/KnockDsp.cpp
    ${FW_DIR}/Engine/Modules/Biquad.cpp)
target_include_directories(test_knock_detector PRIVATE ${FW_DIR} ${GEN_DIR})
target_link_libraries(test_knock_detector PRIVATE platform_stub)
add_test(NAME knock_detector COMMAND test_knock_detector)

add_executable(test_misfire test_misfire.cpp
    ${FW_DIR}/Engine/Modules/Misfire.cpp
    ${FW_DIR}/Scheduler/SegmentTimer.cpp
    ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
    ${FW_DIR}/Platform/stubs/learned_region_stub.cpp
)
target_include_directories(test_misfire PRIVATE ${FW_DIR} ${GEN_DIR})
target_link_libraries(test_misfire PRIVATE platform_stub)
add_test(NAME misfire COMMAND test_misfire)

add_executable(test_segment_timer test_segment_timer.cpp
    ${FW_DIR}/Scheduler/SegmentTimer.cpp)
target_include_directories(test_segment_timer PRIVATE ${FW_DIR} ${GEN_DIR})
add_test(NAME segment_timer COMMAND test_segment_timer)

add_executable(test_text_log test_text_log.cpp ${FW_DIR}/Diagnostics/TextLog.cpp)
target_include_directories(test_text_log PRIVATE ${FW_DIR})
target_link_libraries(test_text_log PRIVATE platform_stub)
add_test(NAME text_log COMMAND test_text_log)

# H-bridge control module: two independent half-bridge slots, each driven by demand_sig from the bus
add_executable(test_hbridge_control
    test_hbridge_control.cpp
    ${FW_DIR}/Engine/Modules/HBridge.cpp
    ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
)
target_include_directories(test_hbridge_control PRIVATE ${GEN_DIR} ${SCHED_DIR} ${FW_DIR})
target_link_libraries(test_hbridge_control PRIVATE platform_stub)
add_test(NAME hbridge_control COMMAND test_hbridge_control)

# Stepper module: 0..100% position demand -> bipolar cos/sin coil duties on the bus
add_executable(test_stepper
    test_stepper.cpp
    ${FW_DIR}/Engine/Modules/Stepper.cpp
    ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
)
target_include_directories(test_stepper PRIVATE ${GEN_DIR} ${SCHED_DIR} ${FW_DIR})
target_link_libraries(test_stepper PRIVATE platform_stub)
add_test(NAME stepper COMMAND test_stepper)

add_executable(test_dtc
    test_dtc.cpp
    ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
)
target_include_directories(test_dtc PRIVATE ${FW_DIR} ${GEN_DIR})
add_test(NAME dtc COMMAND test_dtc)

add_executable(test_fuel_calculator
    test_fuel_calculator.cpp
    ${FW_DIR}/Engine/Modules/FuelCalculator.cpp
    ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
    ${GEN_DIR}/default_config.cpp          # g_config — FuelCalculator live-reads engine.cylinder_count
)
target_include_directories(test_fuel_calculator PRIVATE ${GEN_DIR} ${SCHED_DIR})
target_link_libraries(test_fuel_calculator PRIVATE platform_stub)
add_test(NAME fuel_calculator COMMAND test_fuel_calculator)

add_executable(test_lambda_control
    test_lambda_control.cpp
    ${FW_DIR}/Engine/Modules/Lambda.cpp
    ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
    ${GEN_DIR}/default_config.cpp          # g_config — Lambda resolves wideband scope from sensor assignment
    ${FW_DIR}/Platform/stubs/learned_region_stub.cpp
)
target_include_directories(test_lambda_control PRIVATE ${GEN_DIR} ${SCHED_DIR})
add_test(NAME lambda_control COMMAND test_lambda_control)

add_executable(test_ignition_controller
    test_ignition_controller.cpp
    ${FW_DIR}/Engine/Modules/Ignition.cpp
    ${GEN_DIR}/default_config.cpp          # g_config — Ignition live-reads engine.cylinder_count
)
target_include_directories(test_ignition_controller PRIVATE ${GEN_DIR} ${SCHED_DIR})
add_test(NAME ignition_controller COMMAND test_ignition_controller)
target_link_libraries(test_ignition_controller PRIVATE platform_stub)

add_executable(test_ignition_trim
    test_ignition_trim.cpp
    ${FW_DIR}/Engine/Modules/IgnitionTrim.cpp
)
target_include_directories(test_ignition_trim PRIVATE ${GEN_DIR} ${SCHED_DIR})
add_test(NAME ignition_trim COMMAND test_ignition_trim)
target_link_libraries(test_ignition_trim PRIVATE platform_stub)

# FuelTrim -- 1 kHz round-robin producer of the slow fuel corrections; reads FuelCalculator's tables
add_executable(test_fuel_trim
    test_fuel_trim.cpp
    ${FW_DIR}/Engine/Modules/FuelTrim.cpp
    ${GEN_DIR}/default_config.cpp)        # g_config — default-seeded correction tables
target_include_directories(test_fuel_trim PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${SCHED_DIR} ${FW_DIR})
add_test(NAME fuel_trim COMMAND test_fuel_trim)
target_link_libraries(test_fuel_trim PRIVATE platform_stub)

# ---------------------------------------------------------------------------
# Scripting test -- requires Lua
# ---------------------------------------------------------------------------

# CAN broker static library — used by test_scripting (live CanBroker / OBD-II coverage).
add_library(can_broker STATIC
    ${FW_DIR}/Can/CanBroker.cpp
    ${FW_DIR}/Can/GenericCan.cpp
    ${FW_DIR}/Can/ObdResponder.cpp
    ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
)
target_include_directories(can_broker PUBLIC ${FW_DIR} ${GEN_DIR} ${GEN_DIR}/modules)

add_executable(test_scripting
    test_scripting.cpp
    ${FW_DIR}/Scripting/ScriptEngine.cpp
    ${FW_DIR}/Diagnostics/TextLog.cpp      # g_text_log — ScriptEngine writes error/print text
    ${GEN_DIR}/default_config.cpp          # g_config — read by getCalibration / evalTable
    ${FW_DIR}/Platform/stubs/learned_region_stub.cpp
)
target_include_directories(test_scripting PRIVATE
    ${GEN_DIR}
    ${GEN_DIR}/modules
    ${SCHED_DIR}
    ${FW_DIR}
    ${LUA_DIR}
)
target_link_libraries(test_scripting PRIVATE lua54 platform_stub can_broker)
add_test(NAME scripting COMMAND test_scripting)

# (CAN TX-broadcast test removed with the old schema-driven serializers — pending the
#  rearchitected decoder/broadcaster model.)

# ---------------------------------------------------------------------------
# Signal bus test -- header-only, no extra sources
# ---------------------------------------------------------------------------

add_executable(test_signal_bus
    test_signal_bus.cpp
)
target_include_directories(test_signal_bus PRIVATE ${GEN_DIR} ${SCHED_DIR} ${FW_DIR})
add_test(NAME signal_bus COMMAND test_signal_bus)

# ---------------------------------------------------------------------------
# Sensors runtime pipeline -- provides its own platform HAL stubs
# ---------------------------------------------------------------------------

add_executable(test_sensors
    test_sensors.cpp
    ${FW_DIR}/Sensors/Sensors.cpp
    ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
    ${FW_DIR}/Scheduler/EngineSyncSampler.cpp
    ${FW_DIR}/Integration/HardwareInput.cpp   # publishes SIG_HW_* the analog Acquire now reads off the bus
    ${FW_DIR}/Can/GenericCan.cpp              # a CAN sensor resolves (frame, start bit) -> field through it
    ${GEN_DIR}/default_config.cpp             # g_config — a precondition expression can read config
                                              # fields by baked offset, so the VM needs the image base
    # A precondition may now read a TABLE, and the table registry covers the learned ones too — whose
    # cells are a pointer into the learned region rather than a config member. Linking the stub is the
    # honest version of that dependency: the expression path really can reach those tables.
    ${FW_DIR}/Platform/stubs/learned_region_stub.cpp
)
target_include_directories(test_sensors PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${SCHED_DIR} ${FW_DIR})
# GenericCan::tick() sends through the broker. Nothing here ticks it — the test only configures it
# and resolves against it — but the symbol still has to be there.
target_link_libraries(test_sensors PRIVATE can_broker)
add_test(NAME sensors COMMAND test_sensors)

# Polymorphic pipeline -- engine + stage library + CAN frame cache + HAL acquires
add_executable(test_pipeline test_pipeline.cpp)
target_include_directories(test_pipeline PRIVATE ${GEN_DIR} ${GEN_DIR}/boards ${FW_DIR})
target_link_libraries(test_pipeline PRIVATE platform_stub)
add_test(NAME pipeline COMMAND test_pipeline)

# Pipeline builder -- composes a Pipeline from the sensor catalog + SensorConfig
add_executable(test_pipeline_builder test_pipeline_builder.cpp)
target_include_directories(test_pipeline_builder PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${GEN_DIR}/boards ${FW_DIR})
target_link_libraries(test_pipeline_builder PRIVATE platform_stub)

# Multi-position switch -- ladder buttons on one analog pin, decoded to a position by voltage bands
add_executable(test_multi_switch test_multi_switch.cpp)
target_include_directories(test_multi_switch PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${GEN_DIR}/boards ${FW_DIR})
target_link_libraries(test_multi_switch PRIVATE platform_stub)
add_test(NAME multi_switch COMMAND test_multi_switch)

# The SD log's FORMAT: MLG v2 byte-for-byte, against the real generated field table. Big-endian
# ordering and the fixed-width padding are what a reader depends on, so they are asserted as bytes.
# WHEN the card logs: the deadband that stops a file-per-crossing, the fail-ON default a logger wants,
# and max-on-with-zero-rearm as file rolling.
add_executable(test_datalog_gate test_datalog_gate.cpp)
target_include_directories(test_datalog_gate PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})

find_package(Threads REQUIRED)

# The SPSC ring between the 1 kHz sampler and the card — threaded here on purpose, because the
# interleavings that matter on the ECU are the ones you cannot arrange deliberately.
add_executable(test_log_ring test_log_ring.cpp)
target_include_directories(test_log_ring PRIVATE ${GEN_DIR} ${FW_DIR})
target_link_libraries(test_log_ring PRIVATE Threads::Threads)

add_executable(test_mlg_log test_mlg_log.cpp ${FW_DIR}/Comms/MlgLog.cpp)
target_include_directories(test_mlg_log PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${GEN_DIR}/boards ${FW_DIR})

# Engine-sync angle-window sampler -- the windowing math (provides its own ADC stub)
add_executable(test_engine_sync_sampler test_engine_sync_sampler.cpp ${FW_DIR}/Scheduler/EngineSyncSampler.cpp)
target_include_directories(test_engine_sync_sampler PRIVATE ${FW_DIR})
add_test(NAME engine_sync_sampler COMMAND test_engine_sync_sampler)

# Engine state machine -- STOPPED/CRANKING/RUNNING transition logic (header-only, no deps)
add_executable(test_engine_state_machine test_engine_state_machine.cpp)
target_include_directories(test_engine_state_machine PRIVATE ${FW_DIR})
add_test(NAME engine_state_machine COMMAND test_engine_state_machine)

# Table engine -- the generic 1D/2D/3D float-axis interp core (header-only, no deps)
add_executable(test_table_engine test_table_engine.cpp)
target_include_directories(test_table_engine PRIVATE ${FW_DIR})
add_test(NAME table_engine COMMAND test_table_engine)

# TableEval -- the channel-driven descriptor layer (live-stride indexing over the bus)
add_executable(test_table_eval test_table_eval.cpp)
target_include_directories(test_table_eval PRIVATE ${FW_DIR} ${GEN_DIR})
add_test(NAME table_eval COMMAND test_table_eval)

# TransientThrottle -- 1 kHz throttle/MAP-movement transient (enrich + disenrich); reads g_config
# (default-seeded tables), so link default_config.cpp.
add_executable(test_transient_throttle test_transient_throttle.cpp
    ${FW_DIR}/Engine/Modules/TransientThrottle.cpp
    ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
    ${GEN_DIR}/default_config.cpp)
target_include_directories(test_transient_throttle PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${SCHED_DIR} ${FW_DIR})
target_link_libraries(test_transient_throttle PRIVATE platform_stub)
add_test(NAME transient_throttle COMMAND test_transient_throttle)

# Idle -- hardware-agnostic idle controller (open-loop base position -> idle_duty signal)
add_executable(test_idle_control test_idle_control.cpp ${FW_DIR}/Engine/Modules/Idle.cpp
    ${FW_DIR}/Platform/stubs/learned_region_stub.cpp
    ${GEN_DIR}/default_config.cpp   # g_config: the shipped defaults, so a test starts where a user does
)
target_include_directories(test_idle_control PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
add_test(NAME idle_control COMMAND test_idle_control)

# ThrottleControl -- ETB/DBW servo. Stage 0 read-only A/B; Stage 1 manual nudge; Stage 2 autocal
# (links default_config.cpp for g_config — autocal writes the sensor cal).
add_executable(test_throttle_control test_throttle_control.cpp
    ${FW_DIR}/Engine/Modules/ElectronicThrottle.cpp
    ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
    ${FW_DIR}/Diagnostics/TextLog.cpp      # g_text_log — autocal publishes findlimits/fillff results
    ${GEN_DIR}/default_config.cpp)
target_include_directories(test_throttle_control PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})

# App — DBW accelerator pedal (A/B correlation, pedal-loss fail-safe, calibrate hold)
add_executable(test_app_pedal test_app_pedal.cpp
    ${FW_DIR}/Engine/Modules/App.cpp
    ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
    ${FW_DIR}/Diagnostics/TextLog.cpp      # g_text_log — App reports the pedalcal result to the console
    ${GEN_DIR}/default_config.cpp)
target_include_directories(test_app_pedal PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
add_test(NAME app_pedal COMMAND test_app_pedal)

# Dfco — deceleration/overrun fuel cut (closed-throttle + warm + RPM hysteresis)
add_executable(test_dfco test_dfco.cpp ${FW_DIR}/Engine/Modules/Dfco.cpp)
target_include_directories(test_dfco PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
add_test(NAME dfco COMMAND test_dfco)
target_link_libraries(test_dfco PRIVATE platform_stub)

# LambdaProtect — lean-protection fuel cut (too lean under load past a timeout)
add_executable(test_lambda_protect test_lambda_protect.cpp ${FW_DIR}/Engine/Modules/LambdaProtect.cpp)
target_include_directories(test_lambda_protect PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
add_test(NAME lambda_protect COMMAND test_lambda_protect)
add_test(NAME throttle_control COMMAND test_throttle_control)
add_test(NAME pipeline_builder COMMAND test_pipeline_builder)
add_test(NAME mlg_log COMMAND test_mlg_log)
add_test(NAME log_ring COMMAND test_log_ring)
add_test(NAME datalog_gate COMMAND test_datalog_gate)

# Output-side pipeline -- arbitrate / encode / emit on the shared engine (header-only)
# The output GATE — when a slot is on: the latch, the deadband and the timers (no bus, no pins).
add_executable(test_output_gate test_output_gate.cpp)
target_include_directories(test_output_gate PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${SCHED_DIR} ${FW_DIR})
add_test(NAME output_gate COMMAND test_output_gate)

# The OUTPUT MANAGER — the slot itself: build, gate, value source, encode, region-scoped rebuild.
# It had no host test at all, which is how a compile error in it reached the bench.
add_executable(test_output_manager
    test_output_manager.cpp
    ${FW_DIR}/Integration/OutputManager.cpp
    ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
    ${GEN_DIR}/default_config.cpp
    ${FW_DIR}/Platform/stubs/learned_region_stub.cpp
    ${FW_DIR}/Platform/stubs/platform_hal_stub.cpp
)
target_include_directories(test_output_manager PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${SCHED_DIR} ${FW_DIR})
add_test(NAME output_manager COMMAND test_output_manager)

# The ADC boxcar fold — rank-strided averaging over a DMA ring. Pure arithmetic, no MCU
# header, so the one part of the analog acquisition path with real logic is covered on the host.
# The LPS22HB/LPS25HB environmental sensor driver. Bus-abstracted, so the variant handling —
# CTRL_REG1 address, the LPS25 power-down bit, the auto-increment bit and the temperature
# formula, every one of which fails silently — is covered against a fake bus.
add_executable(test_baro_lps2x test_baro_lps2x.cpp ${FW_DIR}/Platform/devices/Lps2xBaro.cpp)
target_include_directories(test_baro_lps2x PRIVATE ${FW_DIR})
add_test(NAME baro_lps2x COMMAND test_baro_lps2x)

add_executable(test_adc_fold test_adc_fold.cpp)
target_include_directories(test_adc_fold PRIVATE ${FW_DIR})
add_test(NAME adc_fold COMMAND test_adc_fold)

add_executable(test_output_pipeline test_output_pipeline.cpp)
target_include_directories(test_output_pipeline PRIVATE ${GEN_DIR} ${SCHED_DIR} ${FW_DIR})
add_test(NAME output_pipeline COMMAND test_output_pipeline)

# FreqCounter -- header-only frequency measurement logic
add_executable(test_freq_counter test_freq_counter.cpp)
target_include_directories(test_freq_counter PRIVATE ${FW_DIR})
add_test(NAME freq_counter COMMAND test_freq_counter)

# SentDecoder -- header-only SAE J2716 SENT fast-channel decode
add_executable(test_sent test_sent.cpp)
target_include_directories(test_sent PRIVATE ${FW_DIR})
add_test(NAME sent COMMAND test_sent)

# PulseCounter -- header-only period + high-time (duty / pulse width) measurement
add_executable(test_pulse test_pulse.cpp)
target_include_directories(test_pulse PRIVATE ${FW_DIR})
add_test(NAME pulse COMMAND test_pulse)

# CLI text-command registry (core only — dummy commands, no engine/platform deps)
add_executable(test_cli
    test_cli.cpp
    ${FW_DIR}/Cli/CliRegistry.cpp
)
target_include_directories(test_cli PRIVATE ${FW_DIR})
add_test(NAME cli COMMAND test_cli)


# ---------------------------------------------------------------------------
# Storage layer tests — platform-agnostic, no FreeRTOS or HAL
# ---------------------------------------------------------------------------

set(STORAGE_DIR ${FW_DIR}/Storage)

add_executable(test_config_bank
    test_config_bank.cpp
    ${STORAGE_DIR}/ConfigBank.cpp
    ${SCHED_DIR}/Log.cpp
)
target_include_directories(test_config_bank PRIVATE ${FW_DIR})
add_test(NAME config_bank COMMAND test_config_bank)

add_executable(test_default_config
    test_default_config.cpp
    ${GEN_DIR}/default_config.cpp
    ${GEN_DIR}/default_tune.cpp
    ${STORAGE_DIR}/ConfigBank.cpp
    ${SCHED_DIR}/Log.cpp
)
target_include_directories(test_default_config PRIVATE ${FW_DIR} ${GEN_DIR} ${GEN_DIR}/modules)
add_test(NAME default_config COMMAND test_default_config)

add_executable(test_sd_arbitrator
    test_sd_arbitrator.cpp
    ${STORAGE_DIR}/SdArbitrator.cpp
)
target_include_directories(test_sd_arbitrator PRIVATE ${FW_DIR})
add_test(NAME sd_arbitrator COMMAND test_sd_arbitrator)

add_executable(test_storage_manager
    test_storage_manager.cpp
    ${STORAGE_DIR}/ConfigBank.cpp
    ${STORAGE_DIR}/SdArbitrator.cpp
    ${STORAGE_DIR}/StorageManager.cpp
    ${SCHED_DIR}/Log.cpp
)
target_include_directories(test_storage_manager PRIVATE ${FW_DIR})
add_test(NAME storage_manager COMMAND test_storage_manager)


# Generic-trigger step 1: the standalone SEQUENCE primitive (header-only, no decoder).
add_executable(test_sequence_matcher test_sequence_matcher.cpp)
target_include_directories(test_sequence_matcher PRIVATE
    ${GEN_DIR} ${GEN_DIR}/modules ${SCHED_DIR} ${FW_DIR})
add_test(NAME sequence_matcher COMMAND test_sequence_matcher)

add_executable(test_gap_matcher test_gap_matcher.cpp)
target_include_directories(test_gap_matcher PRIVATE
    ${GEN_DIR} ${GEN_DIR}/modules ${SCHED_DIR} ${FW_DIR})
add_test(NAME gap_matcher COMMAND test_gap_matcher)

add_executable(test_width_matcher test_width_matcher.cpp)
target_include_directories(test_width_matcher PRIVATE
    ${GEN_DIR} ${GEN_DIR}/modules ${SCHED_DIR} ${FW_DIR})
add_test(NAME width_matcher COMMAND test_width_matcher)

# Software PWM / pulse-train engine (header-only core, fake clock + pin).
add_executable(test_soft_pwm test_soft_pwm.cpp)
target_include_directories(test_soft_pwm PRIVATE
    ${GEN_DIR} ${GEN_DIR}/modules ${SCHED_DIR} ${FW_DIR})
add_test(NAME soft_pwm COMMAND test_soft_pwm)

# THE OUTPUT ROWS as the scheduler binds them: what each cylinder value names, the wasted-spark companion,
# and the claim / polarity / stage / cut behaviour of a map that is not the default layout.
add_executable(test_output_map test_output_map.cpp
    ${SCHED_DIR}/EventScheduler.cpp ${SCHED_DIR}/CycleRecorder.cpp ${GEN_DIR}/default_config.cpp)
target_include_directories(test_output_map PRIVATE
    ${GEN_DIR} ${GEN_DIR}/modules ${GEN_DIR}/boards ${SCHED_DIR} ${FW_DIR})
target_link_libraries(test_output_map PRIVATE platform_stub)
add_test(NAME output_map COMMAND test_output_map)

# The async transient burst: does it spread over the ENGINE CYCLE, whatever that cycle is?
add_executable(test_async_injection test_async_injection.cpp
    ${SCHED_DIR}/EventScheduler.cpp ${SCHED_DIR}/CycleRecorder.cpp ${GEN_DIR}/default_config.cpp)
target_include_directories(test_async_injection PRIVATE
    ${GEN_DIR} ${GEN_DIR}/modules ${GEN_DIR}/boards ${SCHED_DIR} ${FW_DIR})
target_link_libraries(test_async_injection PRIVATE platform_stub)
add_test(NAME async_injection COMMAND test_async_injection)

# The four injection distribution modes: which channels fire, and how often, at the pins.
add_executable(test_injection_modes test_injection_modes.cpp
    ${SCHED_DIR}/EventScheduler.cpp ${SCHED_DIR}/CycleRecorder.cpp ${GEN_DIR}/default_config.cpp)
target_include_directories(test_injection_modes PRIVATE
    ${GEN_DIR} ${GEN_DIR}/modules ${GEN_DIR}/boards ${SCHED_DIR} ${FW_DIR})
target_link_libraries(test_injection_modes PRIVATE platform_stub)
add_test(NAME injection_modes COMMAND test_injection_modes)

add_executable(test_cycle_recorder test_cycle_recorder.cpp ${SCHED_DIR}/CycleRecorder.cpp)
target_include_directories(test_cycle_recorder PRIVATE
    ${GEN_DIR} ${GEN_DIR}/modules ${SCHED_DIR} ${FW_DIR})
add_test(NAME cycle_recorder COMMAND test_cycle_recorder)

add_executable(test_generic_trigger test_generic_trigger.cpp)
target_include_directories(test_generic_trigger PRIVATE
    ${GEN_DIR} ${GEN_DIR}/modules ${SCHED_DIR} ${FW_DIR})
add_test(NAME generic_trigger COMMAND test_generic_trigger)

# Can a configuration ever know where the engine is? Pure, no decoder state — the same function the
# firmware gates firing on.
add_executable(test_trigger_config_check test_trigger_config_check.cpp)
target_include_directories(test_trigger_config_check PRIVATE
    ${GEN_DIR} ${GEN_DIR}/modules ${SCHED_DIR} ${FW_DIR})
add_test(NAME trigger_config_check COMMAND test_trigger_config_check)

# What the decoder does when the ENGINE misbehaves: chipped teeth, crosstalk, open connectors.
add_executable(test_trigger_failures test_trigger_failures.cpp)
target_include_directories(test_trigger_failures PRIVATE
    ${GEN_DIR} ${GEN_DIR}/modules ${SCHED_DIR} ${FW_DIR})
add_test(NAME trigger_failures COMMAND test_trigger_failures)

add_executable(test_generic_builder test_generic_builder.cpp)
target_include_directories(test_generic_builder PRIVATE
    ${GEN_DIR} ${GEN_DIR}/modules ${SCHED_DIR} ${FW_DIR})
add_test(NAME generic_builder COMMAND test_generic_builder)


# ---------------------------------------------------------------------------
# Engine protection test
# ---------------------------------------------------------------------------

add_executable(test_engine_protection
    test_engine_protection.cpp
    ${FW_DIR}/Engine/Modules/EngineProtection.cpp
    ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
    ${GEN_DIR}/default_config.cpp             # g_config — a monitor's condition can read config
                                              # fields by baked offset, so the VM needs the image base
)
target_include_directories(test_engine_protection PRIVATE
    ${GEN_DIR}
    ${GEN_DIR}/modules
    ${SCHED_DIR}
    ${FW_DIR}
)
target_link_libraries(test_engine_protection PRIVATE platform_stub)
add_test(NAME engine_protection COMMAND test_engine_protection)

# Which SIDE of TDC the spark lands on. The advance-to-angle sense had no test at all, and was
# inverted; every other ignition test only checked the value REACHED the scheduler.
# The decoder's own clock: a wheel that goes silent must be noticed even with no firing clock
# running, which is the state every sync transition used to leave the DCO in.
add_executable(test_trigger_liveness
    test_trigger_liveness.cpp
    ${FW_DIR}/Scheduler/EnginePositionHal.cpp
    ${FW_DIR}/Scheduler/SegmentTimer.cpp
    ${FW_DIR}/Scheduler/EventScheduler.cpp
    ${FW_DIR}/Scheduler/VirtualTrigger.cpp
    ${FW_DIR}/Scheduler/CycleRecorder.cpp
    ${GEN_DIR}/default_config.cpp
)
target_include_directories(test_trigger_liveness PRIVATE
    ${GEN_DIR} ${GEN_DIR}/modules ${GEN_DIR}/boards ${SCHED_DIR} ${FW_DIR}
)
target_link_libraries(test_trigger_liveness PRIVATE platform_stub)
add_test(NAME trigger_liveness COMMAND test_trigger_liveness)

add_executable(test_spark_timing
    test_spark_timing.cpp
    ${FW_DIR}/Scheduler/EnginePositionHal.cpp
    ${FW_DIR}/Scheduler/SegmentTimer.cpp
    ${FW_DIR}/Scheduler/EventScheduler.cpp
    ${FW_DIR}/Scheduler/VirtualTrigger.cpp
    ${FW_DIR}/Scheduler/CycleRecorder.cpp
    ${GEN_DIR}/default_config.cpp
)
target_include_directories(test_spark_timing PRIVATE
    ${GEN_DIR} ${GEN_DIR}/modules ${GEN_DIR}/boards ${SCHED_DIR} ${FW_DIR}
)
target_link_libraries(test_spark_timing PRIVATE platform_stub)
add_test(NAME spark_timing COMMAND test_spark_timing)

add_executable(test_expr_vm
    test_expr_vm.cpp
)
target_include_directories(test_expr_vm PRIVATE
    ${GEN_DIR}
    ${GEN_DIR}/modules
    ${SCHED_DIR}
    ${FW_DIR}
)
add_test(NAME expr_vm COMMAND test_expr_vm)

# Lint (no compiled binary): fail the suite if a test hardcodes a catalog sensor index — those drift
# when the generated catalog reorders. Resolve by SignalId instead. See the script header.
add_test(NAME lint_sensor_index
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/lint_no_hardcoded_sensor_index.sh)

enable_testing()

# GearDetect — derive gear from the rpm/speed ratio
add_executable(test_vehicle_speed test_vehicle_speed.cpp ${FW_DIR}/Engine/Modules/VehicleSpeed.cpp)
target_include_directories(test_vehicle_speed PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
target_link_libraries(test_vehicle_speed PRIVATE platform_stub)   # platform_get_tick_ms for the publish TTL
add_test(NAME vehicle_speed COMMAND test_vehicle_speed)

add_executable(test_gear_detect test_gear_detect.cpp ${FW_DIR}/Engine/Modules/GearDetect.cpp)
target_include_directories(test_gear_detect PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
add_test(NAME gear_detect COMMAND test_gear_detect)
target_link_libraries(test_gear_detect PRIVATE platform_stub)

# FlatShift — clutchless upshift cut
add_executable(test_flat_shift test_flat_shift.cpp ${FW_DIR}/Engine/Modules/FlatShift.cpp)
target_include_directories(test_flat_shift PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
add_test(NAME flat_shift COMMAND test_flat_shift)

# TractionControl — wheel-slip throttle cap
# default_config.cpp is ESSENTIAL, not convenience: a hand-built TractionControlConfig{} has an all-zero
# slip axis, and table_eval on a zero axis is degenerate — every lookup returns the first cell and the
# tests would pass while proving nothing about the curves.
add_executable(test_traction_control test_traction_control.cpp ${FW_DIR}/Engine/Modules/TractionControl.cpp
                                     ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
                                     ${GEN_DIR}/default_config.cpp
                                     ${FW_DIR}/Platform/stubs/learned_region_stub.cpp)
target_include_directories(test_traction_control PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
add_test(NAME traction_control COMMAND test_traction_control)
target_link_libraries(test_traction_control PRIVATE platform_stub)

# VvtControl — closed-loop cam phasing PID
add_executable(test_vvt_control test_vvt_control.cpp ${FW_DIR}/Engine/Modules/VvtControl.cpp
    ${FW_DIR}/Platform/stubs/learned_region_stub.cpp
    ${GEN_DIR}/default_config.cpp   # g_config: the shipped defaults, so a test starts where a user does
)
target_include_directories(test_vvt_control PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
add_test(NAME vvt_control COMMAND test_vvt_control)

# PitLimiter — vehicle-speed limiter
add_executable(test_pit_limiter test_pit_limiter.cpp ${FW_DIR}/Engine/Modules/PitLimiter.cpp)
target_include_directories(test_pit_limiter PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
add_test(NAME pit_limiter COMMAND test_pit_limiter)
target_link_libraries(test_pit_limiter PRIVATE platform_stub)

# Nitrous — single-stage nitrous control
add_executable(test_nitrous test_nitrous.cpp ${FW_DIR}/Engine/Modules/Nitrous.cpp)
target_include_directories(test_nitrous PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
add_test(NAME nitrous COMMAND test_nitrous)
target_link_libraries(test_nitrous PRIVATE platform_stub)

# Wmi — water/methanol injection
add_executable(test_wmi test_wmi.cpp ${FW_DIR}/Engine/Modules/Wmi.cpp)
target_include_directories(test_wmi PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
add_test(NAME wmi COMMAND test_wmi)
target_link_libraries(test_wmi PRIVATE platform_stub)

# AntiLag — anti-lag (ALS) ignition retard
add_executable(test_anti_lag test_anti_lag.cpp ${FW_DIR}/Engine/Modules/AntiLag.cpp)
target_include_directories(test_anti_lag PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
add_test(NAME anti_lag COMMAND test_anti_lag)

# Vvl — variable valve lift (VTEC) cam engagement
add_executable(test_vvl test_vvl.cpp ${FW_DIR}/Engine/Modules/Vvl.cpp)
target_include_directories(test_vvl PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
add_test(NAME vvl COMMAND test_vvl)
target_link_libraries(test_vvl PRIVATE platform_stub)

# EgtProtect — EGT over-temp enrichment + fuel cut
# DtcManager: a cut now raises the code for the PROBE that caused it, so the module needs the table.
add_executable(test_egt_protect test_egt_protect.cpp ${FW_DIR}/Engine/Modules/EgtProtect.cpp
                                ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp)
target_include_directories(test_egt_protect PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
add_test(NAME egt_protect COMMAND test_egt_protect)
target_link_libraries(test_egt_protect PRIVATE platform_stub)

# CruiseControl — closed-loop road-speed hold
# default_config.cpp because the expression VM reads the live config image, and the shipped gain curves
# are what a test should start from. learned_region_stub because the table registry a condition may read
# reaches the learned surfaces. The tick stub stays in the test itself, which drives time by hand.
add_executable(test_cruise_control test_cruise_control.cpp ${FW_DIR}/Engine/Modules/CruiseControl.cpp
                                   ${FW_DIR}/Diagnostics/DtcManager.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
                                   ${GEN_DIR}/default_config.cpp
                                   ${FW_DIR}/Platform/stubs/learned_region_stub.cpp)
target_include_directories(test_cruise_control PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
add_test(NAME cruise_control COMMAND test_cruise_control)

# IsoTpTx — ISO 15765-2 multi-frame sender
add_executable(test_isotp_tx test_isotp_tx.cpp)
target_include_directories(test_isotp_tx PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
add_test(NAME isotp_tx COMMAND test_isotp_tx)

# ObdResponder — OBD-II Mode 09 + ISO-TP multi-frame
# Pure header-only bit packing — no platform, no broker, no generated config.
add_executable(test_can_pack test_can_pack.cpp)
target_include_directories(test_can_pack PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
add_test(NAME can_pack COMMAND test_can_pack)

add_executable(test_generic_can test_generic_can.cpp
    ${GEN_DIR}/default_config.cpp)          # the SHIPPED default for a field's channel has to mean unset
target_include_directories(test_generic_can PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
target_link_libraries(test_generic_can PRIVATE platform_stub can_broker)
add_test(NAME generic_can COMMAND test_generic_can)

add_executable(test_obd test_obd.cpp)
target_include_directories(test_obd PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
target_link_libraries(test_obd PRIVATE platform_stub can_broker)
add_test(NAME obd COMMAND test_obd)

# TorqueModel — table-based engine torque/power estimate
add_executable(test_torque_model test_torque_model.cpp ${FW_DIR}/Engine/Modules/TorqueModel.cpp
    ${GEN_DIR}/default_config.cpp   # g_config: the shipped defaults, so a test starts where a user does
)
target_include_directories(test_torque_model PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${FW_DIR})
add_test(NAME torque_model COMMAND test_torque_model)
target_link_libraries(test_torque_model PRIVATE platform_stub)

# ---------------------------------------------------------------------------
# Studio meta contract — can the studio still READ what codegen emits?
#
# Built against the studio's own MetaModel, because a re-description of the
# format here would pass while the studio failed to load the file. It needs the
# JFramework SDK (the studio's toolkit); where that isn't installed the target
# is simply not created — a firmware developer is not required to have the
# studio's dependencies to build the firmware's tests.
# ---------------------------------------------------------------------------
set(STUDIO_DIR ${ROOT_DIR}/apps/studio-jf)
# THE NATIVE SDK, EXPLICITLY FIRST. A Windows cross-build SDK installed alongside it
# (~/jframework-sdk-win) satisfies find_package just as well, and CMake happily picked it — the
# configure succeeded and the LINK then failed on -lws2_32 / -lgdi32, which reads as a broken test
# rather than as the wrong toolkit. Ask for the native one by name, and only then fall back to
# whatever is installed, so a machine with one SDK behaves exactly as before.
find_package(JFramework QUIET PATHS $ENV{HOME}/jframework-sdk/lib/cmake/JFramework NO_DEFAULT_PATH)
if(NOT TARGET JFramework::platform)
    find_package(JFramework QUIET)
endif()
if(TARGET JFramework::platform AND EXISTS ${STUDIO_DIR}/src/model/MetaModel.cpp)
    add_executable(test_studio_meta_contract
        test_studio_meta_contract.cpp
        ${STUDIO_DIR}/src/model/MetaModel.cpp
        ${STUDIO_DIR}/src/model/UnitManager.cpp)
    target_include_directories(test_studio_meta_contract PRIVATE ${STUDIO_DIR}/src)
    target_compile_features(test_studio_meta_contract PRIVATE cxx_std_20)
    target_compile_definitions(test_studio_meta_contract PRIVATE
        META_PATH="${ROOT_DIR}/shared/tuneit-meta.json")
    target_link_libraries(test_studio_meta_contract PRIVATE JFramework::platform)
    add_test(NAME studio_meta_contract COMMAND test_studio_meta_contract)
else()
    message(STATUS "JFramework SDK not found — skipping the studio meta contract test")
endif()

# The shared PI loop's anti-windup (idle, boost, VVT, alternator, cruise, traction, lambda all use it).
add_executable(test_pi_controller test_pi_controller.cpp)
target_include_directories(test_pi_controller PRIVATE ${FW_DIR})
add_test(NAME pi_controller COMMAND test_pi_controller)

# Audit T2: dwell re-fitted to its spark; maximum-dwell cutoff.
add_executable(test_dwell_fit test_dwell_fit.cpp
    ${SCHED_DIR}/EventScheduler.cpp ${SCHED_DIR}/CycleRecorder.cpp ${GEN_DIR}/default_config.cpp)
target_include_directories(test_dwell_fit PRIVATE
    ${GEN_DIR} ${GEN_DIR}/modules ${GEN_DIR}/boards ${SCHED_DIR} ${FW_DIR})
target_link_libraries(test_dwell_fit PRIVATE platform_stub)
add_test(NAME dwell_fit COMMAND test_dwell_fit)

# Audit T4: angle clock error under cranking speed ripple.
add_executable(test_angle_clock_ripple test_angle_clock_ripple.cpp ${SCHED_DIR}/VirtualTrigger.cpp)
target_include_directories(test_angle_clock_ripple PRIVATE ${GEN_DIR} ${GEN_DIR}/modules ${GEN_DIR}/boards ${SCHED_DIR} ${FW_DIR})
add_test(NAME angle_clock_ripple COMMAND test_angle_clock_ripple)
