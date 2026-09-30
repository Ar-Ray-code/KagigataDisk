cmake_minimum_required(VERSION 3.13)

# Actual target hardware: a bare RP2354A (RP2350A pinout/package, 2 MiB of
# flash on-die instead of 4 MiB external). pico-sdk 2.3.0 has no stock board
# file for it, so boards/pico2_rp2354a.h supplies one - see that file and
# https://forums.raspberrypi.com/viewtopic.php?t=398455.
#   cmake -S . -B build -G Ninja -DPICO_BOARD=pico2_w   # back to a Pico 2 W
#                                                        # dev board instead
set(PICO_BOARD pico2_rp2354a CACHE STRING "Board type")
set(PICO_PLATFORM rp2350 CACHE STRING "Target platform")
list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_BOARD_HEADER_DIRS ${PICO_BOARD_HEADER_DIRS} CACHE STRING "" FORCE)

include(pico_sdk_import.cmake)

project(kagigatadisk C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(kagigatadisk
    src/main.c
    src/spi_slave_transport.c
    src/emu_link.c
    src/emu_server.c
    src/objstore.c
    src/fstore.c
    src/usbdisk.c
    src/usb_device.c
    src/skill_md.S
    src/led.c
    src/devinfo.c
)

# The USB drive's /SKILL.md and /spi_virtual_device/README.md, embedded
# verbatim (src/skill_md.S).
set(SKILL_MD ${CMAKE_CURRENT_LIST_DIR}/src/SKILL.md)
set(README_MD ${CMAKE_CURRENT_LIST_DIR}/src/README_spi.md)
set_source_files_properties(src/skill_md.S PROPERTIES
    COMPILE_DEFINITIONS "SKILL_MD_PATH=\"${SKILL_MD}\";README_MD_PATH=\"${README_MD}\""
    OBJECT_DEPENDS "${SKILL_MD};${README_MD}")

# Everything the board serves is ASCII: stop the build if either document is not.
add_custom_target(ascii_check
    COMMAND ${CMAKE_COMMAND} -DFILE1=${SKILL_MD} -DFILE2=${README_MD}
            -P ${CMAKE_CURRENT_LIST_DIR}/check_ascii.cmake
    VERBATIM
    COMMENT "Checking that the embedded documents are ASCII")
add_dependencies(kagigatadisk ascii_check)

pico_generate_pio_header(kagigatadisk ${CMAKE_CURRENT_LIST_DIR}/src/spi_slave_transport.pio)

# protocol/emu_protocol_defs.h is shared with the host library, which keeps a
# copy of it; the build stops if the two ever differ.
set(PROTOCOL_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../protocol)
target_include_directories(kagigatadisk PRIVATE src ${PROTOCOL_DIR})
add_custom_target(protocol_check
    COMMAND ${CMAKE_COMMAND} -DORIGINAL=${PROTOCOL_DIR}/emu_protocol_defs.h
            -DCOPY=${CMAKE_CURRENT_LIST_DIR}/../host/lib/emu_storage/src/emu_protocol_defs.h
            -P ${CMAKE_CURRENT_LIST_DIR}/check_protocol.cmake
    COMMENT "Checking the host library's copy of emu_protocol_defs.h")
add_dependencies(kagigatadisk protocol_check)

# Reported by GET_INFO.
set(KAGIGATA_FW_VERSION "0.2.0" CACHE STRING "firmware version string")
target_compile_definitions(kagigatadisk PRIVATE KAGIGATA_FW_VERSION="${KAGIGATA_FW_VERSION}")

target_link_libraries(kagigatadisk
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_pio
    hardware_flash
    tinyusb_device
)

# USB is the mass-storage drive (usbdisk.c); there is no stdio at all.
pico_enable_stdio_uart(kagigatadisk 0)
pico_enable_stdio_usb(kagigatadisk 0)

pico_add_extra_outputs(kagigatadisk)
