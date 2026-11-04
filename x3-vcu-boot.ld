/* AT32F415CBT7 bootloader: first 4 KB of flash @ 0x08000000, 32 KB SRAM.
   Compatible with Artery's startup_at32f415.s (expects _estack, _sidata,
   _sdata/_edata, _sbss/_ebss). */
_estack = 0x20008000;      /* top of 32 KB SRAM */

MEMORY
{
    BOOT (rx)  : ORIGIN = 0x08000000, LENGTH = 4K
    RAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 32K
}

/* Reset_Handler is the gc-sections root; without it the linker never pulls the
   startup object (vector table + Reset_Handler) out of libcmsis.a. */
ENTRY(Reset_Handler)

SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) . = ALIGN(4); } > BOOT

    .text : {
        *(.text*) *(.rodata*)
        KEEP(*(.init)) KEEP(*(.fini))
        . = ALIGN(4);
        _etext = .;
    } > BOOT

    _sidata = LOADADDR(.data);
    .data : {
        . = ALIGN(4); _sdata = .;
        *(.data*)
        . = ALIGN(4); _edata = .;
    } > RAM AT > BOOT

    .bss : {
        . = ALIGN(4); _sbss = .; __bss_start__ = .;
        *(.bss*) *(COMMON)
        . = ALIGN(4); _ebss = .; __bss_end__ = .;
    } > RAM

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}

ASSERT(SIZEOF(.isr_vector) + SIZEOF(.text) + SIZEOF(.data) <= 4K, "bootloader exceeds 4 KB")
