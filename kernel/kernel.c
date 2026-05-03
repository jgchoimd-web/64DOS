#include "bootinfo.h"

#include <stddef.h>
#include <stdint.h>

#define VGA_ADDR 0xB8000u
#define VGA_COLS 80u
#define VGA_ROWS 25u
#define DEFAULT_VGA_COLOR 0x07u
#define DEFAULT_PROMPT "A:\\>"
#define MAX_SCRIPT_DEPTH 3u

#define COM1 0x3F8u

typedef int bool;
#define true 1
#define false 0

static const boot_info_t *g_boot;
static const uint8_t *g_image;
static uint32_t g_image_size;

static uint16_t *const vga = (uint16_t *)(uintptr_t)VGA_ADDR;
static uint32_t cursor_x;
static uint32_t cursor_y;
static uint8_t vga_color = DEFAULT_VGA_COLOR;
static char prompt_text[32] = DEFAULT_PROMPT;
static uint32_t script_depth;

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
static char line_buffer[128];
static uint8_t file_buffer[8192];
static uint8_t script_buffers[MAX_SCRIPT_DEPTH][8192];

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t str_len(const char *s) {
    uint32_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static char upper(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - ('a' - 'A'));
    }
    return c;
}

static int str_icmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = upper(*a++);
        char cb = upper(*b++);
        if (ca != cb) {
            return (int)(unsigned char)ca - (int)(unsigned char)cb;
        }
    }
    return (int)(unsigned char)upper(*a) - (int)(unsigned char)upper(*b);
}

static bool starts_icase(const char *s, const char *prefix) {
    while (*prefix) {
        if (upper(*s++) != upper(*prefix++)) {
            return false;
        }
    }
    return true;
}

static char *skip_spaces(char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static void trim_right(char *s) {
    uint32_t len = str_len(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = 0;
    }
}

static char *take_token(char **cursor) {
    char *token = skip_spaces(*cursor);
    char *end = token;
    while (*end && *end != ' ' && *end != '\t') {
        end++;
    }
    if (*end) {
        *end++ = 0;
    }
    *cursor = skip_spaces(end);
    return token;
}

static int hex_value(char c) {
    c = upper(c);
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool parse_uint(char *s, uint32_t *out) {
    uint32_t base = 10;
    uint32_t value = 0;
    const uint32_t max_u32 = 0xFFFFFFFFu;
    s = skip_spaces(s);
    if (!*s) {
        return false;
    }
    if (s[0] == '0' && upper(s[1]) == 'X') {
        base = 16;
        s += 2;
    }
    while (*s) {
        int digit = base == 16 ? hex_value(*s) : (*s >= '0' && *s <= '9' ? *s - '0' : -1);
        if (digit < 0 || (uint32_t)digit >= base) {
            return false;
        }
        if (value > (max_u32 - (uint32_t)digit) / base) {
            return false;
        }
        value = value * base + (uint32_t)digit;
        s++;
    }
    *out = value;
    return true;
}

static void copy_text(char *dst, uint32_t cap, const char *src) {
    uint32_t i = 0;
    if (cap == 0) {
        return;
    }
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static int serial_rx_ready(void) {
    return (inb(COM1 + 5) & 0x01) != 0;
}

static int serial_tx_ready(void) {
    return (inb(COM1 + 5) & 0x20) != 0;
}

static void serial_putc(char c) {
    while (!serial_tx_ready()) {
    }
    outb(COM1, (uint8_t)c);
}

static void vga_sync_cursor(void) {
    uint16_t pos = (uint16_t)(cursor_y * VGA_COLS + cursor_x);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)(pos >> 8));
}

static void vga_clear(void) {
    for (uint32_t y = 0; y < VGA_ROWS; y++) {
        for (uint32_t x = 0; x < VGA_COLS; x++) {
            vga[y * VGA_COLS + x] = (uint16_t)(((uint16_t)vga_color << 8) | ' ');
        }
    }
    cursor_x = 0;
    cursor_y = 0;
    vga_sync_cursor();
}

static void vga_scroll(void) {
    if (cursor_y < VGA_ROWS) {
        return;
    }
    for (uint32_t y = 1; y < VGA_ROWS; y++) {
        for (uint32_t x = 0; x < VGA_COLS; x++) {
            vga[(y - 1) * VGA_COLS + x] = vga[y * VGA_COLS + x];
        }
    }
    for (uint32_t x = 0; x < VGA_COLS; x++) {
        vga[(VGA_ROWS - 1) * VGA_COLS + x] = (uint16_t)(((uint16_t)vga_color << 8) | ' ');
    }
    cursor_y = VGA_ROWS - 1;
}

static void console_putc(char c) {
    if (c == '\n') {
        serial_putc('\r');
        serial_putc('\n');
        cursor_x = 0;
        cursor_y++;
        vga_scroll();
        vga_sync_cursor();
        return;
    }

    if (c == '\r') {
        return;
    }

    if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            vga[cursor_y * VGA_COLS + cursor_x] = (uint16_t)(((uint16_t)vga_color << 8) | ' ');
            serial_putc('\b');
            serial_putc(' ');
            serial_putc('\b');
        }
        vga_sync_cursor();
        return;
    }

    serial_putc(c);
    vga[cursor_y * VGA_COLS + cursor_x] = (uint16_t)(((uint16_t)vga_color << 8) | (uint8_t)c);
    cursor_x++;
    if (cursor_x >= VGA_COLS) {
        cursor_x = 0;
        cursor_y++;
        vga_scroll();
    }
    vga_sync_cursor();
}

