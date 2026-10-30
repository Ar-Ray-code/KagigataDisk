cmake_minimum_required(VERSION 3.13)

# KagigataDisk as a FIDO2 (CTAP 2.0) USB security key. Same hardware as
# example/001 (bare RP2354A, boards/pico2_rp2354a.h); USB is a single FIDO HID
# interface and nothing uses the SPI side.
set(PICO_BOARD pico2_rp2354a CACHE STRING "Board type")
set(PICO_PLATFORM rp2350 CACHE STRING "Target platform")
list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_BOARD_HEADER_DIRS ${PICO_BOARD_HEADER_DIRS} CACHE STRING "" FORCE)

include(pico_sdk_import.cmake)

project(kagigatadisk_fido C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ---------------------------------------------------------------------------
# Build-time identity. Everything that makes one product (or one batch of
# keys) different from another is set here, e.g.
#   cmake ... -DKGFIDO_AAGUID=$(uuidgen | tr -d -) \
#             -DKGFIDO_ATTEST_KEY=attest/attest_key.hex \
#             -DKGFIDO_ATTEST_CERT=attest/attest_cert.der
# (tools/fido_attest.sh makes the key and certificate).
# ---------------------------------------------------------------------------
# USB IDs: the defaults are quoted from TinyUSB 0.18.0's device example
# examples/device/hid_generic_inout (VID 0xCafe, PID 0x4000 | HID bit = 0x4004),
# as example/001 quotes msc_dual_lun's 0x4002. They are example IDs, not
# assigned to KagigataDisk: set your own for anything you distribute.
set(KGFIDO_USB_VID "0xCAFE" CACHE STRING "USB vendor ID (default: TinyUSB example)")
set(KGFIDO_USB_PID "0x4004" CACHE STRING "USB product ID (default: TinyUSB example)")
set(KGFIDO_MANUFACTURER "TinyUSB" CACHE STRING "USB manufacturer string (default: TinyUSB example)")
set(KGFIDO_PRODUCT "TinyUSB Device" CACHE STRING "USB product string (default: TinyUSB example)")
set(KGFIDO_AAGUID "00000000000000000000000000000000" CACHE STRING
    "AAGUID, 32 hex digits (dashes allowed)")
set(KGFIDO_ATTEST_KEY "" CACHE FILEPATH
    "attestation private key: a file with the P-256 scalar as 64 hex digits; empty = self attestation")
set(KGFIDO_ATTEST_CERT "" CACHE FILEPATH
    "attestation certificate (DER) matching KGFIDO_ATTEST_KEY")
# Every request needs a touch; there is no build that approves without one.
# (Dropped from the cache first, so a rejected -DKGFIDO_UP=... does not stick.)
set(kgfido_up "${KGFIDO_UP}")
unset(KGFIDO_UP CACHE)
if(kgfido_up AND NOT kgfido_up STREQUAL "touch")
  message(FATAL_ERROR "KGFIDO_UP was removed: a touch is always required")
endif()
set(KGFIDO_TOUCH_PINS "0,1,2,7" CACHE STRING "GPIOs on the card contacts used as the touch sensor")
set(KGFIDO_FW_VERSION "0.1.0" CACHE STRING "firmware version string")

function(kgfido_hex_bytes hex out)
  string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," arr "${hex}")
  set(${out} "${arr}" PARENT_SCOPE)
endfunction()

foreach(v KGFIDO_USB_VID KGFIDO_USB_PID)
  if(NOT ${v} MATCHES "^0x[0-9a-fA-F][0-9a-fA-F]?[0-9a-fA-F]?[0-9a-fA-F]?$")
    message(FATAL_ERROR "${v} must be a 16-bit hex number like 0x1234, not '${${v}}'")
  endif()
endforeach()
math(EXPR vid "${KGFIDO_USB_VID}")
math(EXPR pid "${KGFIDO_USB_PID}")
if(vid EQUAL 0xCAFE AND pid EQUAL 0x4004)
  message(WARNING "KGFIDO_USB_VID/PID are TinyUSB's example IDs (cafe:4004, "
                  "examples/device/hid_generic_inout): set your own")
endif()
foreach(v KGFIDO_MANUFACTURER KGFIDO_PRODUCT KGFIDO_FW_VERSION)
  string(LENGTH "${${v}}" len)
  if(len EQUAL 0 OR len GREATER 32 OR NOT ${v} MATCHES "^[] -!#-[^-~]+$")
    message(FATAL_ERROR "${v} must be 1-32 printable ASCII characters without \" or \\")
  endif()
endforeach()

string(REPLACE "-" "" aaguid "${KGFIDO_AAGUID}")
if(NOT aaguid MATCHES "^[0-9a-fA-F]+$")
  message(FATAL_ERROR "KGFIDO_AAGUID must be 32 hex digits")
endif()
string(LENGTH "${aaguid}" len)
if(NOT len EQUAL 32)
  message(FATAL_ERROR "KGFIDO_AAGUID must be 32 hex digits")
endif()
if(aaguid MATCHES "^0+$")
  message(WARNING "KGFIDO_AAGUID is all zeros: set one for your product (uuidgen)")
endif()
kgfido_hex_bytes("${aaguid}" KGFIDO_AAGUID_BYTES)

if(KGFIDO_ATTEST_KEY OR KGFIDO_ATTEST_CERT)
  if(NOT EXISTS "${KGFIDO_ATTEST_KEY}" OR NOT EXISTS "${KGFIDO_ATTEST_CERT}")
    message(FATAL_ERROR "KGFIDO_ATTEST_KEY and KGFIDO_ATTEST_CERT must both name existing files")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               "${KGFIDO_ATTEST_KEY}" "${KGFIDO_ATTEST_CERT}")
  file(READ "${KGFIDO_ATTEST_KEY}" key)
  string(REGEX REPLACE "[ \t\r\n]" "" key "${key}")
  string(LENGTH "${key}" len)
  if(NOT key MATCHES "^[0-9a-fA-F]+$" OR NOT len EQUAL 64)
    message(FATAL_ERROR "${KGFIDO_ATTEST_KEY}: expected the private scalar as 64 hex digits")
  endif()
  file(READ "${KGFIDO_ATTEST_CERT}" cert HEX)
  if(NOT cert MATCHES "^30")
    message(FATAL_ERROR "${KGFIDO_ATTEST_CERT}: expected a DER certificate (not PEM)")
  endif()
  string(LENGTH "${cert}" len)
  math(EXPR KGFIDO_ATTEST_CERT_LEN "${len} / 2")
  if(KGFIDO_ATTEST_CERT_LEN GREATER 1024)
    message(FATAL_ERROR "${KGFIDO_ATTEST_CERT}: certificates over 1024 bytes are not supported")
  endif()
  kgfido_hex_bytes("${key}" KGFIDO_ATTEST_KEY_BYTES)
  kgfido_hex_bytes("${cert}" KGFIDO_ATTEST_CERT_BYTES)
  set(KGFIDO_ATTEST_BATCH 1)
