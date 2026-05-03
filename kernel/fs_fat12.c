#include "include/fs_fat12.h"

#include <stddef.h>
#include <stdint.h>

#define true 1
#define false 0

typedef struct fat12 {
    const uint8_t *image;
    uint32_t image_size;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entries;
    uint16_t sectors_per_fat;
    uint32_t root_lba;
    uint32_t root_sectors;
    uint32_t data_lba;
} fat12_t;

static fat12_t fs;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c; }

static bool make_83(const char *name, char out[11]) {
    for (uint32_t i = 0; i < 11; i++) out[i] = ' ';
    if (name[0] == 'A' && name[1] == ':' && (name[2] == '\\' || name[2] == '/')) name += 3;
    while (*name == '\\' || *name == '/') name++;
    uint32_t i = 0;
    while (*name && *name != '.' && *name != ' ' && *name != '\t') { if (i >= 8) return false; out[i++] = upper(*name++); }
    if (*name == '.') { name++; i = 8; while (*name && *name != ' ' && *name != '\t') { if (i >= 11) return false; out[i++] = upper(*name++); } }
    return out[0] != ' ';
}

static const uint8_t *root_entry(uint32_t index) { return fs.image + (fs.root_lba * 512u) + index * 32u; }
static uint16_t fat_next(uint16_t cluster) {
    const uint8_t *fat = fs.image + fs.reserved_sectors * 512u;
    uint32_t offset = cluster + (cluster / 2u);
    uint16_t value = (uint16_t)fat[offset] | ((uint16_t)fat[offset + 1] << 8);
    return (cluster & 1u) ? (value >> 4) : (value & 0x0FFFu);
}

static const uint8_t *find_root_file(const char *name) {
    char dos_name[11];
    if (!make_83(name, dos_name)) return NULL;
    for (uint32_t i = 0; i < fs.root_entries; i++) {
        const uint8_t *e = root_entry(i);
        if (e[0] == 0x00) return NULL;
        if (e[0] == 0xE5 || (e[11] & 0x08) || (e[11] & 0x10)) continue;
        bool same = true;
        for (uint32_t j = 0; j < 11; j++) if ((char)e[j] != dos_name[j]) { same = false; break; }
        if (same) return e;
    }
    return NULL;
}

static void entry_name(const uint8_t *e, char out[13]) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < 8 && e[i] != ' '; i++) out[n++] = (char)e[i];
    if (e[8] != ' ') {
        out[n++] = '.';
        for (uint32_t i = 8; i < 11 && e[i] != ' '; i++) out[n++] = (char)e[i];
    }
    out[n] = 0;
}

static bool fat12_probe(const uint8_t *image, uint32_t image_size) {
    return image_size >= 512 && image[510] == 0x55 && image[511] == 0xAA && rd16(image + 11) == 512;
}

static bool fat12_mount(const uint8_t *image, uint32_t image_size, const fs_ops_t **out_ops);
static bool fat12_list_root(uint32_t *cursor, fs_root_entry_t *out_entry);
static bool fat12_stat(const char *name, fs_stat_t *out_stat);
static bool fat12_read_file(const char *name, uint8_t *buf, uint32_t max_len, uint32_t *out_len);
static void fat12_get_info(fs_info_t *out_info);

static const fs_ops_t fat12_ops = { fat12_list_root, fat12_stat, fat12_read_file, fat12_get_info };
static const fs_driver_t fat12_driver = { "FAT12", fat12_probe, fat12_mount };

const fs_driver_t *fs_fat12_driver(void) { return &fat12_driver; }

static bool fat12_mount(const uint8_t *image, uint32_t image_size, const fs_ops_t **out_ops) {
    if (!fat12_probe(image, image_size)) return false;
    fs.image = image; fs.image_size = image_size;
    fs.bytes_per_sector = rd16(image + 11); fs.sectors_per_cluster = image[13];
    fs.reserved_sectors = rd16(image + 14); fs.fat_count = image[16];
    fs.root_entries = rd16(image + 17); fs.sectors_per_fat = rd16(image + 22);
    if (fs.bytes_per_sector != 512 || fs.sectors_per_cluster == 0 || fs.fat_count == 0 || fs.root_entries == 0 || fs.sectors_per_fat == 0) return false;
    fs.root_lba = fs.reserved_sectors + (uint32_t)fs.fat_count * fs.sectors_per_fat;
    fs.root_sectors = ((uint32_t)fs.root_entries * 32u + fs.bytes_per_sector - 1u) / fs.bytes_per_sector;
    fs.data_lba = fs.root_lba + fs.root_sectors;
    if (fs.data_lba * 512u >= image_size) return false;
    *out_ops = &fat12_ops;
    return true;
}

static bool fat12_list_root(uint32_t *cursor, fs_root_entry_t *out_entry) {
    while (*cursor < fs.root_entries) {
        const uint8_t *e = root_entry((*cursor)++);
        if (e[0] == 0x00) return false;
        if (e[0] == 0xE5 || (e[11] & 0x08) || (e[11] & 0x10)) continue;
        entry_name(e, out_entry->name);
        out_entry->size = rd32(e + 28);
        return true;
    }
    return false;
}

static bool fat12_stat(const char *name, fs_stat_t *out_stat) {
    const uint8_t *e = find_root_file(name);
    if (!e) return false;
    out_stat->size = rd32(e + 28);
    return true;
}

static bool fat12_read_file(const char *name, uint8_t *buf, uint32_t max_len, uint32_t *out_len) {
    const uint8_t *e = find_root_file(name);
    if (!e) return false;
    uint32_t size = rd32(e + 28), copied = 0;
    uint16_t cluster = rd16(e + 26);
    while (cluster >= 2 && cluster < 0xFF8 && copied < size && copied < max_len) {
        uint32_t lba = fs.data_lba + ((uint32_t)cluster - 2u) * fs.sectors_per_cluster;
        uint32_t offset = lba * 512u, chunk = (uint32_t)fs.sectors_per_cluster * 512u;
        if (offset + chunk > fs.image_size) break;
        if (chunk > size - copied) chunk = size - copied;
        if (chunk > max_len - copied) chunk = max_len - copied;
        for (uint32_t i = 0; i < chunk; i++) buf[copied + i] = fs.image[offset + i];
        copied += chunk; cluster = fat_next(cluster);
    }
    *out_len = copied;
    return copied == size || copied == max_len;
}

static void fat12_get_info(fs_info_t *out_info) {
    out_info->driver_name = "FAT12";
    out_info->bytes_per_sector = fs.bytes_per_sector;
    out_info->root_entries = fs.root_entries;
    out_info->root_lba = fs.root_lba;
    out_info->data_lba = fs.data_lba;
}
