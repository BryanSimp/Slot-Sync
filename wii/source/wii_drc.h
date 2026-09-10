/* Wii U GamePad input, vendored from FIX94's libwiidrc.
 *
 * Taken verbatim (MIT, header below) from the copy in vBA-GX's
 * source/utils/wiidrc.c. Vendored rather than linked because Nintendont ships
 * `libwiidrc.a` built against devkitPPC r35 / libogc 1.8.23, and this launcher
 * builds against r47.1 / libogc 2.12.2.
 *
 * Why libogc cannot do this itself: the GamePad is not a Wii input device. In
 * vWii its state is maintained by IOS, and this reads it straight out of IOS's
 * own memory at addresses that differ per IOS revision -- hence the pattern
 * match in __WiiDRC_SetI2CBuf. That needs the `ahb_access` privilege the
 * launcher's meta.xml already asks for.
 *
 * On a real Wii the pattern does not match, WiiDRC_Init() answers false, and
 * every other entry point returns nothing. So it is safe to compile in
 * unconditionally and simply ask.
 */
/*
 * Copyright (C) 2017 FIX94
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */
#ifndef SLOTSYNC_WII_DRC_H
#define SLOTSYNC_WII_DRC_H

#include <gctypes.h>

#ifdef __cplusplus
extern "C" {
#endif

struct WiiDRCData {
	s16 xAxisL;
	s16 xAxisR;
	s16 yAxisL;
	s16 yAxisR;
	u16 button;
	u8 battery;
	u8 extra;
};

#define WIIDRC_BUTTON_A			0x8000
#define WIIDRC_BUTTON_B			0x4000
#define WIIDRC_BUTTON_X			0x2000
#define WIIDRC_BUTTON_Y			0x1000
#define WIIDRC_BUTTON_LEFT		0x0800
#define WIIDRC_BUTTON_RIGHT		0x0400
#define WIIDRC_BUTTON_UP		0x0200
#define WIIDRC_BUTTON_DOWN		0x0100
#define WIIDRC_BUTTON_ZL		0x0080
#define WIIDRC_BUTTON_ZR		0x0040
#define WIIDRC_BUTTON_L			0x0020
#define WIIDRC_BUTTON_R			0x0010
#define WIIDRC_BUTTON_PLUS		0x0008
#define WIIDRC_BUTTON_MINUS		0x0004
#define WIIDRC_BUTTON_HOME		0x0002
#define WIIDRC_BUTTON_SYNC		0x0001

#define WIIDRC_EXTRA_BUTTON_L3		0x80
#define WIIDRC_EXTRA_BUTTON_R3		0x40
#define WIIDRC_EXTRA_BUTTON_TV		0x20
#define WIIDRC_EXTRA_OVERLAY_TV		0x10
#define WIIDRC_EXTRA_OVERLAY_POWER	0x01

bool WiiDRC_Init();
bool WiiDRC_Inited();
bool WiiDRC_Recalibrate();
bool WiiDRC_ScanPads();
bool WiiDRC_Connected();
bool WiiDRC_ShutdownRequested();
const u8 *WiiDRC_GetRawI2CAddr();
const struct WiiDRCData *WiiDRC_Data();
u32 WiiDRC_ButtonsUp();
u32 WiiDRC_ButtonsDown();
u32 WiiDRC_ButtonsHeld();
s16 WiiDRC_lStickX();
s16 WiiDRC_lStickY();
s16 WiiDRC_rStickX();
s16 WiiDRC_rStickY();

#ifdef __cplusplus
}
#endif

#endif
