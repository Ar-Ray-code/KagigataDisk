// Documents embedded as-is and served by the USB drive (usbdisk.c):
//   SKILL_MD_PATH  -> /SKILL.md
//   README_MD_PATH -> /spi_virtual_device/README.md
// Both paths come from CMakeLists.txt.
    .section .rodata.skill_md, "a"
    .global skill_md_start
    .global skill_md_end
skill_md_start:
    .incbin SKILL_MD_PATH
skill_md_end:
    .global readme_md_start
    .global readme_md_end
readme_md_start:
    .incbin README_MD_PATH
readme_md_end:
