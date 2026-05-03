#ifndef DOS64_EXECUTABLE_H
#define DOS64_EXECUTABLE_H

#include <stdint.h>

#define EXECUTABLE_MAGIC 0x58343644u /* "D64X" little-endian */
#define EXECUTABLE_VERSION 1u

typedef struct executable_header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t entry_offset;
    uint32_t code_size;
    uint32_t checksum; /* optional, 0 disables checksum verification */
} executable_header_t;

#endif