static void print(const char *s) {
    while (*s) {
        console_putc(*s++);
    }
}

static void print_hex32(uint32_t value) {
    static const char hex[] = "0123456789ABCDEF";
    print("0x");
    for (int i = 7; i >= 0; i--) {
        console_putc(hex[(value >> (i * 4)) & 0xFu]);
    }
}

static void print_hex8(uint8_t value) {
    static const char hex[] = "0123456789ABCDEF";
    console_putc(hex[(value >> 4) & 0xFu]);
    console_putc(hex[value & 0xFu]);
}

static void print_dec(uint32_t value) {
    char buf[11];
    uint32_t i = 0;
    if (value == 0) {
        console_putc('0');
        return;
    }
    while (value && i < sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i) {
        console_putc(buf[--i]);
    }
}

static const char keymap[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8',
    '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r',
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n',
    'm', ',', '.', '/', 0, '*', 0, ' '
};

static const char keymap_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*',
    '(', ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R',
    'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"', '~', 0, '|', 'Z', 'X', 'C', 'V', 'B', 'N',
    'M', '<', '>', '?', 0, '*', 0, ' '
};

static char read_char(void) {
    static int shift;

    for (;;) {
        if (serial_rx_ready()) {
            char c = (char)inb(COM1);
            if (c == '\r') {
                return '\n';
            }
            return c;
        }

        if ((inb(0x64) & 0x01) == 0) {
            continue;
        }

        uint8_t sc = inb(0x60);
        if (sc == 0x2A || sc == 0x36) {
            shift = 1;
            continue;
        }
        if (sc == 0xAA || sc == 0xB6) {
            shift = 0;
            continue;
        }
        if (sc & 0x80) {
            continue;
        }
        if (sc < 128) {
            char c = shift ? keymap_shift[sc] : keymap[sc];
            if (c) {
                return c;
            }
        }
    }
}

static void read_line(char *buf, uint32_t cap) {
    uint32_t n = 0;
    for (;;) {
        char c = read_char();
        if (c == '\n') {
            console_putc('\n');
            buf[n] = 0;
            return;
        }
        if (c == '\b' || c == 127) {
            if (n > 0) {
                n--;
                console_putc('\b');
            }
            continue;
        }
        if ((uint8_t)c >= 32 && n + 1 < cap) {
            buf[n++] = c;
            console_putc(c);
        }
    }
}

static bool fs_init(const uint8_t *image, uint32_t image_size) {
    if (image_size < 512 || image[510] != 0x55 || image[511] != 0xAA) {
        return false;
    }
    fs.image = image;
    fs.image_size = image_size;
    fs.bytes_per_sector = rd16(image + 11);
    fs.sectors_per_cluster = image[13];
    fs.reserved_sectors = rd16(image + 14);
    fs.fat_count = image[16];
    fs.root_entries = rd16(image + 17);
    fs.sectors_per_fat = rd16(image + 22);

    if (fs.bytes_per_sector != 512 || fs.sectors_per_cluster == 0 ||
        fs.fat_count == 0 || fs.root_entries == 0 || fs.sectors_per_fat == 0) {
        return false;
    }

    fs.root_lba = fs.reserved_sectors + (uint32_t)fs.fat_count * fs.sectors_per_fat;
    fs.root_sectors = ((uint32_t)fs.root_entries * 32u + fs.bytes_per_sector - 1u) /
                      fs.bytes_per_sector;
    fs.data_lba = fs.root_lba + fs.root_sectors;
    return fs.data_lba * 512u < image_size;
}

