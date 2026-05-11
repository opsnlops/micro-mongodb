add_library(umongoc STATIC
    mongo_transport.c
    mongo_wire.c
    mongo_crud.c
    mongo_cursor.c
    mongo_dns.c
    mongo_uri.c
    mongo_ca.c
    mongo_auth.c
    mongo_client.c
    mongo_wifi.c
    mongo_time.c
)

# NTP server for SNTP sync. Defaults to the NTP Pool; override with
# `cmake -DNTP_SERVER="time.cloudflare.com" ..` etc.
set(NTP_SERVER "pool.ntp.org" CACHE STRING "Hostname of the NTP server for SNTP time sync")
target_compile_definitions(umongoc PRIVATE NTP_SERVER="${NTP_SERVER}")

target_include_directories(umongoc PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(umongoc PUBLIC
    bson
    logging
    pico_cyw43_arch_lwip_sys_freertos
    pico_lwip_mbedtls
    pico_mbedtls
    pico_lwip_sntp
    FreeRTOS-Kernel
    freertos_config
)

# Apply the project's lwipopts.h. The app target sets this for itself; we need
# it here too because umongoc compiles lwIP-using code.
target_include_directories(umongoc PRIVATE
    ${CMAKE_SOURCE_DIR}/src/app    # lwipopts.h lives next to main.c for now
    ${CMAKE_SOURCE_DIR}            # FreeRTOSConfig.h
)
