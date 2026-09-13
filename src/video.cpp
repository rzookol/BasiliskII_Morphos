/*
 *  video.cpp - Video/graphics emulation
 *
 *  Basilisk II (C) 1997-2001 Christian Bauer
 *  Portions (C) 1997-1999 Marc Hellwig
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 *  SEE ALSO
 *    Inside Macintosh: Devices, chapter 1 "Device Manager"
 *    Designing Cards and Drivers for the Macintosh Family, Second Edition
 */

#include <stdio.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "macos_util.h"
#include "video.h"
#include "video_defs.h"

#define DEBUG 0
#include "debug.h"


// Description of the main monitor
video_desc VideoMonitor;

// Local variables (per monitor)
struct {
	video_desc *desc;			// Pointer to monitor description
	uint8 palette[256 * 3];		// Color palette, 256 entries, RGB
	bool luminance_mapping;		// Luminance mapping on/off
	bool interrupts_enabled;	// VBL interrupts on/off
} VidLocal;

#ifdef __MORPHOS__
static uint16 current_apple_mode = 0x82;
static uint16 preferred_apple_mode = 0x82;
static uint32 preferred_resolution_id = 0x80;

static uint16 morphos_apple_mode_for_video_mode(int mode)
{
	switch (mode) {
		case VMODE_8BIT: return 0x80;
		case VMODE_16BIT: return 0x81;
		case VMODE_32BIT: return 0x82;
		default: return 0x80;
	}
}

static int morphos_video_mode_for_apple_mode(uint16 mode)
{
	switch (mode) {
		case 0x80: return VMODE_8BIT;
		case 0x81: return VMODE_16BIT;
		case 0x82: return VMODE_32BIT;
		default: return -1;
	}
}

static int16 morphos_set_gamma(uint32 table)
{
	uint8 red[256], green[256], blue[256];

	if (table == 0) {
		for (int i = 0; i < 256; ++i)
			red[i] = green[i] = blue[i] = (uint8)i;
		video_set_gamma(red, green, blue);
		return noErr;
	}

	if (ReadMacInt16(table + gVersion) != 0 || ReadMacInt16(table + gType) != 0)
		return paramErr;

	uint16 formula_size = ReadMacInt16(table + gFormulaSize);
	uint16 channels = ReadMacInt16(table + gChanCnt);
	uint16 data_count = ReadMacInt16(table + gDataCnt);
	uint16 data_width = ReadMacInt16(table + gDataWidth);
	if ((channels != 1 && channels != 3) || data_width > 8)
		return paramErr;
	if (data_count != (1U << data_width))
		return paramErr;

	uint32 data = table + gFormulaData + formula_size;
	int shift = 8 - data_width;
	for (int i = 0; i < 256; ++i) {
		uint32 index = (uint32)i >> shift;
		red[i] = ReadMacInt8(data + index);
		if (channels == 1) {
			green[i] = blue[i] = red[i];
		} else {
			green[i] = ReadMacInt8(data + data_count + index);
			blue[i] = ReadMacInt8(data + data_count * 2 + index);
		}
	}
	video_set_gamma(red, green, blue);
	return noErr;
}

static int16 morphos_switch_mode(uint16 apple_mode, uint32 param)
{
	int mode = morphos_video_mode_for_apple_mode(apple_mode);
	if (mode < 0)
		return paramErr;
	VidLocal.desc->mode = mode;
	uint32 bytes = mode == VMODE_8BIT ? 1 : mode == VMODE_16BIT ? 2 : 4;
	VidLocal.desc->bytes_per_row = (VidLocal.desc->x * bytes + 31) & ~31;
	current_apple_mode = apple_mode;
	WriteMacInt32(param + csBaseAddr, VidLocal.desc->mac_frame_base);
	video_set_mode(mode);
	if (!IsDirectMode(mode))
		video_set_palette(VidLocal.palette);
	return noErr;
}
#endif


/*
 *  Driver Open() routine
 */

int16 VideoDriverOpen(uint32 pb, uint32 dce)
{
	D(bug("VideoDriverOpen\n"));

	// Init local variables
	VidLocal.desc = &VideoMonitor;
	VidLocal.luminance_mapping = false;
	VidLocal.interrupts_enabled = false;
#ifdef __MORPHOS__
	current_apple_mode = morphos_apple_mode_for_video_mode(VidLocal.desc->mode);
	preferred_apple_mode = current_apple_mode;
	preferred_resolution_id = 0x80;
#endif

	// Init color palette (solid gray)
	if (!IsDirectMode(VidLocal.desc->mode)) {
		for (int i=0; i<256; i++) {
			VidLocal.palette[i * 3 + 0] = 127;
			VidLocal.palette[i * 3 + 1] = 127;
			VidLocal.palette[i * 3 + 2] = 127;
		}
		video_set_palette(VidLocal.palette);
	}
	return noErr;
}