static bool make_83(const char *name, char out[11]) {
    for (uint32_t i = 0; i < 11; i++) {
        out[i] = ' ';
    }

    if (name[0] == 'A' && name[1] == ':' && (name[2] == '\\' || name[2] == '/')) {
        name += 3;
    }
    while (*name == '\\' || *name == '/') {
        name++;
    }

    uint32_t i = 0;
    while (*name && *name != '.' && *name != ' ' && *name != '\t') {
        if (i >= 8) {
            return false;
        }
        out[i++] = upper(*name++);
    }

    if (*name == '.') {
        name++;
        i = 8;
        while (*name && *name != ' ' && *name != '\t') {
            if (i >= 11) {
                return false;
            }
            out[i++] = upper(*name++);
        }
    }

    return out[0] != ' ';
}

static const uint8_t *root_entry(uint32_t index) {
    return fs.image + (fs.root_lba * 512u) + index * 32u;
}

static uint16_t fat_next(uint16_t cluster) {
    const uint8_t *fat = fs.image + fs.reserved_sectors * 512u;
    uint32_t offset = cluster + (cluster / 2u);
    uint16_t value = (uint16_t)fat[offset] | ((uint16_t)fat[offset + 1] << 8);
    if (cluster & 1u) {
        value >>= 4;
    } else {
        value &= 0x0FFFu;
    }
    return value;
}

static const uint8_t *find_root_file(const char *name) {
    char dos_name[11];
    if (!make_83(name, dos_name)) {
        return NULL;
    }

    for (uint32_t i = 0; i < fs.root_entries; i++) {
        const uint8_t *e = root_entry(i);
        if (e[0] == 0x00) {
            return NULL;
        }
        if (e[0] == 0xE5 || (e[11] & 0x08) || (e[11] & 0x10)) {
            continue;
        }
        bool same = true;
        for (uint32_t j = 0; j < 11; j++) {
            if ((char)e[j] != dos_name[j]) {
                same = false;
                break;
            }
        }
        if (same) {
            return e;
        }
    }
    return NULL;
}

static bool fs_read_file(const char *name, uint8_t *buf, uint32_t max_len, uint32_t *out_len) {
    const uint8_t *e = find_root_file(name);
    if (!e) {
        return false;
    }

    uint32_t size = rd32(e + 28);
    uint32_t copied = 0;
    uint16_t cluster = rd16(e + 26);

    while (cluster >= 2 && cluster < 0xFF8 && copied < size && copied < max_len) {
        uint32_t lba = fs.data_lba + ((uint32_t)cluster - 2u) * fs.sectors_per_cluster;
        uint32_t offset = lba * 512u;
        uint32_t chunk = (uint32_t)fs.sectors_per_cluster * 512u;
        if (offset + chunk > fs.image_size) {
            break;
        }
        if (chunk > size - copied) {
            chunk = size - copied;
        }
        if (chunk > max_len - copied) {
            chunk = max_len - copied;
        }
        for (uint32_t i = 0; i < chunk; i++) {
            buf[copied + i] = fs.image[offset + i];
        }
        copied += chunk;
        cluster = fat_next(cluster);
    }

    *out_len = copied;
    return copied == size || copied == max_len;
}

static void print_root_name(const uint8_t *e) {
    for (uint32_t i = 0; i < 8 && e[i] != ' '; i++) {
        console_putc((char)e[i]);
    }
    if (e[8] != ' ') {
        console_putc('.');
        for (uint32_t i = 8; i < 11 && e[i] != ' '; i++) {
            console_putc((char)e[i]);
        }
    }
}

