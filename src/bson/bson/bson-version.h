/*
 * bson-version.h for micro-mongodb. Hand-written from the libbson 1.28.0
 * template — these values must match the vendored source tag.
 */

#if !defined(BSON_INSIDE) && !defined(BSON_COMPILATION)
#error "Only <bson/bson.h> can be included directly."
#endif

#ifndef BSON_VERSION_H
#define BSON_VERSION_H

#define BSON_MAJOR_VERSION       (1)
#define BSON_MINOR_VERSION       (28)
#define BSON_MICRO_VERSION       (0)
#define BSON_PRERELEASE_VERSION  ()

#define BSON_VERSION             (1.28.0)
#define BSON_VERSION_S           "1.28.0"

#define BSON_VERSION_HEX (BSON_MAJOR_VERSION << 24 | \
                          BSON_MINOR_VERSION << 16 | \
                          BSON_MICRO_VERSION << 8)

#define BSON_CHECK_VERSION(major, minor, micro) \
   (BSON_MAJOR_VERSION > (major) || \
    (BSON_MAJOR_VERSION == (major) && BSON_MINOR_VERSION > (minor)) || \
    (BSON_MAJOR_VERSION == (major) && BSON_MINOR_VERSION == (minor) && BSON_MICRO_VERSION >= (micro)))

#endif /* BSON_VERSION_H */
