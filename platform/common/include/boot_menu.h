/* Copyright Statement:
*
* This software/firmware and related documentation ("MediaTek Software") are
* protected under relevant copyright laws. The information contained herein
* is confidential and proprietary to MediaTek Inc. and/or its licensors.
* Without the prior written permission of MediaTek inc. and/or its licensors,
* any reproduction, modification, use or disclosure of MediaTek Software,
* and information contained herein, in whole or in part, shall be strictly prohibited.
*/
/* MediaTek Inc. (C) 2019. All rights reserved.
*
* BY OPENING THIS FILE, RECEIVER HEREBY UNEQUIVOCALLY ACKNOWLEDGES AND AGREES
* THAT THE SOFTWARE/FIRMWARE AND ITS DOCUMENTATIONS ("MEDIATEK SOFTWARE")
* RECEIVED FROM MEDIATEK AND/OR ITS REPRESENTATIVES ARE PROVIDED TO RECEIVER ON
* AN "AS-IS" BASIS ONLY. MEDIATEK EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE OR NONINFRINGEMENT.
* NEITHER DOES MEDIATEK PROVIDE ANY WARRANTY WHATSOEVER WITH RESPECT TO THE
* SOFTWARE OF ANY THIRD PARTY WHICH MAY BE USED BY, INCORPORATED IN, OR
* SUPPLIED WITH THE MEDIATEK SOFTWARE, AND RECEIVER AGREES TO LOOK ONLY TO SUCH
* THIRD PARTY FOR ANY WARRANTY CLAIM RELATING THERETO. RECEIVER EXPRESSLY ACKNOWLEDGES
* THAT IT IS RECEIVER'S SOLE RESPONSIBILITY TO OBTAIN FROM ANY THIRD PARTY ALL PROPER LICENSES
* CONTAINED IN MEDIATEK SOFTWARE. MEDIATEK SHALL ALSO NOT BE RESPONSIBLE FOR ANY MEDIATEK
* SOFTWARE RELEASES MADE TO RECEIVER'S SPECIFICATION OR TO CONFORM TO A PARTICULAR
* STANDARD OR OPEN FORUM. RECEIVER'S SOLE AND EXCLUSIVE REMEDY AND MEDIATEK'S ENTIRE AND
* CUMULATIVE LIABILITY WITH RESPECT TO THE MEDIATEK SOFTWARE RELEASED HEREUNDER WILL BE,
* AT MEDIATEK'S OPTION, TO REVISE OR REPLACE THE MEDIATEK SOFTWARE AT ISSUE,
* OR REFUND ANY SOFTWARE LICENSE FEES OR SERVICE CHARGE PAID BY RECEIVER TO
* MEDIATEK FOR SUCH MEDIATEK SOFTWARE AT ISSUE.

*
* The following software/firmware and/or related documentation ("MediaTek Software")
* have been modified by MediaTek Inc. All revisions are subject to any receiver\'s
* applicable license agreements with MediaTek Inc.
*/
#if !defined (__BOOT_MENU_H__)
#define __BOOT_MENU_H__

/*
 * Author credit, drawn once on the boot menu and once on the fastboot
 * screen ("Developed by Melek Saidani", name in the accent colour).
 */
#define NOVALK_CREDIT_PREFIX	"Developed by "
#define NOVALK_CREDIT_NAME	"Melek Saidani"

void boot_menu_select();

/*
 * NovaLK fastboot screen (platform/common/boot_menu/fastboot_ui.c).
 *
 * novalk_fastboot_ui_show() paints the fastboot screen in the boot menu
 * style; novalk_fastboot_ui_usb() repaints only the USB status card and is
 * safe to call before the screen exists (it does nothing then).
 */
/*
 * NovaLK Snake (platform/common/boot_menu/snake.c): a small touch game
 * opened from the boot menu.  Returns when the player leaves the game.
 * touch_ok tells whether tpd_read() may be used; without it the game is
 * played with the volume keys.
 */
void novalk_snake_play(int touch_ok);

/*
 * Boot menu lock (platform/common/boot_menu/lock_ui.c, lock_store.c).
 *
 * novalk_lock_gate() blocks until the right PIN / pattern was entered; it
 * returns immediately when no password is set.  novalk_lock_settings() is
 * the SECURITY screen (set, change, remove).  touch_ok tells whether
 * tpd_read() may be used; without it the hardware keys are used.
 */
void novalk_lock_gate(int touch_ok);

/*
 * NovaOS (platform/common/boot_menu/novaos.c): the small touch environment
 * with a home screen and apps (Info, Storage, Calc, Paint, Clock, Snake,
 * Security, Power).  Returns when the user leaves it.
 */
void novalk_os_run(int touch_ok);

/*
 * Wi-Fi bring-up, stage 1 (platform/common/boot_menu/consys.c): power up
 * the connectivity block, read its ID registers, power it down again.
 * Fills lines[] with a report (46 characters per line); returns 1 when the
 * expected IP version was read.
 */
int novalk_consys_probe(char (*lines)[46], int max, int *count);

/*
 * Wi-Fi bring-up, stage 2: reserve the shared DRAM block, copy the MCU and
 * Wi-Fi ROM patches into it, release the MCU and wait for its ready id.
 * Returns 1 when the MCU reports ready.
 */
int novalk_consys_stage2(char (*lines)[46], int max, int *count);
void novalk_lock_settings(int touch_ok);

void novalk_fastboot_ui_show(void);
void novalk_fastboot_ui_usb(int online);

#endif /* __BOOT_MENU_H__ */