else()
  set(KGFIDO_ATTEST_KEY_BYTES "0")
  set(KGFIDO_ATTEST_CERT_BYTES "0")
  set(KGFIDO_ATTEST_CERT_LEN 0)
  set(KGFIDO_ATTEST_BATCH 0)
endif()

set(KGFIDO_TOUCH_PIN_MASK 0)
string(REPLACE "," ";" pins "${KGFIDO_TOUCH_PINS}")
foreach(p IN LISTS pins)
  if(NOT p MATCHES "^[0-9]+$" OR p GREATER 29)
    message(FATAL_ERROR "KGFIDO_TOUCH_PINS: '${p}' is not a GPIO number")
  endif()
  math(EXPR KGFIDO_TOUCH_PIN_MASK "${KGFIDO_TOUCH_PIN_MASK} | (1 << ${p})" OUTPUT_FORMAT HEXADECIMAL)
endforeach()

# The private key ends up in this generated header, inside the build directory.
configure_file(src/kgfido_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/kgfido_config.h @ONLY)

set(KGFIDO_SOURCES
    src/main.c
    src/usb_desc.c
    src/ctaphid.c
    src/ctap2.c
    src/cbor.c
    src/crypto.c
    src/store.c
    src/fstore.c
    src/touch.c
    src/led.c
)
add_executable(kagigatadisk_fido ${KGFIDO_SOURCES})
# Warnings for this firmware's own code (not the SDK or Mbed TLS sources).
set_source_files_properties(${KGFIDO_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

target_include_directories(kagigatadisk_fido PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)

target_link_libraries(kagigatadisk_fido
    pico_stdlib
    pico_unique_id
    hardware_resets
    pico_mbedtls
    hardware_flash
    tinyusb_device
)

# USB is the FIDO HID interface only; there is no stdio at all.
pico_enable_stdio_uart(kagigatadisk_fido 0)
pico_enable_stdio_usb(kagigatadisk_fido 0)

pico_add_extra_outputs(kagigatadisk_fido)
