#include "bootinfo.h"
#include "executable.h"
#include "fs_fat12.h"
#include "fs_iface.h"

#include <stddef.h>
#include <stdint.h>

#define VGA_ADDR 0xB8000u
#define VGA_COLS 80u
#define VGA_ROWS 25u
#define DEFAULT_VGA_COLOR 0x07u
#define DEFAULT_PROMPT "RFS:\\>"
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

typedef struct script_var {
    char name[16];
    int32_t value;
} script_var_t;

static script_var_t runtime_vars[16];
static uint32_t runtime_var_count;

static const fs_ops_t *g_fs;
static fs_info_t g_fs_info;

static char line_buffer[128];
static uint8_t file_buffer[8192];
static uint8_t script_buffers[MAX_SCRIPT_DEPTH][8192];

#define MAX_BINARY_FILE_SIZE 65536u
#define EXEC_IMAGE_ALIGN 4u

typedef void (*exec_entry_fn_t)(const executable_header_t *header, const uint8_t *image);

static void execute_command(char *line);

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

static bool is_rem_comment(const char *line) {
    if (!starts_icase(line, "REM")) {
        return false;
    }
    return line[3] == 0 || line[3] == ' ' || line[3] == '	';
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


static const char rfs_readme_txt[] =
    "64DOS RFS (64-bit git-style)\n"
    "- read-only object store\n"
    "- use paths like RFS:\\README.TXT\n";

static const uint8_t rfs_demo_rxe[] = {
    '6','4','E','X',
    'E','C','H','O',' ','H','e','l','l','o',' ','f','r','o','m',' ','R','X','E','\n',
    'D','I','R','\n'
};

typedef struct rfs_file {
    const char *name;
    const uint8_t *data;
    uint32_t len;
} rfs_file_t;

static const rfs_file_t rfs_files[] = {
    { "README.TXT", (const uint8_t *)rfs_readme_txt, (uint32_t)(sizeof(rfs_readme_txt) - 1u) },
    { "DEMO.RXE", rfs_demo_rxe, (uint32_t)sizeof(rfs_demo_rxe) },
};


typedef struct rfs_blob {
    uint64_t oid;
    const uint8_t *data;
    uint32_t len;
} rfs_blob_t;

typedef struct rfs_ref {
    const char *name;
    uint64_t commit_oid;
} rfs_ref_t;

typedef struct pkg_entry {
    const char *name;
    const char *version;
    const char *desc;
    bool installed;
} pkg_entry_t;

typedef struct vcs_commit {
    uint64_t id;
    const char *msg;
} vcs_commit_t;

static const rfs_blob_t rfs_blobs[] = {
    { 0x1000000000000001ull, (const uint8_t *)rfs_readme_txt, (uint32_t)(sizeof(rfs_readme_txt) - 1u) },
    { 0x1000000000000002ull, rfs_demo_rxe, (uint32_t)sizeof(rfs_demo_rxe) },
};

static const rfs_ref_t rfs_refs[] = {
    { "HEAD", 0x3000000000000001ull },
    { "refs/heads/main", 0x3000000000000001ull },
};

static pkg_entry_t pkg_db[] = {
    { "BASE", "1.0.0", "core shell package", true },
    { "NET", "0.1.0", "network utilities pack", false },
    { "DEV", "0.1.0", "developer tools pack", false },
};

static vcs_commit_t vcs_log[8] = {
    { 0x3000000000000001ull, "init: bootstrap RFS repository" },
};
static char vcs_msgs[8][64];
static uint32_t vcs_log_count = 1;
static const char *vcs_branch = "main";
static bool vcs_dirty;

static const char *strip_rfs_prefix(const char *name) {
    if (upper(name[0]) == 'R' && upper(name[1]) == 'F' && upper(name[2]) == 'S' &&
        name[3] == ':' && (name[4] == '\\' || name[4] == '/')) {
        return name + 5;
    }
    if (upper(name[0]) == 'R' && upper(name[1]) == 'F' && upper(name[2]) == 'S' &&
        (name[3] == '\\' || name[3] == '/')) {
        return name + 4;
    }
    return NULL;
}

static const rfs_file_t *rfs_find_file(const char *name) {
    const char *base = strip_rfs_prefix(name);
    if (!base) return NULL;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(rfs_files) / sizeof(rfs_files[0])); i++) {
        if (str_icmp(base, rfs_files[i].name) == 0) return &rfs_files[i];
    }
    if (str_icmp(base, "OBJECTS/1000000000000001.BLOB") == 0) return &rfs_files[0];
    if (str_icmp(base, "OBJECTS/1000000000000002.BLOB") == 0) return &rfs_files[1];
    return NULL;
}

