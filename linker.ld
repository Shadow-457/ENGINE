OUTPUT_FORMAT(binary)

SECTIONS {
    . = 0x8000;

    .text : {
        *(.text.start)   /* entry.S must be first */
        *(.text .text.*) /* all other code */
    }

    .rodata : { *(.rodata .rodata.*) }

    .data : { *(.data .data.*) }

    _bss_start = .;
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
    _bss_end = .;
}
