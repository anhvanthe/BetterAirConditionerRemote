/* Linker script to configure memory regions. */

SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
  FLASH (rx) : ORIGIN = 0x23000, LENGTH = 0x5d000
  RAM (rwx) :  ORIGIN = 0x20001380, LENGTH = 0xec80
  
}

SECTIONS
{
}

SECTIONS
{
  . = ALIGN(4);
  .mem_section_dummy_ram :
  {
  }

} INSERT AFTER .data;

SECTIONS
{
  .mem_section_dummy_rom :
  {
  }
  

} INSERT AFTER .text

INCLUDE "nrf5x_common.ld"