static void cmd_pkg(char *args) {
    char *op = take_token(&args);
    char *name = take_token(&args);
    if (!*op) {
        print("Usage: PKG LIST | PKG INFO <name> | PKG INSTALL <name> | PKG REMOVE <name>\n");
        return;
    }
    if (str_icmp(op, "LIST") == 0) {
        for (uint32_t i = 0; i < (uint32_t)(sizeof(pkg_db) / sizeof(pkg_db[0])); i++) {
            print(pkg_db[i].installed ? "[*] " : "[ ] ");
            print(pkg_db[i].name);
            print(" ");
            print(pkg_db[i].version);
            print(" - ");
            print(pkg_db[i].desc);
            print("\n");
        }
        return;
    }
    if (!*name) {
        print("PKG: package name required\n");
        return;
    }
    for (uint32_t i = 0; i < (uint32_t)(sizeof(pkg_db) / sizeof(pkg_db[0])); i++) {
        if (str_icmp(name, pkg_db[i].name) != 0) {
            continue;
        }
        if (str_icmp(op, "INFO") == 0) {
            print(pkg_db[i].name); print(" "); print(pkg_db[i].version); print("\n");
            print(pkg_db[i].desc); print("\n");
            print(pkg_db[i].installed ? "installed\n" : "not installed\n");
            return;
        } else if (str_icmp(op, "INSTALL") == 0) {
            pkg_db[i].installed = true;
            print("Installed "); print(pkg_db[i].name); print("\n");
            return;
        } else if (str_icmp(op, "REMOVE") == 0) {
            if (str_icmp(pkg_db[i].name, "BASE") == 0) {
                print("PKG: BASE cannot be removed\n");
                return;
            }
            pkg_db[i].installed = false;
            print("Removed "); print(pkg_db[i].name); print("\n");
            return;
        }
        print("PKG: unknown operation\n");
        return;
    }
    print("PKG: package not found\n");
}

static void cmd_vcs(char *args) {
    char *op = take_token(&args);
    if (!*op) {
        print("Usage: VCS STATUS | VCS LOG | VCS BRANCH | VCS COMMIT <message>\n");
        return;
    }
    if (str_icmp(op, "STATUS") == 0) {
        print("On branch ");
        print(vcs_branch);
        print("\n");
        print(vcs_dirty ? "Changes pending commit\n" : "Working tree clean\n");
        return;
    }
    if (str_icmp(op, "BRANCH") == 0) {
        print("* ");
        print(vcs_branch);
        print("\n");
        return;
    }
    if (str_icmp(op, "LOG") == 0) {
        for (uint32_t i = 0; i < vcs_log_count; i++) {
            uint32_t idx = vcs_log_count - 1u - i;
            print_hex32((uint32_t)(vcs_log[idx].id >> 32));
            print_hex32((uint32_t)(vcs_log[idx].id & 0xFFFFFFFFu));
            print(" ");
            print(vcs_log[idx].msg);
            print("\n");
        }
        return;
    }
    if (str_icmp(op, "COMMIT") == 0) {
        args = skip_spaces(args);
        if (!*args) {
            print("VCS: commit message required\n");
            return;
        }
        if (vcs_log_count >= (uint32_t)(sizeof(vcs_log) / sizeof(vcs_log[0]))) {
            print("VCS: log is full\n");
            return;
        }
        copy_text(vcs_msgs[vcs_log_count], sizeof(vcs_msgs[0]), args);
        vcs_log[vcs_log_count].id = 0x3000000000000001ull + (uint64_t)vcs_log_count;
        vcs_log[vcs_log_count].msg = vcs_msgs[vcs_log_count];
        vcs_log_count++;
        vcs_dirty = false;
        print("Committed: ");
        print(args);
        print("\n");
        return;
    }
    print("VCS: unknown operation\n");
}

static bool fs_mount_image(const uint8_t *image, uint32_t image_size) {
    const fs_driver_t *driver = fs_fat12_driver();
    if (!driver || !driver->mount) {
        return false;
    }
    if (!driver->mount(image, image_size, &g_fs) || !g_fs || !g_fs->read_file) {
        return false;
    }
    g_fs_info.driver_name = driver->name;
    if (g_fs->get_info) {
        g_fs->get_info(&g_fs_info);
    }
    return true;
}

