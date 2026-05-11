add_library(logging STATIC
    logging.c
)

target_include_directories(logging PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(logging PUBLIC
    pico_stdlib
    FreeRTOS-Kernel
    freertos_config
)
