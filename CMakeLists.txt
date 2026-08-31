cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME gateway_board)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -T "${CMAKE_SOURCE_DIR}/STM32G491XX_FLASH.ld")

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/can_bus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/telemetry_thread.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/telemetry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/telemetry_hooks.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/telemetry_uart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/board_link_uart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/ltc2990.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/data_acq.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
    CAN_BUS_RX_RING_DEPTH=32
    CAN_BUS_REASM_SLOTS=2
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# SEDSNet v4
set(SEDSNET_DEVICE_IDENTIFIER "GB" CACHE STRING "" FORCE)
set(SEDSNET_MAX_STACK_PAYLOAD "128" CACHE STRING "" FORCE)
set(SEDSNET_MAX_QUEUE_BUDGET "8192" CACHE STRING
    "Bound SEDSNet queues below the Gateway shared ThreadX pool reserve" FORCE)
set(SEDSNET_MAX_RECENT_RX_IDS "16" CACHE STRING "" FORCE)
set(SEDSNET_ENV_STARTING_QUEUE_SIZE "512" CACHE STRING
    "Leave room for each Gateway router side within the bounded queue budget" FORCE)
set(SEDSNET_ENV_QUEUE_GROW_STEP "1.25" CACHE STRING "" FORCE)
set(SEDSNET_ENV_PAYLOAD_COMPRESS_THRESHOLD "30" CACHE STRING "" FORCE)
set(SEDSNET_TARGET "thumbv7em-none-eabihf" CACHE STRING "" FORCE)
include(cmake/sedsnet_fetch.cmake)



# Option defaults to ON
option(ENABLE_TELEMETRY "Enable the telemetry subsystem" ON)
set(ENABLE_TELEMETRY ON CACHE BOOL "Enable the telemetry subsystem" FORCE)
option(ENABLE_TELEMETRY_BOARD_LINK_UART "Enable board-to-board telemetry side on LPUART1" OFF)
set(ENABLE_TELEMETRY_BOARD_LINK_UART OFF CACHE BOOL
    "Enable board-to-board telemetry side on LPUART1" FORCE)

set(TELEMETRY_TIMESYNC_MASTER_PRIO "-1" CACHE STRING
    "Timesync source priority. Values < 0 make this board a consumer; values >= 0 make it a source with that priority.")

# Print its state for debugging
message(STATUS "Telemetry enabled: ${ENABLE_TELEMETRY}")

# Define or undefine the macro globally
if(ENABLE_TELEMETRY)
    add_compile_definitions(TELEMETRY_ENABLED)
else()
    # Not defined → disabled
endif()

if(ENABLE_TELEMETRY_BOARD_LINK_UART)
    add_compile_definitions(TELEMETRY_BOARD_LINK_UART)
endif()

set(LAUNCHCORE_DEVICE_DEFINE STM32G491xx)
set(LAUNCHCORE_CMSIS_DEVICE_DIR "${CMAKE_SOURCE_DIR}/Drivers/CMSIS/Device/ST/STM32G4xx/Include")
set(LAUNCHCORE_HAL_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/Drivers/STM32G4xx_HAL_Driver/Inc")
set(LAUNCHCORE_SYSTEM_SOURCE "${CMAKE_SOURCE_DIR}/Core/Src/system_stm32g4xx.c")
set(LAUNCHCORE_STARTUP_SOURCE "${CMAKE_SOURCE_DIR}/startup_stm32g491xx.s")
set(LAUNCHCORE_FLASH_SOURCES
    "${CMAKE_SOURCE_DIR}/Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash.c"
    "${CMAKE_SOURCE_DIR}/Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ex.c")
set(LAUNCHCORE_APP_SLOT_BASE 0x08004000)
set(LAUNCHCORE_APP_VECTOR_TABLE 0x08004200)
set(LAUNCHCORE_METADATA0_BASE 0x0807E000)
set(LAUNCHCORE_DELTA_SIZE 0x00002000)
include(cmake/launchcore_stm32.cmake)