static bool fs_read_file(const char *name, uint8_t *buf, uint32_t max_len, uint32_t *out_len) {
    const rfs_file_t *vf = rfs_find_file(name);
    if (!vf) {
        return false;
    }
    uint32_t n = vf->len < max_len ? vf->len : max_len;
    for (uint32_t i = 0; i < n; i++) buf[i] = vf->data[i];
    *out_len = n;
    return n == vf->len;
}


static void cmd_dir(void) {
    uint32_t cursor = 0;
    fs_root_entry_t entry;
    print(" Directory of RFS:\\\n\n");
    if (!g_fs || !g_fs->list_root) {
        print("Filesystem not ready\n");
        return;
    }
    while (g_fs->list_root(&cursor, &entry)) {
        print(entry.name);
        print("    ");
        print_dec(entry.size);
        print(" bytes\n");
    }
}


static uint32_t fs_file_size(const char *name) {
    fs_stat_t st;
    if (!g_fs || !g_fs->stat) {
        return 0xFFFFFFFFu;
    }
    if (!g_fs->stat(name, &st)) {
        return 0xFFFFFFFFu;
    }
    return st.size;
}


static void cmd_type(const char *name) {
    uint32_t len = 0;
    if (!g_fs->read_file(name, file_buffer, sizeof(file_buffer) - 1u, &len)) {
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
    if (file_size == 0xFFFFFFFFu || !g_fs->read_file(name, file_buffer, count, &len)) {
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
    if (file_size == 0xFFFFFFFFu || !g_fs->read_file(name, file_buffer, sizeof(file_buffer), &len)) {
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

static void cmd_stat(char *args) {
    char *name = take_token(&args);
    uint32_t size;
    if (!*name) {
        print("Usage: STAT filename\n");
        return;
    }
    size = fs_file_size(name);
    if (size == 0xFFFFFFFFu) {
        print("File not found\n");
        return;
    }
    print(name);
    print(": ");
    print_dec(size);
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
    console_putc('\n');
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
    print("\nRFS refs ");
    print_dec((uint32_t)(sizeof(rfs_refs) / sizeof(rfs_refs[0])));
    print(", blobs ");
    print_dec((uint32_t)(sizeof(rfs_blobs) / sizeof(rfs_blobs[0])));
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
    print("\nRFS refs ");
    print_dec((uint32_t)(sizeof(rfs_refs) / sizeof(rfs_refs[0])));
    print(", blobs ");
    print_dec((uint32_t)(sizeof(rfs_blobs) / sizeof(rfs_blobs[0])));
    print("\n");
}

static void cmd_rfsrefs(void) {
    print("RFS references\n");
    for (uint32_t i = 0; i < (uint32_t)(sizeof(rfs_refs) / sizeof(rfs_refs[0])); i++) {
        print("  ");
        print(rfs_refs[i].name);
        print(" -> ");
        print_hex32((uint32_t)(rfs_refs[i].commit_oid >> 32));
        print_hex32((uint32_t)(rfs_refs[i].commit_oid & 0xFFFFFFFFu));
        print("\n");
    }
}

static void cmd_pause(void) {
    print("Press Enter to continue . . .");
    for (;;) {
        char c = read_char();
        if (c == '\n') {
            console_putc('\n');
            return;
        }
    }
}

static void cmd_beep(void) {
    console_putc('\a');
    print("Beep\n");
}


static int32_t parse_int32(const char *s, bool *ok) {
    bool neg = false;
    uint32_t value = 0;
    *ok = false;
    if (!s || !*s) {
        return 0;
    }
    if (*s == '-') {
        neg = true;
        s++;
    }
    if (!*s) {
        return 0;
    }
    while (*s) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
        value = value * 10u + (uint32_t)(*s - '0');
        s++;
    }
    *ok = true;
    if (neg) {
        return -(int32_t)value;
    }
    return (int32_t)value;
}

static script_var_t *find_runtime_var(const char *name) {
    for (uint32_t i = 0; i < runtime_var_count; i++) {
        if (str_icmp(runtime_vars[i].name, name) == 0) {
            return &runtime_vars[i];
        }
    }
    return 0;
}

static script_var_t *get_or_create_runtime_var(const char *name) {
    script_var_t *v = find_runtime_var(name);
    if (v) {
        return v;
    }
    if (runtime_var_count >= (sizeof(runtime_vars) / sizeof(runtime_vars[0]))) {
        return 0;
    }
    v = &runtime_vars[runtime_var_count++];
    copy_text(v->name, sizeof(v->name), name);
    v->value = 0;
    return v;
}

static void cmd_script(char *args) {
    uint32_t len = 0;
    uint8_t *script = file_buffer;
    char line[128];
    uint32_t pos = 0;

    args = skip_spaces(args);
    if (!*args) {
        print("Usage: SCRIPT filename\n");
        return;
    }
    if (!g_fs->read_file(args, script, sizeof(file_buffer) - 1u, &len)) {
        print("Script file not found or too large\n");
        return;
    }
    script[len] = 0;
    runtime_var_count = 0;

    for (uint32_t i = 0; i <= len; i++) {
        char c = (char)script[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n' || c == 0) {
            line[pos] = 0;
            char *cur = skip_spaces(line);
            trim_right(cur);
            if (*cur && !is_rem_comment(cur) && cur[0] != '#') {
                char *rest = cur;
                char *op = take_token(&rest);
                if (str_icmp(op, "SET") == 0) {
                    char *name = take_token(&rest);
                    bool ok;
                    int32_t v;
                    script_var_t *var;
                    if (!*name) { print("SCRIPT: SET needs variable\n"); goto fail; }
                    v = parse_int32(skip_spaces(rest), &ok);
                    if (!ok) { print("SCRIPT: SET needs integer value\n"); goto fail; }
                    var = get_or_create_runtime_var(name);
                    if (!var) { print("SCRIPT: variable table full\n"); goto fail; }
                    var->value = v;
                } else if (str_icmp(op, "ADD") == 0) {
                    char *name = take_token(&rest);
                    bool ok;
                    int32_t v;
                    script_var_t *var = find_runtime_var(name);
                    if (!var) { print("SCRIPT: unknown variable\n"); goto fail; }
                    v = parse_int32(skip_spaces(rest), &ok);
                    if (!ok) { print("SCRIPT: ADD needs integer value\n"); goto fail; }
                    var->value += v;
                } else if (str_icmp(op, "PRINT") == 0) {
                    char *arg = skip_spaces(rest);
                    script_var_t *var = find_runtime_var(arg);
                    if (var) {
                        print_dec((uint32_t)var->value);
                        print("\n");
                    } else {
                        print(arg);
                        print("\n");
                    }
                } else if (str_icmp(op, "RUN") == 0) {
                    execute_command(rest);
                } else {
                    print("SCRIPT: unknown instruction\n");
                    goto fail;
                }
            }
            pos = 0;
        } else if (pos + 1 < sizeof(line)) {
            line[pos++] = c;
        }
    }
    return;
fail:
    print("SCRIPT aborted\n");
}

static void cmd_help(char *topic) {
    topic = skip_spaces(topic);
    if (!*topic) {
        print("Commands: VER HELP DIR/LS TYPE/CAT DUMP/HEX WC RUN SCRIPT CLS MEM/INFO RFSREFS PKG VCS\n");
        print("          DATE TIME COLOR PROMPT PWD ECHO PAUSE BEEP EXIT REBOOT\n");
        return;
    }
    if (str_icmp(topic, "DUMP") == 0 || str_icmp(topic, "HEX") == 0) {
        print("DUMP filename [bytes] - show file bytes in hex and ASCII\n");
    } else if (str_icmp(topic, "WC") == 0) {
        print("WC filename - count lines, words, and bytes in a file\n");
    } else if (str_icmp(topic, "STAT") == 0) {
        print("STAT filename - show file size in bytes\n");
    } else if (str_icmp(topic, "RUN") == 0) {
        print("RUN filename - execute by type: .BAT/.CMD as batch, executable header as binary\n");
        print("            batch files run one command per line from root directory\n");
    } else if (str_icmp(topic, "SCRIPT") == 0) {
        print("SCRIPT filename - run runtime script language (SET/ADD/PRINT/RUN)\n");
    } else if (str_icmp(topic, "COLOR") == 0) {
        print("COLOR bgfg - set VGA text color with DOS hex digits, e.g. COLOR 1E\n");
    } else if (str_icmp(topic, "PROMPT") == 0) {
        print("PROMPT [text] - set the command prompt, or reset it without text\n");
    } else if (str_icmp(topic, "PAUSE") == 0) {
        print("PAUSE - wait for Enter before continuing\n");
    } else if (str_icmp(topic, "BEEP") == 0) {
        print("BEEP - emit an ASCII bell on the active console\n");
    } else if (str_icmp(topic, "RFSREFS") == 0) {
        print("RFSREFS - show git-style refs and 64-bit commit IDs\n");
    } else if (str_icmp(topic, "PKG") == 0) {
        print("PKG - proprietary package manager (LIST/INFO/INSTALL/REMOVE)\n");
    } else if (str_icmp(topic, "VCS") == 0) {
        print("VCS - proprietary version control (STATUS/LOG/BRANCH/COMMIT)\n");
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
    print("\nRFS refs ");
    print_dec((uint32_t)(sizeof(rfs_refs) / sizeof(rfs_refs[0])));
    print(", blobs ");
    print_dec((uint32_t)(sizeof(rfs_blobs) / sizeof(rfs_blobs[0])));
    print("\n");
}

static void cmd_info(void) {
    cmd_mem();
    print("FS driver: ");
    print(g_fs_info.driver_name ? g_fs_info.driver_name : "unknown");
    print("\nFAT12 root LBA ");
    print_dec(g_fs_info.root_lba);
    print(", data LBA ");
    print_dec(g_fs_info.data_lba);
    print("\nRFS refs ");
    print_dec((uint32_t)(sizeof(rfs_refs) / sizeof(rfs_refs[0])));
    print(", blobs ");
    print_dec((uint32_t)(sizeof(rfs_blobs) / sizeof(rfs_blobs[0])));
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

static void run_rxe_file(const char *name) {
    uint32_t len = 0;
    char cmd[128];
    uint32_t pos = 0;
    uint8_t *payload;
    if (!fs_read_file(name, file_buffer, sizeof(file_buffer) - 1u, &len)) {
        print("Executable not found or too large\n");
        return;
    }
    if (len < 4 || file_buffer[0] != '6' || file_buffer[1] != '4' || file_buffer[2] != 'E' || file_buffer[3] != 'X') {
        print("Invalid RXE header\n");
        return;
    }
    payload = file_buffer + 4;
    len -= 4;
    payload[len] = 0;
    for (uint32_t i = 0; i <= len; i++) {
        char c = (char)payload[i];
        if (c == '\r') continue;
        if (c == '\n' || c == 0) {
            cmd[pos] = 0;
            trim_right(cmd);
            char *trimmed = skip_spaces(cmd);
            if (*trimmed) execute_command(trimmed);
            pos = 0;
        } else if (pos + 1 < sizeof(cmd)) {
            cmd[pos++] = c;
        }
    }
}

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
    if (!g_fs->read_file(name, script, sizeof(script_buffers[0]) - 1u, &len)) {
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
            if (*trimmed && upper(trimmed[0]) != ':' && !is_rem_comment(trimmed)) {
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
        print("RFS:\\\n");
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
    } else if (str_icmp(cmd, "PAUSE") == 0) {
        cmd_pause();
    } else if (str_icmp(cmd, "BEEP") == 0) {
        cmd_beep();
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
    } else if (str_icmp(cmd, "STAT") == 0) {
        cmd_stat(args);
    } else if (str_icmp(cmd, "RUN") == 0) {
        if (*args) {
            uint32_t n = str_len(args);
            if (n >= 4 && upper(args[n - 1]) == 'E' && upper(args[n - 2]) == 'X' && upper(args[n - 3]) == 'R' && args[n - 4] == '.') {
                run_rxe_file(args);
            } else {
                run_script_file(args);
            }
        } else {
            print("Usage: RUN filename\n");
        }
    } else if (str_icmp(cmd, "SCRIPT") == 0) {
        cmd_script(args);
    } else if (str_icmp(cmd, "RFSREFS") == 0) {
        cmd_rfsrefs();
    } else if (str_icmp(cmd, "PKG") == 0) {
        cmd_pkg(args);
    } else if (str_icmp(cmd, "VCS") == 0) {
        cmd_vcs(args);
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
    if (!fs_mount_image(g_image, g_image_size)) {
        print("Image mount failed: no supported filesystem\n");
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
