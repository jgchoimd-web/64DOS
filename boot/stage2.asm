; 64DOS BIOS stage2 loader.
; Loads the whole floppy image to RAM, copies the kernel, enters x86-64
; long mode, and jumps to the 64-bit kernel.
; Released into the public domain with the rest of 64DOS.

bits 16
org 0x8000

CODE32_SEL        equ 0x08
DATA_SEL          equ 0x10
CODE64_SEL        equ 0x18

SECTORS_PER_TRACK equ 18
HEADS             equ 2

BOOTINFO_ADDR     equ 0x5000
BOUNCE_BUFFER     equ 0x6000
MANIFEST_ADDR     equ 0xBC00

IMAGE_BASE        equ 0x100000
KERNEL_LOAD       equ 0x300000
PML4_BASE         equ 0x1000
PDPT_BASE         equ 0x2000
PD_BASE           equ 0x3000

MANIFEST_MAGIC    equ 0x464D3436 ; "64MF"
BOOTINFO_MAGIC    equ 0x42443436 ; "64DB"

stage2_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    call serial_init
    mov [boot_drive], dl
    mov si, msg_stage2
    call puts

    cmp dword [MANIFEST_ADDR], MANIFEST_MAGIC
    jne manifest_error

    call enable_a20
    call enter_unreal_mode
    call verify_unreal_mode
    mov si, msg_loading
    call puts
    call load_floppy_image
    mov si, msg_image_ok
    call puts
    call enter_unreal_mode
    call verify_image_base
    call verify_kernel_source
    call copy_kernel
    call verify_kernel_copy
    mov si, msg_kernel_ok
    call puts
    call write_boot_info
    call setup_page_tables
    mov si, msg_long_mode
    call puts
    call enter_long_mode

halt:
    cli
    hlt
    jmp halt

load_floppy_image:
    mov word [current_lba], 0
    mov dword [dest_addr], IMAGE_BASE

.next_sector:
    mov ax, [current_lba]
    cmp ax, [MANIFEST_ADDR + 8]
    jae .done

    call read_lba_to_bounce
    call verify_kernel_bounce
    call enter_unreal_mode
    call copy_bounce_to_dest

    inc word [current_lba]
    add dword [dest_addr], 512
    jmp .next_sector

.done:
    ret

verify_unreal_mode:
    mov edi, 0x280000
    mov dword [fs:edi], 0x12345678
    mov esi, 0x280000
    cmp dword [fs:esi], 0x12345678
    jne unreal_error
    ret

verify_image_base:
    mov esi, IMAGE_BASE
    mov al, [fs:esi]
    cmp al, 0xEB
    jne image_base_error
    ret

verify_kernel_bounce:
    mov ax, [current_lba]
    cmp ax, [MANIFEST_ADDR + 12]
    jne .done
    cmp byte [BOUNCE_BUFFER], 0xFC
    jne kernel_bounce_error
.done:
    ret

verify_kernel_source:
    mov esi, IMAGE_BASE
    mov eax, [MANIFEST_ADDR + 12]
    shl eax, 9
    add esi, eax
    mov al, [fs:esi]
    cmp al, 0xFC
    jne kernel_source_error
    ret

copy_kernel:
    mov esi, IMAGE_BASE
    mov eax, [MANIFEST_ADDR + 12]
    shl eax, 9
    add esi, eax

    mov edi, [MANIFEST_ADDR + 28]
    mov ecx, [MANIFEST_ADDR + 16]
    shl ecx, 7

.copy_dword:
    test ecx, ecx
    jz .done
    mov eax, [fs:esi]
    mov [fs:edi], eax
    add esi, 4
    add edi, 4
    dec ecx
    jmp .copy_dword

.done:
    ret

verify_kernel_copy:
    mov esi, KERNEL_LOAD
    mov al, [fs:esi]
    cmp al, 0xFC
    jne kernel_copy_error
    ret

write_boot_info:
    mov dword [BOOTINFO_ADDR + 0], BOOTINFO_MAGIC
    mov dword [BOOTINFO_ADDR + 4], 1

    mov eax, [MANIFEST_ADDR + 24]
    mov [BOOTINFO_ADDR + 8], eax

    mov eax, [MANIFEST_ADDR + 8]
    shl eax, 9
    mov [BOOTINFO_ADDR + 12], eax

    mov eax, [MANIFEST_ADDR + 28]
    mov [BOOTINFO_ADDR + 16], eax

    mov eax, [MANIFEST_ADDR + 20]
    mov [BOOTINFO_ADDR + 20], eax

    mov dword [BOOTINFO_ADDR + 24], 1

    xor eax, eax
    mov al, [boot_drive]
    mov [BOOTINFO_ADDR + 28], eax

    mov dword [BOOTINFO_ADDR + 32], MANIFEST_ADDR
    ret

