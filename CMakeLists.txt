cmake_minimum_required(VERSION 3.25)

# -----------------------------------------------------------------------------
# micro-mongodb: a minimal MongoDB driver for the Raspberry Pi Pico 2 W
# -----------------------------------------------------------------------------

# Default the Pico SDK to the vendored submodule. An explicit
# -DPICO_SDK_PATH=... on the cmake command line overrides this; we deliberately
# do NOT honour $PICO_SDK_PATH from the environment, because users often have
# a stale value pointing at another checkout.
if(NOT DEFINED PICO_SDK_PATH)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/pico-sdk/external/pico_sdk_import.cmake")
        set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/pico-sdk" CACHE PATH "Path to Pico SDK")
        message(STATUS "Using vendored Pico SDK at ${PICO_SDK_PATH}")
    elseif(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Path to Pico SDK")
        message(STATUS "Using PICO_SDK_PATH from environment: ${PICO_SDK_PATH}")
    else()
        message(FATAL_ERROR "Pico SDK not found. Run 'git submodule update --init' or set -DPICO_SDK_PATH=...")
    endif()
endif()

# Target the Pico 2 W (RP2350 + CYW43 wifi)
set(PICO_BOARD pico2_w CACHE STRING "Board the firmware is built for")
set(PICO_PLATFORM rp2350 CACHE STRING "Pico platform target")
add_compile_definitions(PICO_PLATFORM_IS_RP2350=1)

# FreeRTOS config (matches FreeRTOSConfig.h at repo root)
set(FREERTOS_HEAP "4" CACHE STRING "" FORCE)
set(PICO_USE_FASTEST_SUPPORTED_CLOCK 1)

# Pull in the Pico SDK
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

if(PICO_SDK_VERSION_STRING VERSION_LESS "2.2.0")
    message(FATAL_ERROR "Pico SDK >= 2.2.0 required (have ${PICO_SDK_VERSION_STRING})")
endif()

# Locate or download FreeRTOS-Kernel. We DO NOT add it as a subdirectory yet --
# its CMakeLists tries to auto-detect a host port if invoked before the
# cross-toolchain is active, so we defer the actual include() until after
# project() + pico_sdk_init() have set up the arm-none-eabi-gcc toolchain.
if(NOT DEFINED FREERTOS_KERNEL_PATH)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/FreeRTOS-Kernel/CMakeLists.txt")
        set(FREERTOS_KERNEL_PATH "${CMAKE_CURRENT_LIST_DIR}/FreeRTOS-Kernel")
        message(STATUS "Using vendored FreeRTOS-Kernel: ${FREERTOS_KERNEL_PATH}")
    else()
        set(FREERTOS_KERNEL_PATH "${CMAKE_BINARY_DIR}/_deps/freertos_kernel-src")
        if(NOT EXISTS "${FREERTOS_KERNEL_PATH}/CMakeLists.txt")
            message(STATUS "Cloning raspberrypi/FreeRTOS-Kernel into ${FREERTOS_KERNEL_PATH} (one-shot)...")
            execute_process(
                COMMAND git clone --depth 1 https://github.com/raspberrypi/FreeRTOS-Kernel.git ${FREERTOS_KERNEL_PATH}
                RESULT_VARIABLE _clone_rc
            )
            if(NOT _clone_rc EQUAL 0)
                message(FATAL_ERROR "Failed to clone FreeRTOS-Kernel (rc=${_clone_rc}). Drop it into ./FreeRTOS-Kernel/ manually or set -DFREERTOS_KERNEL_PATH=...")
            endif()
        endif()
    endif()
endif()

project(micro_mongodb
        VERSION "0.1.0"
        DESCRIPTION "Minimal MongoDB driver for the RP2350 (Pico 2 W)"
        LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Now that the toolchain is active, define the freertos_config interface and
# bring in the FreeRTOS-Kernel CMakeLists via its RP2350 ARM-NTZ import shim.
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config SYSTEM INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake)

message(STATUS "Building micro-mongodb ${PROJECT_VERSION} for ${PICO_PLATFORM} (board=${PICO_BOARD})")

add_subdirectory(src)