/*
 *  Driver Control() routine
 */

int16 VideoDriverControl(uint32 pb, uint32 dce)
{
	uint16 code = ReadMacInt16(pb + csCode);
	uint32 param = ReadMacInt32(pb + csParam);
	D(bug("VideoDriverControl %d\n", code));
	switch (code) {

		case cscSetMode:		// Set color depth
			D(bug(" SetMode %04x\n", ReadMacInt16(param + csMode)));
#ifdef __MORPHOS__
			return morphos_switch_mode(ReadMacInt16(param + csMode), param);
#else
			WriteMacInt32(param + csBaseAddr, VidLocal.desc->mac_frame_base);
			return noErr;
#endif

		case cscSetEntries: {	// Set palette
			D(bug(" SetEntries table %08lx, count %d, start %d\n", ReadMacInt32(param + csTable), ReadMacInt16(param + csCount), ReadMacInt16(param + csStart)));
			if (IsDirectMode(VidLocal.desc->mode))
				return controlErr;

			uint32 s_pal = ReadMacInt32(param + csTable);	// Source palette
			uint8 *d_pal;									// Destination palette
			uint16 count = ReadMacInt16(param + csCount);
			if (!s_pal || count > 255)
				return paramErr;

			if (ReadMacInt16(param + csStart) == 0xffff) {	// Indexed
				for (uint32 i=0; i<=count; i++) {
					d_pal = VidLocal.palette + ReadMacInt16(s_pal) * 3;
					uint8 red = (uint16)ReadMacInt16(s_pal + 2) >> 8;
					uint8 green = (uint16)ReadMacInt16(s_pal + 4) >> 8;
					uint8 blue = (uint16)ReadMacInt16(s_pal + 6) >> 8;
					if (VidLocal.luminance_mapping)
						red = green = blue = (red * 0x4ccc + green * 0x970a + blue * 0x1c29) >> 16;
					*d_pal++ = red;
					*d_pal++ = green;
					*d_pal++ = blue;
					s_pal += 8;
				}
			} else {										// Sequential
				d_pal = VidLocal.palette + ReadMacInt16(param + csStart) * 3;
				for (uint32 i=0; i<=count; i++) {
					uint8 red = (uint16)ReadMacInt16(s_pal + 2) >> 8;
					uint8 green = (uint16)ReadMacInt16(s_pal + 4) >> 8;
					uint8 blue = (uint16)ReadMacInt16(s_pal + 6) >> 8;
					if (VidLocal.luminance_mapping)
						red = green = blue = (red * 0x4ccc + green * 0x970a + blue * 0x1c29) >> 16;
					*d_pal++ = red;
					*d_pal++ = green;
					*d_pal++ = blue;
					s_pal += 8;
				}
			}
			video_set_palette(VidLocal.palette);
			return noErr;
		}

		case cscSetGamma:		// Set gamma table
			D(bug(" SetGamma\n"));
#ifdef __MORPHOS__
			return morphos_set_gamma(ReadMacInt32(param + csGTable));
#else
			return noErr;
#endif

		case cscGrayPage: {		// Fill page with dithered gray pattern
			D(bug(" GrayPage %d\n", ReadMacInt16(param + csPage)));
			if (ReadMacInt16(param + csPage))
				return paramErr;

			uint32 pattern[6] = {
				0xaaaaaaaa,		// 1 bpp
				0xcccccccc,		// 2 bpp
				0xf0f0f0f0,		// 4 bpp
				0xff00ff00,		// 8 bpp
				0xffff0000,		// 16 bpp
				0xffffffff		// 32 bpp
			};
			uint32 p = VidLocal.desc->mac_frame_base;
			uint32 pat = pattern[VidLocal.desc->mode];
			for (uint32 y=0; y<VidLocal.desc->y; y++) {
				for (uint32 x=0; x<VidLocal.desc->bytes_per_row; x+=4) {
					WriteMacInt32(p + x, pat);
					if (VidLocal.desc->mode == VMODE_32BIT)
						pat = ~pat;
				}
				p += VidLocal.desc->bytes_per_row;
				pat = ~pat;
			}
			return noErr;
		}

		case cscSetGray:		// Enable/disable luminance mapping
			D(bug(" SetGray %02x\n", ReadMacInt8(param + csMode)));
			VidLocal.luminance_mapping = ReadMacInt8(param + csMode);
			return noErr;

#ifdef __MORPHOS__
		case cscSetDefaultMode: {	// Set preferred color depth
			uint16 mode = ReadMacInt8(param + csMode);
			D(bug(" SetDefaultMode %02x\n", mode));
			if (morphos_video_mode_for_apple_mode(mode) < 0)
				return paramErr;
			preferred_apple_mode = mode;
			return noErr;
		}
#endif

		case cscSwitchMode:		// Switch video mode
			D(bug(" SwitchMode %04x, %08lx\n", ReadMacInt16(param + csMode), ReadMacInt32(param + csData)));
#ifdef __MORPHOS__
			if (ReadMacInt32(param + csData) != 0x80)
				return paramErr;
			return morphos_switch_mode(ReadMacInt16(param + csMode), param);
#else
			WriteMacInt32(param + csBaseAddr, VidLocal.desc->mac_frame_base);
			return noErr;
#endif

		case cscSetInterrupt:	// Enable/disable VBL
			D(bug(" SetInterrupt %02x\n", ReadMacInt8(param + csMode)));
			VidLocal.interrupts_enabled = (ReadMacInt8(param + csMode) == 0);
			return noErr;

#ifdef __MORPHOS__
		case cscSavePreferredConfiguration: {
			uint16 mode = ReadMacInt16(param + csMode);
			uint32 id = ReadMacInt32(param + csData);
			D(bug(" SavePreferredConfiguration %04x, %08lx\n", mode, id));
			if (id != 0x80 || morphos_video_mode_for_apple_mode(mode) < 0)
				return paramErr;
			preferred_apple_mode = mode;
			preferred_resolution_id = id;
			return noErr;
		}
#endif

		default:
			printf("WARNING: Unknown VideoDriverControl(%d)\n", code);
			return controlErr;
	}
}


