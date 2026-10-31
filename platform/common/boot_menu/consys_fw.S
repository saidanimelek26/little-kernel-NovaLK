/*
 * ROM patch images of the MT6765 connectivity block (CONNSYS), taken from
 * the stock RMX3201 firmware (/vendor/firmware):
 *
 *   soc1_0_ram_mcu_1_1_hdr.bin    105048 bytes  MCU code, patch type 4
 *   soc1_0_ram_wifi_1_1_hdr.bin   195580 bytes  Wi-Fi code, patch type 3
 *
 * Each file starts with a 32 byte header (build date, "ALPS", HW/SW
 * version 0x008a, EMI address word, patch type); the body follows.
 *
 * .incbin resolves relative to the build root, where the assembler runs.
 */

	.section .rodata

	.global consys_patch_mcu
	.global consys_patch_mcu_end
	.global consys_patch_wifi
	.global consys_patch_wifi_end

	.align 4
consys_patch_mcu:
	.incbin "platform/common/boot_menu/fw/soc1_0_ram_mcu_1_1_hdr.bin"
consys_patch_mcu_end:

	.align 4
consys_patch_wifi:
	.incbin "platform/common/boot_menu/fw/soc1_0_ram_wifi_1_1_hdr.bin"
consys_patch_wifi_end:
	.align 4
