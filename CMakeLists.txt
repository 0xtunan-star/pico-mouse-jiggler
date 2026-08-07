# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)


set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)


# Pico SDK path
set(PICO_SDK_PATH "/Users/gx1000/code/pico-sdk")


set(PICO_BOARD pico CACHE STRING "Board type")



# Import Pico SDK

include(pico_sdk_import.cmake)



if (PICO_SDK_VERSION_STRING VERSION_LESS "1.4.0")

    message(FATAL_ERROR 
    "Raspberry Pi Pico SDK version 1.4.0 or later required")

endif()



project(usb_mouse C CXX ASM)



# Initialize SDK

pico_sdk_init()



# ============================
# Source files
# ============================


add_executable(usb_mouse

    main.c

    usb_descriptors.c

)



# PIO

pico_generate_pio_header(
    usb_mouse
    ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio
)



# Program information

pico_set_program_name(
    usb_mouse
    "usb_mouse"
)


pico_set_program_version(
    usb_mouse
    "1.0"
)



# Disable stdio

pico_enable_stdio_uart(
    usb_mouse
    0
)


pico_enable_stdio_usb(
    usb_mouse
    0
)



# ============================
# Libraries
# ============================


target_link_libraries(
    usb_mouse

    pico_stdlib

    tinyusb_device

    tinyusb_board

    hardware_pio
)



# Include directories

target_include_directories(
    usb_mouse PRIVATE

    ${CMAKE_CURRENT_LIST_DIR}

)



# ============================
# TinyUSB config
# ============================


target_compile_definitions(
    usb_mouse PRIVATE

    PICO_TINYUSB_IMPL=1

)



# Generate UF2

pico_add_extra_outputs(
    usb_mouse
)



# ============================
# Flash target
# ============================


add_custom_target(
    flash

    COMMAND ${CMAKE_COMMAND}
    -E copy_if_different

    ${CMAKE_BINARY_DIR}/usb_mouse.uf2

    /Volumes/RPI-RP2/

    DEPENDS usb_mouse

    COMMENT
    "Copying usb_mouse.uf2 to Pico"

    VERBATIM
)