copy_bounce_to_dest:
    push si
    push cx
    push eax
    push edi

    mov si, BOUNCE_BUFFER
    mov edi, [dest_addr]
    mov cx, 128

.copy:
    lodsd
    mov [fs:edi], eax
    add edi, 4
    loop .copy

    pop edi
    pop eax
    pop cx
    pop si
    ret

read_lba_to_bounce:
    push ax
    push bx
    push cx
    push dx
    push ds

    xor dx, dx
    mov cx, SECTORS_PER_TRACK * HEADS
    div cx
    push ax

    mov ax, dx
    xor dx, dx
    mov cx, SECTORS_PER_TRACK
    div cx
    mov dh, al
    mov cl, dl
    inc cl
    pop ax
    mov ch, al

    xor ax, ax
    mov es, ax
    mov bx, BOUNCE_BUFFER
    mov dl, [boot_drive]
    mov ax, 0x0201
    int 0x13
    jc disk_error

    pop ds
    pop dx
    pop cx
    pop bx
    pop ax
    ret

enable_a20:
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al
    ret

enter_unreal_mode:
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    mov ax, DATA_SEL
    mov fs, ax
    mov gs, ax
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax
    xor ax, ax
    mov ds, ax
    mov es, ax
    sti
    ret

setup_page_tables:
    xor eax, eax
    mov edi, PML4_BASE
    mov ecx, (4096 * 3) / 4

.clear:
    mov [edi], eax
    add edi, 4
    dec ecx
    jnz .clear

    mov dword [PML4_BASE], PDPT_BASE | 0x003
    mov dword [PML4_BASE + 4], 0
    mov dword [PDPT_BASE], PD_BASE | 0x003
    mov dword [PDPT_BASE + 4], 0

    mov edi, PD_BASE
    mov eax, 0x00000083
    xor edx, edx
    mov ecx, 512

.map_2m:
    mov [edi], eax
    mov [edi + 4], edx
    add eax, 0x00200000
    adc edx, 0
    add edi, 8
    dec ecx
    jnz .map_2m
    ret

enter_long_mode:
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp CODE32_SEL:protected_entry

bits 32
protected_entry:
    mov ax, DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov eax, cr4
    or eax, 0x20
    mov cr4, eax

    mov eax, PML4_BASE
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 0x100
    wrmsr

    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    jmp CODE64_SEL:long_mode_entry

bits 64
long_mode_entry:
    mov ax, DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, 0x90000
    mov rdi, BOOTINFO_ADDR
    mov rax, KERNEL_LOAD
    jmp rax

bits 16
puts:
    lodsb
    test al, al
    jz .done
    call serial_putc
    push ax
    push bx
    push ds
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    pop ds
    pop bx
    pop ax
    jmp puts
.done:
    ret

serial_init:
    push ax
    push dx
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8
    mov al, 0x03
    out dx, al
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x03
    out dx, al
    mov dx, 0x3FA
    mov al, 0xC7
    out dx, al
    mov dx, 0x3FC
    mov al, 0x0B
    out dx, al
    pop dx
    pop ax
    ret

serial_putc:
    push ax
    push dx
    mov ah, al
.wait:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jz .wait
    mov al, ah
    mov dx, 0x3F8
    out dx, al
    pop dx
    pop ax
    ret

manifest_error:
    mov si, msg_manifest_error
    call puts
    jmp halt

disk_error:
    mov si, msg_disk_error
    call puts
    jmp halt

kernel_copy_error:
    mov si, msg_kernel_copy_error
    call puts
    jmp halt

kernel_source_error:
    mov si, msg_kernel_source_error
    call puts
    jmp halt

kernel_bounce_error:
    mov si, msg_kernel_bounce_error
    call puts
    jmp halt

image_base_error:
    mov si, msg_image_base_error
    call puts
    jmp halt

unreal_error:
    mov si, msg_unreal_error
    call puts
    jmp halt

align 8
gdt_start:
    dq 0
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
    dq 0x00AF9A000000FFFF
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

boot_drive: db 0
current_lba: dw 0
dest_addr: dd 0

msg_stage2: db '64DOS stage2', 13, 10, 0
msg_loading: db 'Loading floppy image', 13, 10, 0
msg_image_ok: db 'Image loaded', 13, 10, 0
msg_kernel_ok: db 'Kernel copied', 13, 10, 0
msg_long_mode: db 'Entering long mode', 13, 10, 0
msg_manifest_error: db 'Manifest error', 13, 10, 0
msg_disk_error: db 'Disk read error', 13, 10, 0
msg_image_base_error: db 'Image base error', 13, 10, 0
msg_kernel_bounce_error: db 'Kernel read error', 13, 10, 0
msg_kernel_source_error: db 'Kernel source error', 13, 10, 0
msg_kernel_copy_error: db 'Kernel copy error', 13, 10, 0
msg_unreal_error: db 'Unreal mode error', 13, 10, 0
