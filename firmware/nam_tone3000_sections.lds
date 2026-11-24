/* BOOT_SRAM: keep the ITCM load image inside the SRAM image the bootloader
 * copies. The eight-byte gap avoids placing a function at a null address. */
SECTIONS
{
  .nam_itcm_text :
  {
    . = ALIGN(8);
    . += 8;
    _nam_itcm_start = .;
    *nam_a2lite.o(.text .text*)
    . = ALIGN(4);
    _nam_itcm_end = .;
  } > ITCMRAM AT > SRAM
  _snam_itcm = LOADADDR(.nam_itcm_text)
      + (_nam_itcm_start - ADDR(.nam_itcm_text));
} INSERT BEFORE .text;

SECTIONS
{
  .nam_axi_bss (NOLOAD) :
  {
    . = ALIGN(32);
    *(.nam_axi_bss .nam_axi_bss*)
    . = ALIGN(32);
  } > SRAM
} INSERT BEFORE .heap;
