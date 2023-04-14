cmake_minimum_required(VERSION 3.13)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(sound C CXX ASM)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(sound
    main.cpp
    btle.cpp
    hd44780.cpp
    mcp3428.cpp
    usb_descriptors.c
)

target_include_directories(sound PRIVATE ${PROJECT_SOURCE_DIR})

target_compile_definitions(sound PRIVATE CYW43_LWIP=0 CYW43_ENABLE_BLUETOOTH=1 CYW43_GPIO=1 PICO_CYW43_ARCH_POLL=1)

pico_btstack_make_gatt_header(sound PRIVATE ${PROJECT_SOURCE_DIR}/midi-btle.gatt)

# Add pico_stdlib library which aggregates commonly used features
target_link_libraries(sound
    pico_stdlib
    hardware_timer
    hardware_pwm
    hardware_flash
    hardware_gpio
    hardware_i2c
    hardware_adc
    cyw43_driver
    pico_sync
    pico_multicore
    pico_async_context_base
    pico_async_context_poll
    pico_bootrom
    pico_btstack_run_loop_async_context
    pico_btstack_ble
    pico_btstack_cyw43
    pico_cyw43_arch_poll
    pico_cyw43_driver
    pico_unique_id
    tinyusb_device
)

pico_enable_stdio_usb(sound 0)
pico_enable_stdio_uart(sound 0)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(sound)