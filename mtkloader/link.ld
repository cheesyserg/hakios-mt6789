ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

    .text : {
        *(.text.boot)
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(16);
    __loader_end = .;
}

/* Android's Image follows at CHAIN_OFFSET (tools/chainboot.py): start.S clears the BSS, which must end before it. */
ASSERT(__loader_end <= 0x40080000 + 0x40000, "mtkloader: program and BSS run into Android's Image at CHAIN_OFFSET")
