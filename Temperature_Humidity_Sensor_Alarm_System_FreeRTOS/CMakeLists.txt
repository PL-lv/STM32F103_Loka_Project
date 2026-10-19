cmake_minimum_required(VERSION 3.20)

# CLion code model only. Project.uvprojx remains the firmware build authority.
# Static compiler probes also permit ARM compilers without a linker script.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(Temperature_Humidity_Sensor_Alarm_System_FreeRTOS LANGUAGES C)

include(cmake/CLionIndex.cmake)

# Baseline Keil C inputs and A/B entry points for navigation. This OBJECT
# index target does not link these independent main functions together.
set(index_sources
    FreeRTOS/Source/portable/MemMang/heap_4.c
    FreeRTOS/Source/event_groups.c
    FreeRTOS/Source/list.c
    FreeRTOS/Source/queue.c
    FreeRTOS/Source/stream_buffer.c
    FreeRTOS/Source/tasks.c
    FreeRTOS/Source/timers.c
    Hardware/buzzer.c
    Hardware/Key.c
    Hardware/LED.c
    Hardware/OLED.c
    Hardware/CAN_Bus.c
    Library/misc.c
    Library/stm32f10x_adc.c
    Library/stm32f10x_bkp.c
    Library/stm32f10x_can.c
    Library/stm32f10x_cec.c
    Library/stm32f10x_crc.c
    Library/stm32f10x_dac.c
    Library/stm32f10x_dbgmcu.c
    Library/stm32f10x_dma.c
    Library/stm32f10x_exti.c
    Library/stm32f10x_flash.c
    Library/stm32f10x_fsmc.c
    Library/stm32f10x_gpio.c
    Library/stm32f10x_i2c.c
    Library/stm32f10x_iwdg.c
    Library/stm32f10x_pwr.c
    Library/stm32f10x_rcc.c
    Library/stm32f10x_rtc.c
    Library/stm32f10x_sdio.c
    Library/stm32f10x_spi.c
    Library/stm32f10x_tim.c
    Library/stm32f10x_usart.c
    Library/stm32f10x_wwdg.c
    Start/system_stm32f10x.c
    System/alarm.c
    System/Delay.c
    System/dht11.c
    System/Record_storage.c
    System/UI.c
    System/USART.c
    Tasks/task_display.c
    Tasks/task_sensor.c
    User/main.c
    User/main_environment.c
    User/main_manager.c
    User/stm32f10x_it.c
    Tasks/task_alarm.c
    Tasks/task_key.c
    Hardware/timer_delay.c
    Hardware/SPI2_Flash.c
    Hardware/W25Q64.c
    System/Threshold_storage.c
    Tasks/task_statemachine.c
    Tasks/task_record.c
)

# Original implementations remain readable. Never send Keil assembly to the
# host compiler; no ASM language is enabled in this code model.
set(browse_only_sources
    Start/core_cm3.c
    Start/startup_stm32f10x_md.s
    FreeRTOS/Source/portable/RVDS/ARM_CM3/port.c
)
set_source_files_properties(${browse_only_sources} PROPERTIES HEADER_FILE_ONLY TRUE)

# Attach headers for navigation, not separate compilation. The two distinct
# stack_macros.h / StackMacros.h names are both real files.
file(GLOB index_headers CONFIGURE_DEPENDS
    "Hardware/*.h"
    "Library/*.h"
    "Start/*.h"
    "System/*.h"
    "Tasks/*.h"
    "User/*.h"
    "FreeRTOS/Source/include/*.h"
    "FreeRTOS/Source/portable/RVDS/ARM_CM3/*.h"
)

# OBJECT supplies per-file settings to CLion and compile_commands.json.
# A standalone INTERFACE target has no compiled C translation units in the
# code model. This target never links firmware and is excluded from ALL.
add_library(CLionIndex OBJECT EXCLUDE_FROM_ALL
    ${index_sources}
    ${index_headers}
    ${browse_only_sources}
    "${clion_index_include}/portmacro.h"
)
set_target_properties(CLionIndex PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES
)
target_compile_definitions(CLionIndex PRIVATE
    STM32F10X_MD
    USE_STDPERIPH_DRIVER
    CLION_CODE_INDEX=1
)

# STM32 uses 32-bit pointers. MinGW's 32-bit analysis mode avoids misleading
# pointer-size diagnostics from its default 64-bit host model. No 32-bit CRT
# linking is required because this project has no executable/link target.
if(MINGW AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_options(CLionIndex PRIVATE -m32)
endif()

# The generated declaration view must precede the original RVDS portmacro.h.
# Remaining paths match Project.uvprojx's actual include directories.
target_include_directories(CLionIndex PRIVATE
    "${clion_index_include}"
    Start
    Library
    System
    Hardware
    Tasks
    User
    FreeRTOS/Source/include
    FreeRTOS/Source/portable/MemMang
    FreeRTOS/Source/portable/RVDS/ARM_CM3
    FreeRTOS/Source
)

# stm32f10x_it_old.c has duplicate interrupt handlers and is not a Keil input.
# warning_system.c is also absent from Keil's current file list. Both stay on
# disk but are not active translation units in this index model.
message(STATUS "CLion indexing only: use Project.uvprojx to build/debug firmware")