static void cmd_dir(void) {
    uint32_t files = 0;
    uint32_t bytes = 0;
    print(" Directory of A:\\\n\n");
    for (uint32_t i = 0; i < fs.root_entries; i++) {
        const uint8_t *e = root_entry(i);
        if (e[0] == 0x00) {
            break;
        }
        if (e[0] == 0xE5 || (e[11] & 0x08) || (e[11] & 0x10)) {
            continue;
        }
        print_root_name(e);
        uint32_t name_len = 0;
        for (uint32_t j = 0; j < 8 && e[j] != ' '; j++) {
            name_len++;
        }
        if (e[8] != ' ') {
            name_len++;
            for (uint32_t j = 8; j < 11 && e[j] != ' '; j++) {
                name_len++;
            }
        }
        while (name_len++ < 14) {
            console_putc(' ');
        }
        uint32_t size = rd32(e + 28);
        print_dec(size);
        print(" bytes\n");
        files++;
        bytes += size;
    }
    print("\n");
    print_dec(files);
    print(" file(s)  ");
    print_dec(bytes);
    print(" bytes\n");
}

static uint32_t fs_file_size(const char *name) {
    const uint8_t *e = find_root_file(name);
    if (!e) {
        return 0xFFFFFFFFu;
    }
    return rd32(e + 28);
}

static void cmd_type(const char *name) {
    uint32_t len = 0;
    if (!fs_read_file(name, file_buffer, sizeof(file_buffer) - 1u, &len)) {
        print("File not found or too large\n");
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        console_putc((char)file_buffer[i]);
    }
    if (len == 0 || file_buffer[len - 1] != '\n') {
        console_putc('\n');
    }
}

static void cmd_dump(char *args) {
    char *name = take_token(&args);
    uint32_t count = 128;
    uint32_t file_size;
    uint32_t len = 0;

    if (!*name) {
        print("Usage: DUMP filename [bytes]\n");
        return;
    }
    if (*args && !parse_uint(args, &count)) {
        print("Bad byte count\n");
        return;
    }
    if (count > sizeof(file_buffer)) {
        count = sizeof(file_buffer);
    }

    file_size = fs_file_size(name);
    if (file_size == 0xFFFFFFFFu || !fs_read_file(name, file_buffer, count, &len)) {
        print("File not found\n");
        return;
    }

    print("Hex dump of ");
    print(name);
    print(" (");
    print_dec(file_size);
    print(" bytes, showing ");
    print_dec(len);
    print(")\n");

    for (uint32_t off = 0; off < len; off += 16) {
        print_hex32(off);
        print("  ");
        for (uint32_t i = 0; i < 16; i++) {
            if (off + i < len) {
                print_hex8(file_buffer[off + i]);
            } else {
                print("  ");
            }
            console_putc(' ');
        }
        console_putc(' ');
        for (uint32_t i = 0; i < 16 && off + i < len; i++) {
            uint8_t c = file_buffer[off + i];
            console_putc(c >= 32 && c < 127 ? (char)c : '.');
        }
        console_putc('\n');
    }
}


static void cmd_wc(char *args) {
    char *name = take_token(&args);
    uint32_t file_size;
    uint32_t len = 0;
    uint32_t lines = 0;
    uint32_t words = 0;
    bool in_word = false;

    if (!*name) {
        print("Usage: WC filename\n");
        return;
    }

    file_size = fs_file_size(name);
    if (file_size == 0xFFFFFFFFu || !fs_read_file(name, file_buffer, sizeof(file_buffer), &len)) {
        print("File not found or too large\n");
        return;
    }

    for (uint32_t i = 0; i < len; i++) {
        char c = (char)file_buffer[i];
        if (c == '\n') {
            lines++;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            in_word = false;
        } else if (!in_word) {
            words++;
            in_word = true;
        }
    }

    print(name);
    print(": ");
    print_dec(lines);
    print(" line(s), ");
    print_dec(words);
    print(" word(s), ");
    print_dec(file_size);
    print(" byte(s)\n");
}

static uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static uint8_t bcd_to_bin(uint8_t value) {
    return (uint8_t)((value & 0x0Fu) + ((value >> 4) * 10u));
}

typedef struct rtc_time {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
} rtc_time_t;

static void rtc_read(rtc_time_t *rtc) {
    uint8_t status_b;
    while (cmos_read(0x0A) & 0x80u) {
    }
    rtc->second = cmos_read(0x00);
    rtc->minute = cmos_read(0x02);
    rtc->hour = cmos_read(0x04);
    rtc->day = cmos_read(0x07);
    rtc->month = cmos_read(0x08);
    rtc->year = cmos_read(0x09);
    status_b = cmos_read(0x0B);

    if ((status_b & 0x04u) == 0) {
        rtc->second = bcd_to_bin(rtc->second);
        rtc->minute = bcd_to_bin(rtc->minute);
        rtc->hour = (uint8_t)((rtc->hour & 0x80u) | bcd_to_bin(rtc->hour & 0x7Fu));
        rtc->day = bcd_to_bin(rtc->day);
        rtc->month = bcd_to_bin(rtc->month);
        rtc->year = bcd_to_bin(rtc->year);
    }

    if ((status_b & 0x02u) == 0 && (rtc->hour & 0x80u)) {
        rtc->hour = (uint8_t)(((rtc->hour & 0x7Fu) + 12u) % 24u);
    }
}

