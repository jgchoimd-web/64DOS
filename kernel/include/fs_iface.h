#ifndef FS_IFACE_H
#define FS_IFACE_H

#include <stdint.h>

typedef int bool;

typedef struct fs_stat {
    uint32_t size;
} fs_stat_t;

typedef struct fs_info {
    const char *driver_name;
    uint32_t bytes_per_sector;
    uint32_t root_entries;
    uint32_t root_lba;
    uint32_t data_lba;
} fs_info_t;

typedef struct fs_root_entry {
    char name[13];
    uint32_t size;
} fs_root_entry_t;

typedef struct fs_ops {
    bool (*list_root)(uint32_t *cursor, fs_root_entry_t *out_entry);
    bool (*stat)(const char *name, fs_stat_t *out_stat);
    bool (*read_file)(const char *name, uint8_t *buf, uint32_t max_len, uint32_t *out_len);
    void (*get_info)(fs_info_t *out_info);
} fs_ops_t;

typedef struct fs_driver {
    const char *name;
    bool (*probe)(const uint8_t *image, uint32_t image_size);
    bool (*mount)(const uint8_t *image, uint32_t image_size, const fs_ops_t **out_ops);
} fs_driver_t;

#endif
