; 64DOS BIOS stage1 boot sector.
; Released into the public domain with the rest of 64DOS.

bits 16
org 0x7C00

STAGE2_LOAD        equ 0x8000
STAGE2_SECTORS     equ 31
SECTORS_PER_TRACK  equ 18
HEADS              equ 2

jmp short start
nop

; FAT12 1.44 MB BIOS Parameter Block. scripts/mkfloppy.py verifies these
; fields after inserting this boot sector into the final image.
db '64DOS1  '
dw 512
db 1
dw 32
db 2
dw 224
dw 2880
db 0xF0
dw 9
dw SECTORS_PER_TRACK
dw HEADS
dd 0
dd 0
db 0
db 0
db 0x29
dd 0x64006400
db '64DOS FLOP'
db 'FAT12   '

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    call serial_init
    mov [boot_drive], dl
    mov si, msg_stage1
    call puts

    mov bx, STAGE2_LOAD
    mov ax, 1
    mov cx, STAGE2_SECTORS

.load_stage2:
    call read_lba
    add bx, 512
    inc ax
    loop .load_stage2

    mov dl, [boot_drive]
    jmp 0x0000:STAGE2_LOAD

read_lba:
    push ax
    push bx
    push cx
    push dx
    push ds

    ; Convert LBA in AX to CHS for a standard 1.44 MB floppy.
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

disk_error:
    mov si, msg_disk_error
    call puts
.halt:
    cli
    hlt
    jmp .halt

boot_drive: db 0
msg_stage1: db '64DOS stage1', 13, 10, 0
msg_disk_error: db 'Disk read error', 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