static void print_2(uint32_t value) {
    console_putc((char)('0' + (value / 10u) % 10u));
    console_putc((char)('0' + value % 10u));
}

static void cmd_date(void) {
    rtc_time_t rtc;
    rtc_read(&rtc);
    print("Current date: 20");
    print_2(rtc.year);
    console_putc('-');
    print_2(rtc.month);
    console_putc('-');
    print_2(rtc.day);
    console_putc('\n');
}

static void cmd_time(void) {
    rtc_time_t rtc;
    rtc_read(&rtc);
    print("Current time: ");
    print_2(rtc.hour);
    console_putc(':');
    print_2(rtc.minute);
    console_putc(':');
    print_2(rtc.second);
    print("\n");
}

static void cmd_color(char *args) {
    int bg;
    int fg;
    args = skip_spaces(args);
    if (!*args) {
        print("Current color: ");
        print_hex8(vga_color);
        print("\nUsage: COLOR bgfg, example COLOR 1E\n");
        return;
    }
    bg = hex_value(args[0]);
    fg = hex_value(args[1]);
    if (bg < 0 || fg < 0 || args[2]) {
        print("Usage: COLOR bgfg, example COLOR 1E\n");
        return;
    }
    vga_color = (uint8_t)((bg << 4) | fg);
    vga_clear();
    print("Color set to ");
    print_hex8(vga_color);
    print("\n");
}

static void cmd_prompt(char *args) {
    args = skip_spaces(args);
    if (!*args) {
        copy_text(prompt_text, sizeof(prompt_text), DEFAULT_PROMPT);
    } else {
        copy_text(prompt_text, sizeof(prompt_text), args);
    }
    print("Prompt is ");
    print(prompt_text);
    print("\n");
}

static void cmd_help(char *topic) {
    topic = skip_spaces(topic);
    if (!*topic) {
        print("Commands: VER HELP DIR/LS TYPE/CAT DUMP/HEX WC RUN CLS MEM/INFO\n");
        print("          DATE TIME COLOR PROMPT PWD ECHO EXIT REBOOT\n");
        return;
    }
    if (str_icmp(topic, "DUMP") == 0 || str_icmp(topic, "HEX") == 0) {
        print("DUMP filename [bytes] - show file bytes in hex and ASCII\n");
    } else if (str_icmp(topic, "WC") == 0) {
        print("WC filename - count lines, words, and bytes in a file\n");
    } else if (str_icmp(topic, "RUN") == 0) {
        print("RUN filename - run a root-directory batch file, one command per line\n");
    } else if (str_icmp(topic, "COLOR") == 0) {
        print("COLOR bgfg - set VGA text color with DOS hex digits, e.g. COLOR 1E\n");
    } else if (str_icmp(topic, "PROMPT") == 0) {
        print("PROMPT [text] - set the command prompt, or reset it without text\n");
    } else {
        print("No detailed help for that command\n");
    }
}

static void cmd_ver(void) {
    print("64DOS v0.1 BIOS x86-64, public domain\n");
}

static void cmd_mem(void) {
    print("Image ");
    print_hex32(g_boot->image_base);
    print(" + ");
    print_dec(g_boot->image_size);
    print(" bytes\nKernel ");
    print_hex32(g_boot->kernel_load);
    print(" + ");
    print_dec(g_boot->kernel_size);
    print(" bytes\nBoot drive ");
    print_hex32(g_boot->boot_drive);
    print("\n");
}

static void cmd_info(void) {
    cmd_mem();
    print("FAT12 root LBA ");
    print_dec(fs.root_lba);
    print(", data LBA ");
    print_dec(fs.data_lba);
    print(", root entries ");
    print_dec(fs.root_entries);
    print("\n");
}

static void print_prompt(void) {
    print(prompt_text);
    console_putc(' ');
}

