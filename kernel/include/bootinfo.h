#ifndef DOS64_BOOTINFO_H
#define DOS64_BOOTINFO_H

#include <stdint.h>

#define DOS64_BOOTINFO_MAGIC 0x42443436u
#define DOS64_BOOTINFO_VERSION 1u

typedef struct boot_info {
    uint32_t magic;
    uint32_t version;
    uint32_t image_base;
    uint32_t image_size;
    uint32_t kernel_load;
    uint32_t kernel_size;
    uint32_t flags;
    uint32_t boot_drive;
    uint32_t manifest_addr;
} boot_info_t;

#endif

