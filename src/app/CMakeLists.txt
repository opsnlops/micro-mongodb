add_executable(micro_mongodb_app
    main.c
    freertos_hooks.c
    logger_hook.c
)

target_include_directories(micro_mongodb_app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/..
    ${CMAKE_SOURCE_DIR}            # for FreeRTOSConfig.h at repo root
)

# WiFi credentials and mongo URI come in via CMake cache. Keep them out of git.
set(WIFI_SSID     "" CACHE STRING "WiFi SSID")
set(WIFI_PASSWORD "" CACHE STRING "WiFi password")
set(MONGO_URI     "mongodb://192.168.1.1:27017" CACHE STRING "MongoDB connection string")

target_compile_definitions(micro_mongodb_app PRIVATE
    WIFI_SSID="${WIFI_SSID}"
    WIFI_PASSWORD="${WIFI_PASSWORD}"
    MONGO_URI="${MONGO_URI}"
    # FreeRTOS / pico SMP debugging conveniences
    configUSE_TRACE_FACILITY=1
    configRECORD_STACK_HIGH_ADDRESS=1
)

target_link_libraries(micro_mongodb_app
    pico_stdlib
    pico_rand
    pico_unique_id
    pico_cyw43_arch_lwip_sys_freertos       # WiFi + lwIP + FreeRTOS integration
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
    logging                                  # queue-based async logger
    umongoc                                  # pulls in bson transitively
)

# stdio on USB-CDC for easy serial debugging from a laptop
pico_enable_stdio_usb(micro_mongodb_app 1)
pico_enable_stdio_uart(micro_mongodb_app 0)

pico_add_extra_outputs(micro_mongodb_app)
