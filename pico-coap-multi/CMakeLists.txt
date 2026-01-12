# pico-examples/pico_w/wifi/pico-coap-multi/CMakeLists.txt
cmake_minimum_required(VERSION 3.13)

# --- Common include path ---
set(PROJ_INC
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/../no-OS-FatFS-SD-SPI-RPi-Pico/FatFs_SPI/include
)

# --- Common CoAP sources ---
set(COAP_SRC
    ${CMAKE_CURRENT_LIST_DIR}/src/coap_min.c
)

# --- SD card configuration ---
set(HW_CONFIG_SRC
    ${CMAKE_CURRENT_LIST_DIR}/src/hw_config.c
)

# ===============================================================
# FatFS library
# ===============================================================
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../no-OS-FatFS-SD-SPI-RPi-Pico/FatFs_SPI build)

# ===============================================================
# Server build
# ===============================================================
add_executable(pico_coap_server
    ${CMAKE_CURRENT_LIST_DIR}/src/server_main.c
    ${COAP_SRC}
    ${HW_CONFIG_SRC}
)

target_include_directories(pico_coap_server
    PRIVATE
        ${PROJ_INC}
        ${PICO_LWIP_ARCH_INC}
)

target_link_libraries(pico_coap_server
    pico_stdlib
    pico_lwip
    pico_cyw43_arch_lwip_threadsafe_background
    FatFs_SPI
    hardware_adc
)

target_compile_definitions(pico_coap_server PRIVATE CYW43_DEFAULT_COUNTRY=CYW43_COUNTRY_SINGAPORE)

pico_enable_stdio_usb(pico_coap_server 1)
pico_enable_stdio_uart(pico_coap_server 0)

pico_add_extra_outputs(pico_coap_server)

# ===============================================================
# Client A0 build
# ===============================================================
add_executable(pico_coap_client_A0
    ${CMAKE_CURRENT_LIST_DIR}/src/client_main.c
    ${COAP_SRC}
    ${HW_CONFIG_SRC}
)

target_include_directories(pico_coap_client_A0
    PRIVATE
        ${PROJ_INC}
        ${PICO_LWIP_ARCH_INC}
)

target_link_libraries(pico_coap_client_A0
    pico_stdlib
    pico_lwip
    pico_cyw43_arch_lwip_threadsafe_background
    FatFs_SPI
    hardware_adc
)

target_compile_definitions(pico_coap_client_A0
    PRIVATE
        CLIENT_ID_STR="A0"  # This is the Client ID, can change to A0, A1, etc.
        CYW43_DEFAULT_COUNTRY=CYW43_COUNTRY_SINGAPORE
)

pico_enable_stdio_usb(pico_coap_client_A0 1)
pico_enable_stdio_uart(pico_coap_client_A0 0)

pico_add_extra_outputs(pico_coap_client_A0)
