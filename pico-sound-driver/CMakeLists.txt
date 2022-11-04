cmake_minimum_required(VERSION 3.13)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(sound C CXX ASM)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(sound
    main.cpp
    usb_descriptors.c
)

# Add pico_stdlib library which aggregates commonly used features
target_link_libraries(sound pico_stdlib hardware_timer hardware_pwm hardware_gpio pico_sync pico_multicore pico_bootrom tinyusb_device)

pico_enable_stdio_usb(sound 0)
pico_enable_stdio_uart(sound 0)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(sound)