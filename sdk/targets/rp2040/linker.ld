MEMORY
{
    FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 2048K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 264K
}

ENTRY(Reset_Handler)

SECTIONS
{
    .boot2 :
    {
        __boot2_start__ = .;
        KEEP(*(.boot2))
        . = __boot2_start__ + 256;
    } > FLASH

    .vectors :
    {
        . = ALIGN(256);
        __vectors_start__ = .;
        KEEP(*(.vectors))
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .data : AT (_etext)
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM

    _sidata = LOADADDR(.data);

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.comment)
        *(.note*)
    }

    __stack_top = ORIGIN(RAM) + LENGTH(RAM);
}
