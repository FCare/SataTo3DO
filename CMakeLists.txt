cmake_minimum_required(VERSION 3.13)
include(pico-sdk/pico_sdk_init.cmake)

set(FAMILY rp2040)
set(BOARD pico_sdk)
set(PICO_TINYUSB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tinyusb")

set(PICO_BOARD waveshare_rp2040_zero)

include(${PICO_TINYUSB_PATH}/hw/bsp/${FAMILY}/family.cmake)

set(CFG_TUSB_CONFIG_FILE "include/tusb_config.h")


project(SataTo3DO)

add_definitions(-DPICO_TINYUSB_PATH=${PICO_TINYUSB_PATH})

pico_sdk_init()

include_directories(./include)
include_directories(./include/controllers)
include_directories(./include/interfaces)
include_directories(./include/gamepads)


add_executable(SataTo3DO
src/main.c
src/interfaces/USB.c
)

# enable usb output, disable uart output
# pico_enable_stdio_usb(SataTo3DO 1)
pico_enable_stdio_uart(SataTo3DO 1)

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(SataTo3DO)

target_link_libraries(SataTo3DO pico_stdlib pico_multicore tinyusb_board tinyusb_host)