static void reboot(void) {
    print("Rebooting...\n");
    for (uint32_t i = 0; i < 100000; i++) {
        __asm__ volatile("pause");
    }
    outb(0x64, 0xFE);
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static void execute_command(char *line);

static void run_script_file(const char *name) {
    uint32_t len = 0;
    char cmd[128];
    uint32_t pos = 0;
    uint8_t *script;

    if (script_depth >= MAX_SCRIPT_DEPTH) {
        print("Batch nesting too deep\n");
        return;
    }
    script = script_buffers[script_depth];
    if (!fs_read_file(name, script, sizeof(script_buffers[0]) - 1u, &len)) {
        print("Batch file not found or too large\n");
        return;
    }
    script[len] = 0;
    script_depth++;

    for (uint32_t i = 0; i <= len; i++) {
        char c = (char)script[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n' || c == 0) {
            cmd[pos] = 0;
            trim_right(cmd);
            char *trimmed = skip_spaces(cmd);
            if (*trimmed && upper(trimmed[0]) != ':' && !starts_icase(trimmed, "REM")) {
                print_prompt();
                print(trimmed);
                print("\n");
                execute_command(trimmed);
            }
            pos = 0;
        } else if (pos + 1 < sizeof(cmd)) {
            cmd[pos++] = c;
        }
    }

    script_depth--;
}

static void execute_command(char *line) {
    char *cmd = skip_spaces(line);
    char *args;
    trim_right(cmd);
    if (!*cmd) {
        return;
    }
    args = cmd;
    cmd = take_token(&args);

    if (str_icmp(cmd, "VER") == 0) {
        cmd_ver();
    } else if (str_icmp(cmd, "HELP") == 0) {
        cmd_help(args);
    } else if (str_icmp(cmd, "DIR") == 0 || str_icmp(cmd, "LS") == 0) {
        cmd_dir();
    } else if (str_icmp(cmd, "PWD") == 0 || str_icmp(cmd, "CD") == 0) {
        print("A:\\\n");
    } else if (str_icmp(cmd, "CLS") == 0) {
        vga_clear();
    } else if (str_icmp(cmd, "MEM") == 0) {
        cmd_mem();
    } else if (str_icmp(cmd, "INFO") == 0) {
        cmd_info();
    } else if (str_icmp(cmd, "DATE") == 0) {
        cmd_date();
    } else if (str_icmp(cmd, "TIME") == 0) {
        cmd_time();
    } else if (str_icmp(cmd, "COLOR") == 0) {
        cmd_color(args);
    } else if (str_icmp(cmd, "PROMPT") == 0) {
        cmd_prompt(args);
    } else if (str_icmp(cmd, "EXIT") == 0 || str_icmp(cmd, "REBOOT") == 0) {
        reboot();
    } else if (str_icmp(cmd, "ECHO") == 0) {
        print(args);
        console_putc('\n');
    } else if (str_icmp(cmd, "TYPE") == 0 || str_icmp(cmd, "CAT") == 0) {
        if (*args) {
            cmd_type(args);
        } else {
            print("Usage: TYPE filename\n");
        }
    } else if (str_icmp(cmd, "DUMP") == 0 || str_icmp(cmd, "HEX") == 0) {
        cmd_dump(args);
    } else if (str_icmp(cmd, "WC") == 0) {
        cmd_wc(args);
    } else if (str_icmp(cmd, "RUN") == 0) {
        if (*args) {
            run_script_file(args);
        } else {
            print("Usage: RUN filename\n");
        }
    } else {
        print("Bad command or file name\n");
    }
}

static void run_autoexec(void) {
    run_script_file("AUTOEXEC.BAT");
}

void kmain(const boot_info_t *boot) {
    serial_init();
    vga_clear();

    g_boot = boot;
    print("64DOS BIOS x86-64\n");

    if (!boot || boot->magic != DOS64_BOOTINFO_MAGIC ||
        boot->version != DOS64_BOOTINFO_VERSION) {
        print("Invalid boot info\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    g_image = (const uint8_t *)(uintptr_t)boot->image_base;
    g_image_size = boot->image_size;
    if (!fs_init(g_image, g_image_size)) {
        print("FAT12 image mount failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    print("A: mounted read-only from RAM image\n");
    run_autoexec();

    for (;;) {
        print_prompt();
        read_line(line_buffer, sizeof(line_buffer));
        execute_command(line_buffer);
    }
}
