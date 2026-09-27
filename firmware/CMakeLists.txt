cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Toolchain must be set BEFORE project().  Pass via cmake CLI:
#   cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/stm32f7xx.cmake -S firmware -B firmware/build
# ---------------------------------------------------------------------------

project(jayecu C CXX ASM)

set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

set(ROOT_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(GEN_DIR     "${ROOT_DIR}/generated")
set(FW_DIR      "${CMAKE_CURRENT_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# Board selection — controls which board_hal implementation is compiled.
# ---------------------------------------------------------------------------
set(BOARD "jaytek_v1" CACHE STRING "Target board")
set_property(CACHE BOARD PROPERTY STRINGS jaytek_v1 proteus_f7)
message(STATUS "Board: ${BOARD}")

set(BOARD_DIR     "${FW_DIR}/Platform/boards/${BOARD}")
set(BOARD_HAL_SRC "${BOARD_DIR}/board_hal_${BOARD}.cpp" "${BOARD_DIR}/${BOARD}_profile.cpp")

# ---------------------------------------------------------------------------
# STM32CubeF7 / FreeRTOS — fetched automatically from GitHub.
# We use a persistent cache directory to avoid re-downloads if build/ is wiped.
# ---------------------------------------------------------------------------

# Set persistent base directory for downloads (outside of build/)
set(FETCHCONTENT_BASE_DIR "${ROOT_DIR}/.stm32_cache")
# Show progress so the build doesn't look frozen during download
set(FETCHCONTENT_QUIET OFF)

include(FetchContent)

FetchContent_Declare(
    stm32cube_f7
    GIT_REPOSITORY https://github.com/STMicroelectronics/STM32CubeF7.git
    GIT_TAG        v1.17.1
)

message(STATUS "Checking STM32CubeF7 dependency (fetching if not present in ${FETCHCONTENT_BASE_DIR})...")
FetchContent_MakeAvailable(stm32cube_f7)

set(CUBE_DIR     "${stm32cube_f7_SOURCE_DIR}")

set(HAL_DIR      "${CUBE_DIR}/Drivers/STM32F7xx_HAL_Driver")
set(CMSIS_DIR    "${CUBE_DIR}/Drivers/CMSIS")
set(FREERTOS_DIR "${CUBE_DIR}/Middlewares/Third_Party/FreeRTOS/Source")
set(USB_MW_DIR   "${CUBE_DIR}/Middlewares/ST/STM32_USB_Device_Library")

# ---------------------------------------------------------------------------
# MCU-specific compile flags
# ---------------------------------------------------------------------------

set(MCU_FLAGS
    -mcpu=cortex-m7
    -mfpu=fpv5-d16
    -mfloat-abi=hard
    -mthumb
)

# Default to Debug build when running CMake without -DCMAKE_BUILD_TYPE.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Debug → -Og -g3 (good for stepping + breakpoints, mild optimisation so flow
# is sensible but variables remain inspectable). Release → -Os -g (small, but
# keeps DWARF for crash decoding).
set(CMAKE_C_FLAGS_DEBUG     "-Og -g3 -gdwarf-4 -fno-omit-frame-pointer"
    CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_DEBUG   "-Og -g3 -gdwarf-4 -fno-omit-frame-pointer"
    CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS_DEBUG   "-g3 -gdwarf-4"
    CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELEASE   "-Os -g -gdwarf-4"
    CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-Os -g -gdwarf-4"
    CACHE STRING "" FORCE)

add_compile_options(
    ${MCU_FLAGS}
    -Wall -Wextra
    -ffunction-sections
    -fdata-sections
    # C++-only — guarded so they aren't applied to .c sources (which warns).
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)
add_link_options(
    ${MCU_FLAGS}
    -T "${FW_DIR}/Platform/stm32f7xx/STM32F767XX_FLASH.ld"
    -Wl,--gc-sections
    # Bare-metal flash image: the single PT_LOAD spans text+data so ld (>=2.39)
    # flags it RWX. No runtime loader enforces W^X here, so silence the note.
    -Wl,--no-warn-rwx-segments
    -Wl,-Map=jayecu.map
    --specs=nano.specs
    --specs=nosys.specs
)
message(STATUS "JayECU build type: ${CMAKE_BUILD_TYPE}")

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------

include_directories(
    ${FW_DIR}
    ${FW_DIR}/Platform/stm32f7xx
    ${FW_DIR}/Led
    ${FW_DIR}/Storage
    ${ROOT_DIR}/third_party/fatfs
    ${BOARD_DIR}
    ${GEN_DIR}
    ${GEN_DIR}/modules
    ${GEN_DIR}/boards
    ${HAL_DIR}/Inc
    ${CMSIS_DIR}/Device/ST/STM32F7xx/Include
    ${CMSIS_DIR}/Include
    ${FREERTOS_DIR}/include
    ${FREERTOS_DIR}/portable/GCC/ARM_CM7/r0p1
    ${USB_MW_DIR}/Core/Inc
    ${USB_MW_DIR}/Class/CDC/Inc
    ${USB_MW_DIR}/Class/MSC/Inc
    ${USB_MW_DIR}/Class/CompositeBuilder/Inc
)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------

file(GLOB SCHEDULER_SRC
    ${FW_DIR}/Scheduler/EventScheduler.cpp
    ${FW_DIR}/Scheduler/VirtualTrigger.cpp
    ${FW_DIR}/Scheduler/EnginePositionHal.cpp
    ${FW_DIR}/Scheduler/SegmentTimer.cpp
    ${FW_DIR}/Scheduler/EngineSyncSampler.cpp
    ${FW_DIR}/Scheduler/CycleRecorder.cpp
    ${FW_DIR}/Scheduler/Log.cpp
)

file(GLOB ENGINE_SRC
    ${FW_DIR}/Engine/Modules/RevLimiter.cpp
    ${FW_DIR}/Engine/Modules/Alternator.cpp
    ${FW_DIR}/Engine/Modules/Boost.cpp
    ${FW_DIR}/Engine/Modules/Launch.cpp
    ${FW_DIR}/Engine/Modules/Lambda.cpp
    ${FW_DIR}/Engine/Modules/TransientThrottle.cpp
    ${FW_DIR}/Engine/Modules/FuelCalculator.cpp
    ${FW_DIR}/Engine/Modules/Ignition.cpp
    ${FW_DIR}/Engine/Modules/IgnitionTrim.cpp
    ${FW_DIR}/Engine/Modules/FuelTrim.cpp
    ${FW_DIR}/Engine/Modules/Knock.cpp
    ${FW_DIR}/Engine/Modules/Misfire.cpp
    ${FW_DIR}/Engine/Modules/KnockDetector.cpp
    ${FW_DIR}/Engine/Modules/KnockDsp.cpp
    ${FW_DIR}/Engine/Modules/Biquad.cpp
    ${FW_DIR}/Engine/Modules/Idle.cpp
    ${FW_DIR}/Engine/Modules/App.cpp
    ${FW_DIR}/Engine/Modules/ElectronicThrottle.cpp
    ${FW_DIR}/Engine/Modules/EngineProtection.cpp
    ${FW_DIR}/Engine/Modules/Dfco.cpp
    ${FW_DIR}/Engine/Modules/LambdaProtect.cpp
    ${FW_DIR}/Engine/Modules/VehicleSpeed.cpp
    ${FW_DIR}/Engine/Modules/GearDetect.cpp
    ${FW_DIR}/Engine/Modules/FlatShift.cpp
    ${FW_DIR}/Engine/Modules/TractionControl.cpp
    ${FW_DIR}/Engine/Modules/VvtControl.cpp
    ${FW_DIR}/Engine/Modules/PitLimiter.cpp
    ${FW_DIR}/Engine/Modules/Nitrous.cpp
    ${FW_DIR}/Engine/Modules/Wmi.cpp
    ${FW_DIR}/Engine/Modules/AntiLag.cpp
    ${FW_DIR}/Engine/Modules/Vvl.cpp
    ${FW_DIR}/Engine/Modules/EgtProtect.cpp
    ${FW_DIR}/Engine/Modules/CruiseControl.cpp
    ${FW_DIR}/Engine/Modules/TorqueModel.cpp
    ${FW_DIR}/Engine/Modules/HBridge.cpp
    ${FW_DIR}/Engine/Modules/OutputTest.cpp
    ${FW_DIR}/Engine/Modules/Stepper.cpp
    ${FW_DIR}/Engine/EngineTask.cpp
    ${FW_DIR}/Engine/SystemComposer.cpp
    ${FW_DIR}/Integration/OutputManager.cpp
    ${FW_DIR}/Integration/HardwareInput.cpp
)

set(LED_SRC
    ${FW_DIR}/Led/LedTask.cpp
)

set(STORAGE_SRC
    ${FW_DIR}/Storage/SdDtcStore.cpp
    ${FW_DIR}/Storage/ConfigBank.cpp
    ${FW_DIR}/Storage/SdArbitrator.cpp
    ${FW_DIR}/Storage/StorageManager.cpp
    ${FW_DIR}/Platform/stm32f7xx/SdCardSpi.c
    ${FW_DIR}/Platform/stm32f7xx/SdConfigStore.cpp
    ${FW_DIR}/Platform/stm32f7xx/LearnedStore.cpp
    ${FW_DIR}/Platform/stm32f7xx/SdVolume.cpp
    ${FW_DIR}/Platform/stm32f7xx/Stm32FlashBank.cpp
    ${FW_DIR}/Platform/stm32f7xx/SdArbitratorShim.cpp
    ${FW_DIR}/Platform/stm32f7xx/diskio.c
)

# FatFS R0.14b — third_party sources + project-local FreeRTOS sync impl
set(FATFS_DIR "${ROOT_DIR}/third_party/fatfs")
set(FATFS_SRC
    ${FATFS_DIR}/ff.c
    ${FATFS_DIR}/ffunicode.c
    ${FW_DIR}/Platform/stm32f7xx/ffsystem.c
)

file(GLOB COMMS_SRC
    ${FW_DIR}/Comms/PacketParser.cpp
    ${FW_DIR}/Comms/CommsManager.cpp
    ${FW_DIR}/Comms/SdProtocol.cpp
    ${FW_DIR}/Comms/MlgLog.cpp
    ${FW_DIR}/Cli/CliRegistry.cpp
    ${FW_DIR}/Cli/CliCommands.cpp
)

set(USB_MSC_DIR "${USB_MW_DIR}/Class/MSC")

set(USB_SRC
    ${USB_MW_DIR}/Core/Src/usbd_core.c
    ${USB_MW_DIR}/Core/Src/usbd_ctlreq.c
    ${USB_MW_DIR}/Core/Src/usbd_ioreq.c
    ${USB_MW_DIR}/Class/CDC/Src/usbd_cdc.c
    ${USB_MW_DIR}/Class/CompositeBuilder/Src/usbd_composite_builder.c
    # ST's MSC class (usbd_msc/bot/scsi/data) is REPLACED by our thread-based BOT
    # class in usb_msc.cpp (see usb_msc.h for why). The composite builder
    # still emits the MSC descriptor from CLASS_TYPE_MSC, so it stays.
)

set(PLATFORM_SRC
    ${FW_DIR}/Platform/stm32f7xx/platform_hal_stm32.cpp
    ${FW_DIR}/Platform/stm32f7xx/Stm32AdcUnit.cpp
    ${FW_DIR}/Platform/stm32f7xx/Stm32I2cBus.cpp
    ${FW_DIR}/Platform/devices/Lps2xBaro.cpp
    ${FW_DIR}/Platform/stm32f7xx/Stm32CanChannel.cpp
    ${FW_DIR}/Platform/stm32f7xx/PlatformCan.cpp
    ${FW_DIR}/Platform/stm32f7xx/Stm32Capture.cpp
    ${FW_DIR}/Platform/stm32f7xx/Stm32OutputCompare.cpp
    ${FW_DIR}/Platform/stm32f7xx/Stm32SoftTimer.cpp
    ${FW_DIR}/Platform/stm32f7xx/Stm32DmaPwm.cpp
    ${FW_DIR}/Platform/AssignmentResolver.cpp
    ${FW_DIR}/Platform/stm32f7xx/usb_device.cpp
    ${FW_DIR}/Platform/stm32f7xx/usbd_conf.c
    ${FW_DIR}/Platform/stm32f7xx/usbd_desc.c
    ${FW_DIR}/Platform/stm32f7xx/usbd_cdc_if.cpp
    ${FW_DIR}/Platform/stm32f7xx/usb_msc.cpp
    ${FW_DIR}/Platform/stm32f7xx/system_stm32f7xx.c   # local — full clock setup at SystemInit
    ${FW_DIR}/Platform/stm32f7xx/syscalls.c           # newlib syscall stubs (silence link warnings)
    ${BOARD_HAL_SRC}
)

set(SENSORS_SRC
    ${FW_DIR}/Sensors/Sensors.cpp
    ${FW_DIR}/Diagnostics/CpuStats.cpp
    ${FW_DIR}/Diagnostics/ReconfigCost.cpp
    ${FW_DIR}/Diagnostics/DtcManager.cpp
)

set(SCRIPTING_SRC
    ${FW_DIR}/Scripting/ScriptEngine.cpp
    ${FW_DIR}/Diagnostics/TextLog.cpp
)

# Lua 5.4 — compiled from source alongside firmware.
set(LUA_DIR "${ROOT_DIR}/third_party/lua/src")
file(GLOB LUA_SRCS ${LUA_DIR}/*.c)
list(FILTER LUA_SRCS EXCLUDE REGEX ".*(lua|luac)\\.c$")
add_library(lua54_fw STATIC ${LUA_SRCS})
target_include_directories(lua54_fw PUBLIC ${LUA_DIR})
target_compile_options(lua54_fw PRIVATE 
    ${MCU_FLAGS} 
    -w 
    -std=c99
)
target_compile_definitions(lua54_fw PUBLIC LUA_32BITS)

set(CAN_SRC
    ${FW_DIR}/Can/CanBroker.cpp
    ${FW_DIR}/Can/GenericCan.cpp
    ${FW_DIR}/Can/ObdResponder.cpp
)

set(FREERTOS_SRC
    ${FREERTOS_DIR}/croutine.c
    ${FREERTOS_DIR}/event_groups.c
    ${FREERTOS_DIR}/list.c
    ${FREERTOS_DIR}/queue.c
    ${FREERTOS_DIR}/stream_buffer.c
    ${FREERTOS_DIR}/tasks.c
    ${FREERTOS_DIR}/timers.c
    ${FREERTOS_DIR}/portable/MemMang/heap_4.c
    ${FREERTOS_DIR}/portable/GCC/ARM_CM7/r0p1/port.c
)

set(HAL_SRC
    ${HAL_DIR}/Src/stm32f7xx_hal.c
    ${HAL_DIR}/Src/stm32f7xx_hal_cortex.c
    ${HAL_DIR}/Src/stm32f7xx_hal_dma.c
    ${HAL_DIR}/Src/stm32f7xx_hal_dma_ex.c
    ${HAL_DIR}/Src/stm32f7xx_hal_flash.c
    ${HAL_DIR}/Src/stm32f7xx_hal_flash_ex.c
    ${HAL_DIR}/Src/stm32f7xx_hal_gpio.c
    ${HAL_DIR}/Src/stm32f7xx_hal_i2c.c
    ${HAL_DIR}/Src/stm32f7xx_hal_i2c_ex.c
    ${HAL_DIR}/Src/stm32f7xx_hal_rcc.c
    ${HAL_DIR}/Src/stm32f7xx_hal_rcc_ex.c
    ${HAL_DIR}/Src/stm32f7xx_hal_rtc.c
    ${HAL_DIR}/Src/stm32f7xx_hal_rtc_ex.c
    ${HAL_DIR}/Src/stm32f7xx_hal_pwr.c
    ${HAL_DIR}/Src/stm32f7xx_hal_pwr_ex.c
    ${HAL_DIR}/Src/stm32f7xx_hal_spi.c
    ${HAL_DIR}/Src/stm32f7xx_hal_uart.c
    ${HAL_DIR}/Src/stm32f7xx_hal_tim.c
    ${HAL_DIR}/Src/stm32f7xx_hal_tim_ex.c
    ${HAL_DIR}/Src/stm32f7xx_hal_adc.c
    ${HAL_DIR}/Src/stm32f7xx_hal_adc_ex.c
    ${HAL_DIR}/Src/stm32f7xx_hal_pcd.c
    ${HAL_DIR}/Src/stm32f7xx_hal_pcd_ex.c
    ${HAL_DIR}/Src/stm32f7xx_ll_usb.c
    ${HAL_DIR}/Src/stm32f7xx_hal_can.c
)

# Vendored STM32Cube HAL — compile with warnings OFF (-w wins over the global -Wall -Wextra).
# Not our code to fix; keeps the build's warning output to OUR sources only.
set_source_files_properties(${HAL_SRC} PROPERTIES COMPILE_OPTIONS "-w")

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------

add_executable(jayecu.elf
    ${FW_DIR}/main.cpp
    ${FW_DIR}/Platform/stm32f7xx/startup_stm32f767xx.s
    ${GEN_DIR}/default_config.cpp
    ${GEN_DIR}/default_tune.cpp          # CRC-valid default tune image → .flash_cfg_a (bank A)
    ${SCHEDULER_SRC}
    ${ENGINE_SRC}
    ${SCRIPTING_SRC}
    ${COMMS_SRC}
    ${PLATFORM_SRC}
    ${SENSORS_SRC}
    ${USB_SRC}
    ${CAN_SRC}
    ${LED_SRC}
    ${STORAGE_SRC}
    ${FATFS_SRC}
    ${FREERTOS_SRC}
    ${HAL_SRC}
)

target_include_directories(jayecu.elf PRIVATE ${LUA_DIR})
target_link_libraries(jayecu.elf PRIVATE lua54_fw)

target_compile_definitions(jayecu.elf PRIVATE
    JAYECU_FIRMWARE        # on-target build: enables FreeRTOS critical sections (host = no-op)
    STM32F767xx
    USE_HAL_DRIVER
    HSE_VALUE=8000000U
    # CDC + MSC composite USB device (ST composite builder)
    USE_USBD_COMPOSITE
    USBD_CMPSIT_ACTIVATE_CDC=1
    USBD_CMPSIT_ACTIVATE_MSC=1
)

# ---------------------------------------------------------------------------
# Post-build — generate flash images. The default-tune image lives at flash
# sector 11 (0x08180000), ~1.3 MB above the code, so a single .bin spans that
# gap with zeros (~1.6 MB) and st-flash writes the whole span (~50 s). Instead we
# emit TWO region images — code.bin (everything but the tune) and tune.bin (just
# .flash_cfg_a) — and `make flash` writes each at its own address, so only the
# code sectors + sector 11 are erased/written (~260 KB, ~10 s), skipping the gap.
# code.bin is flashed LAST with --reset so the MCU runs and USB re-enumerates.
# jayecu.bin/.hex are still emitted as whole-image artifacts.
# ---------------------------------------------------------------------------

add_custom_command(TARGET jayecu.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   jayecu.elf jayecu.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary jayecu.elf jayecu.bin
    COMMAND ${CMAKE_OBJCOPY} -O binary --remove-section .flash_cfg_a jayecu.elf code.bin
    COMMAND ${CMAKE_OBJCOPY} -O binary --only-section  .flash_cfg_a jayecu.elf tune.bin
    COMMENT "Generating flash images (code.bin + tune.bin + whole jayecu.hex/.bin)"
)

find_program(ST_FLASH st-flash)
if(ST_FLASH)
    add_custom_target(flash
        COMMAND ${ST_FLASH} write tune.bin 0x08180000              # default tune (sector 11)
        COMMAND ${ST_FLASH} --reset write code.bin 0x08000000      # code, then run
        DEPENDS jayecu.elf
        COMMENT "Flashing firmware (code + tune regions, skips the gap, --reset) via ST-Link"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()
