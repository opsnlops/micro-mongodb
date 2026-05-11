# Vendored libbson 1.28.0 + minimal `common/` shims, patched for arm-none-eabi
# + newlib-nano + FreeRTOS. See src/bson/README.md for the porting decisions.

add_library(bson STATIC
    # libbson core
    bson/bson.c
    bson/bson-iter.c
    bson/bson-writer.c
    bson/bson-oid.c
    bson/bson-utf8.c
    bson/bson-string.c
    bson/bson-value.c
    bson/bson-context.c
    bson/bson-memory.c
    bson/bson-clock.c
    bson/bson-keys.c
    bson/bson-atomic.c
    bson/bson-decimal128.c
    bson/bson-error.c

    # bson_as_json* in bson.c calls _bson_iso8601_date_format, so iso8601 +
    # its timegm dependency must come along. jsonsl + bson-json.c (JSON ->
    # BSON parsing) and bson-reader.c (file-I/O streaming) are NOT compiled
    # -- callers on the Pico don't need JSON parsing or file I/O, and skipping
    # them keeps the binary smaller while avoiding the jsonsl vendor.
    bson/bson-iso8601.c
    bson/bson-timegm.c

    # BCON convenience macros + version functions. Cheap.
    bson/bcon.c
    bson/bson-version-functions.c

    # base64 from the shared common/ tree. We'd want this for SCRAM auth
    # in umongoc/ anyway, but it's libbson's dependency too (used by EJSON).
    common/common-b64.c

    # Port shims (sched_yield -> taskYIELD, etc.)
    port_pico.c
)

# port_pico.c pulls in FreeRTOS headers; freertos_config carries the include
# path for FreeRTOSConfig.h, FreeRTOS-Kernel provides the rest.
target_link_libraries(bson PRIVATE freertos_config FreeRTOS-Kernel)

target_include_directories(bson PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}            # so `<bson/bson.h>` resolves
)
target_include_directories(bson PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/common     # for `"common-*-private.h"` quote-includes
)

target_compile_definitions(bson PRIVATE
    BSON_COMPILATION
    MCOMMON_NAME_PREFIX=mcommon
)

# Silence the more pedantic warnings from upstream code so the embedded build
# doesn't get spammed. The vendored code is third-party; we accept its style.
target_compile_options(bson PRIVATE
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-sign-compare
    -Wno-implicit-fallthrough
    -Wno-strict-aliasing
)