/*
 *  Driver Status() routine
 */

int16 VideoDriverStatus(uint32 pb, uint32 dce)
{
	uint16 code = ReadMacInt16(pb + csCode);
	uint32 param = ReadMacInt32(pb + csParam);
	D(bug("VideoDriverStatus %d\n", code));
	switch (code) {

		case cscGetPageCnt:			// Get number of pages
			D(bug(" GetPageCnt\n"));
			WriteMacInt16(param + csPage, 1);
			return noErr;

		case cscGetPageBase:		// Get page base address
			D(bug(" GetPageBase\n"));
			WriteMacInt32(param + csBaseAddr, VidLocal.desc->mac_frame_base);
			return noErr;

		case cscGetGray:			// Get luminance mapping flag
			D(bug(" GetGray\n"));
			WriteMacInt8(param + csMode, VidLocal.luminance_mapping ? 1 : 0);
			return noErr;

		case cscGetInterrupt:		// Get interrupt disable flag
			D(bug(" GetInterrupt\n"));
			WriteMacInt8(param + csMode, VidLocal.interrupts_enabled ? 0 : 1);
			return noErr;

#ifdef __MORPHOS__
		case cscGetGamma:
			// This driver currently uses the system/default gamma ramp.
			// A NULL table is a valid way to report that no private gamma table is installed.
			D(bug(" GetGamma -> default\n"));
			WriteMacInt32(param + csGTable, 0);
			return noErr;
#endif

		case cscGetDefaultMode:		// Get default video mode
			D(bug(" GetDefaultMode\n"));
#ifdef __MORPHOS__
			WriteMacInt8(param + csMode, preferred_apple_mode);
#else
			WriteMacInt8(param + csMode, 0x80);
#endif
			return noErr;

		case cscGetCurMode:			// Get current video mode
			D(bug(" GetCurMode\n"));
#ifdef __MORPHOS__
			WriteMacInt16(param + csMode, current_apple_mode);
#else
			WriteMacInt16(param + csMode, 0x80);
#endif
			WriteMacInt32(param + csData, 0x80);
			WriteMacInt16(param + csPage, 0);
			WriteMacInt32(param + csBaseAddr, VidLocal.desc->mac_frame_base);
			return noErr;

		case cscGetConnection:		// Get monitor information
			D(bug(" GetConnection\n"));
			WriteMacInt16(param + csDisplayType, 6);		// 21" Multiscan
			WriteMacInt8(param + csConnectTaggedType, 6);
			WriteMacInt8(param + csConnectTaggedData, 0x23);
			WriteMacInt32(param + csConnectFlags, 0x03);	// All modes valid and safe
			WriteMacInt32(param + csDisplayComponent, 0);
			return noErr;

		case cscGetModeTiming:		// Get video timing for mode
			D(bug(" GetModeTiming mode %08lx\n", ReadMacInt32(param + csTimingMode)));
			WriteMacInt32(param + csTimingFormat, FOURCC('d', 'e', 'c', 'l'));
			WriteMacInt32(param + csTimingData, 220);		// 21" Multiscan
			WriteMacInt32(param + csTimingFlags, 0x0f);		// Mode valid, safe, default and shown in Monitors panel
			return noErr;

		case cscGetModeBaseAddress:	// Get frame buffer base address
			D(bug(" GetModeBaseAddress\n"));
			WriteMacInt32(param + csBaseAddr, VidLocal.desc->mac_frame_base);	// Base address of video RAM for the current DisplayModeID and relative bit depth
			return noErr;

#ifdef __MORPHOS__
		case cscGetPreferredConfiguration:
			D(bug(" GetPreferredConfiguration -> %04x/%08lx\n", preferred_apple_mode, preferred_resolution_id));
			WriteMacInt16(param + csMode, preferred_apple_mode);
			WriteMacInt32(param + csData, preferred_resolution_id);
			return noErr;

		case cscGetVideoParameters: {
			uint32 id = ReadMacInt32(param + csDisplayModeID);
			uint16 apple_mode = ReadMacInt16(param + csDepthMode);
			int mode = morphos_video_mode_for_apple_mode(apple_mode);
			D(bug(" GetVideoParameters %04x/%08lx\n", apple_mode, id));
			if (id != 0x80 || mode < 0)
				return paramErr;

			uint32 vp = ReadMacInt32(param + csVPBlockPtr);
			if (!vp)
				return paramErr;

			uint32 bytes = mode == VMODE_8BIT ? 1 : mode == VMODE_16BIT ? 2 : 4;
			uint32 row_bytes = (VidLocal.desc->x * bytes + 31) & ~31;
			WriteMacInt32(vp + vpBaseOffset, 0);
			WriteMacInt16(vp + vpRowBytes, row_bytes);
			WriteMacInt16(vp + vpBounds + 0, 0);
			WriteMacInt16(vp + vpBounds + 2, 0);
			WriteMacInt16(vp + vpBounds + 4, VidLocal.desc->y);
			WriteMacInt16(vp + vpBounds + 6, VidLocal.desc->x);
			WriteMacInt16(vp + vpVersion, 0);
			WriteMacInt16(vp + vpPackType, 0);
			WriteMacInt32(vp + vpPackSize, 0);
			WriteMacInt32(vp + vpHRes, 0x00480000);
			WriteMacInt32(vp + vpVRes, 0x00480000);

			if (mode == VMODE_8BIT) {
				WriteMacInt16(vp + vpPixelType, 0);
				WriteMacInt16(vp + vpPixelSize, 8);
				WriteMacInt16(vp + vpCmpCount, 1);
				WriteMacInt16(vp + vpCmpSize, 8);
				WriteMacInt32(param + csDeviceType, 0);
			} else if (mode == VMODE_16BIT) {
				WriteMacInt16(vp + vpPixelType, 16);
				WriteMacInt16(vp + vpPixelSize, 16);
				WriteMacInt16(vp + vpCmpCount, 3);
				WriteMacInt16(vp + vpCmpSize, 5);
				WriteMacInt32(param + csDeviceType, 2);
			} else {
				WriteMacInt16(vp + vpPixelType, 16);
				WriteMacInt16(vp + vpPixelSize, 32);
				WriteMacInt16(vp + vpCmpCount, 3);
				WriteMacInt16(vp + vpCmpSize, 8);
				WriteMacInt32(param + csDeviceType, 2);
			}
			WriteMacInt32(vp + vpPlaneBytes, 0);
			WriteMacInt32(param + csPageCount, 1);
			return noErr;
		}
#endif

		case cscGetMode:		// REQUIRED for MacsBug
			D(bug(" GetMode\n"));
#ifdef __MORPHOS__
			WriteMacInt16(param + csPageMode, current_apple_mode);
#else
			WriteMacInt16(param + csPageMode, 0x80);
#endif
			WriteMacInt32(param + csPageData, 0x80);	// Unused
			WriteMacInt16(param + csPagePage, 0);	// Current display page
			WriteMacInt32(param + csPageBaseAddr, VidLocal.desc->mac_frame_base);
			return noErr;

		default:
			printf("WARNING: Unknown VideoDriverStatus(%d)\n", code);
			return statusErr;
	}
}
