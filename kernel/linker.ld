ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x300000;

    .text : ALIGN(16) {
        *(.text*)
    } :text

    .rodata : ALIGN(16) {
        *(.rodata*)
    } :text

    .data : ALIGN(16) {
        *(.data*)
    } :data

    .bss : ALIGN(16) {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        __bss_end = .;
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note*)
        *(.comment*)
    }
}
