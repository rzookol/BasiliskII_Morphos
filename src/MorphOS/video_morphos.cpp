/*
 *  video_morphos.cpp - Video/graphics emulation, MorphOS specific stuff
 *
 *  Basilisk II (C) 1997-2001 Christian Bauer
 *                  2005-2008 Ilkka Lehtoranta
 *  MorphOS 1.3 graphics extensions: BasiliskII/BeOS-style classic Mac
 *  hardware-pointer tracking, dirty refresh, CGXVideo overlay and isolated
 *  host AltiVec conversion.
 */

#include <intuition/intuition.h>
#include <intuition/pointerclass.h>
#include <intuition/monitorclass.h>
#include <graphics/rastport.h>
#include <graphics/gfx.h>
#include <graphics/modeid.h>
#include <cybergraphx/cgxvideo.h>
#include <cybergraphx/cybergraphics.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <libraries/iffparse.h>

#include <proto/alib.h>
#include <proto/cgxvideo.h>
#include <proto/cybergraphics.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "macos_util.h"
#include "adb.h"
#include "prefs.h"
#include "user_strings.h"
#include "video.h"
#include "altivec_morphos.h"

#define DEBUG 0
#include "debug.h"
#include "guithread.h"
#include "mui.h"

// Global configuration/state
static bool is_classic, is_fullscreen;
static bool use_gfxaccel, use_overlay, use_hardware_cursor, use_altivec;
static bool altivec_active;
static int display_width, display_height;
static ULONG palette[256];
static volatile ULONG palette_changed = 0;
static volatile ULONG mode_changed = 0;

struct Screen *MainScreen;
ULONG quitflag = 0;
struct Library *CGXVideoBase = NULL;

// Keep host-window tracking outside Display_Data.  This deliberately keeps the
// custom-class instance ABI identical to r16 and older incremental builds.
// There is only one Basilisk display object at a time.
static bool overlay_host_geometry_valid = false;
static LONG overlay_host_left_cache = 0, overlay_host_top_cache = 0;
static ULONG overlay_window_width_cache = 0, overlay_window_height_cache = 0;

// MUI handler/timer lifetime state.  Teardown must stop callbacks before any
// framebuffer/VLayer storage is released.
static bool display_input_handler_added = false;
static bool display_event_handler_added = false;
static bool display_timer_pending = false;
static bool display_shown = false;

// Cache the real Intuition window only while MUIM_Show has established it.
// During MUI teardown _window(obj) may already be poisoned (e.g. 0xdeadbeef),
// so MUIM_Hide/OM_DISPOSE must not rediscover the Window through the dying
// MUI object.
static struct Window *display_host_window = NULL;

// Host-side damage/expose is independent from guest framebuffer dirtiness.
// Keep this outside Display_Data to preserve the custom-class instance ABI.
static bool display_host_refresh_pending = false;

// Fullscreen gamma state. Intuition does not copy SA_Gamma* arrays, so these
// buffers must remain valid for the lifetime of MainScreen.
static bool gamma_control_active = false;
static uint8 system_gamma_red[256], system_gamma_green[256], system_gamma_blue[256];
static uint8 mac_gamma_red[256], mac_gamma_green[256], mac_gamma_blue[256];
static uint8 applied_gamma_red[256], applied_gamma_green[256], applied_gamma_blue[256];

static void gamma_identity(uint8 *r, uint8 *g, uint8 *b)
{
	for (int i = 0; i < 256; ++i)
		r[i] = g[i] = b[i] = (uint8)i;
}

static void gamma_apply(void)
{
	if (!MainScreen || !gamma_control_active)
		return;

	for (int i = 0; i < 256; ++i) {
		applied_gamma_red[i] = system_gamma_red[mac_gamma_red[i]];
		applied_gamma_green[i] = system_gamma_green[mac_gamma_green[i]];
		applied_gamma_blue[i] = system_gamma_blue[mac_gamma_blue[i]];
	}

	SetAttrs(MainScreen,
		SA_GammaRed, (ULONG)applied_gamma_red,
		SA_GammaGreen, (ULONG)applied_gamma_green,
		SA_GammaBlue, (ULONG)applied_gamma_blue,
		TAG_DONE);
}

static void gamma_init_screen(void)
{
	APTR monitor = NULL;
	ULONG supported = FALSE;

	gamma_control_active = false;
	gamma_identity(system_gamma_red, system_gamma_green, system_gamma_blue);
	gamma_identity(mac_gamma_red, mac_gamma_green, mac_gamma_blue);

	if (!MainScreen)
		return;
	if (!GetAttr(SA_MonitorObject, MainScreen, (ULONG *)&monitor) || !monitor)
		return;
	if (!GetAttr(MA_GammaControl, monitor, &supported) || !supported)
		return;

	gamma_control_active = true;
	// Fullscreen always starts from the MorphOS monitor's system gamma.
	// Arrays were initialized to identity, so unsupported/failed retrieval
	// still has a safe neutral fallback.
	DoMethod((Object *)monitor, MM_GetDefaultGammaTables,
		system_gamma_red, system_gamma_green, system_gamma_blue);
	gamma_apply();
}

extern struct MUI_CustomClass *CL_Display;
extern Object *app;

// MUI window that owns the display.  It is explicitly closed before the
// Application object is disposed so MUIM_Hide runs while the Intuition Window
// is still valid.
static Object *video_window = NULL;

// BasiliskII/BeOS-style classic 68k cursor tracking.
// The emulated Cursor Manager keeps the 16x16 cursor image and mask in the
// low-memory TheCrsr record at 0x844 and mirrors the hotspot at 0x885/0x887.
// Unlike SheepShaver, BasiliskII does not need to advertise the Display
// Manager hardware-cursor API to obtain this data.  The MUI task samples it
// at video refresh time and installs an Intuition pointerclass object.
static ULONG cursor_serial = 1;
static uint8 cursor_data[68] = {16, 1};
static bool cursor_valid = false;
static bool cursor_visible = false;

static bool cursor_sync_classic(void)
{
	if (!use_hardware_cursor || !HasMacStarted())
		return false;

	uint8 local[68] = {16, 1};
	Mac2Host_memcpy(local + 4, 0x844, 64);
	local[2] = ReadMacInt8(0x885);
	local[3] = ReadMacInt8(0x887);

	// r15 keeps one extra HideCursor nesting level solely to suppress guest
	// framebuffer drawing.  Treat -1 as the host-visible neutral state; an
	// application's additional HideCursor moves it to -2 or lower.
	int16 state = (int16)ReadMacInt16(0x8d0);
	bool visible = state >= -1 && ReadMacInt8(0x8d2) == 0;

	if (!cursor_valid || cursor_visible != visible ||
	    memcmp(cursor_data + 2, local + 2, 66) != 0) {
		memcpy(cursor_data, local, sizeof(cursor_data));
		cursor_visible = visible;
		cursor_valid = true;
		++cursor_serial;
		return true;
	}
	return false;
}

static inline ULONG guest_bytes_per_pixel(void)
{
	if (VideoMonitor.mode == VMODE_8BIT) return 1;
	if (VideoMonitor.mode == VMODE_16BIT) return 2;
	return 4;
}

static inline ULONG row_bytes_for_mode(int mode)
{
	ULONG bpp = mode == VMODE_8BIT ? 1 : mode == VMODE_16BIT ? 2 : 4;
	return (display_width * bpp + 31) & ~31UL;
}

static void mark_full_dirty(struct Display_Data *data)
{
	if (!data) return;
	data->dirty_left = 0;
	data->dirty_top = 0;
	data->dirty_right = data->width;
	data->dirty_bottom = data->height;
	data->dirty_valid = 1;
}

// MorphOS -> Mac raw keycode translation table
static const uint8 keycode2mac[0x80] =
{
	0x0a, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1a,
	0x1c, 0x19, 0x1d, 0x1b, 0x18, 0x2a, 0xff, 0x52,
	0x0c, 0x0d, 0x0e, 0x0f, 0x11, 0x10, 0x20, 0x22,
	0x1f, 0x23, 0x21, 0x1e, 0xff, 0x53, 0x54, 0x55,
	0x00, 0x01, 0x02, 0x03, 0x05, 0x04, 0x26, 0x28,
	0x25, 0x29, 0x27, 0x2a, 0xff, 0x56, 0x57, 0x58,
	0x32, 0x06, 0x07, 0x08, 0x09, 0x0b, 0x2d, 0x2e,
	0x2b, 0x2f, 0x2c, 0xff, 0x41, 0x59, 0x5b, 0x5c,
	0x31, 0x33, 0x30, 0x4c, 0x24, 0x35, 0x75, 0x72,
	0x74, 0x79, 0x4e, 0x67, 0x3e, 0x3d, 0x3c, 0x3b,
	0x7a, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64,
	0x65, 0x6d, 0x47, 0x51, 0x4b, 0x43, 0x45, 0x72,
	0x38, 0x38, 0x39, 0x36, 0x3a, 0x3a, 0x37, 0x37,
	0xff, 0xff, 0xff, 0xff, 0x32, 0xff, 0x7f, 0x6f,
	0x73, 0x77, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0x3e, 0x3d, 0xff, 0xff, 0xff, 0xff
};

static void cursor_detach(struct Display_Data *data, struct Window *window)
{
	// Leaving the Mac display must only detach the custom pointer from the
	// Intuition window.  Keep the pointerclass object and its serial alive so
	// it can be re-attached immediately on re-entry even when TheCrsr itself
	// has not changed.  r14 destroyed the object here and then believed the
	// same serial was still installed, so the Mac cursor only returned after a
	// later SetCursor()/redraw changed TheCrsr.
	if (!data) return;
	if (window) {
		// Clear any legacy SetPointer() blank pointer first, then explicitly
		// restore the Preferences pointer associated with the window.  WA_Pointer
		// objects are installed with SetWindowPointerA(), so ClearPointer() alone
		// is not a reliable way to detach them.
		ClearPointer(window);
		struct TagItem tags[2] = {{WA_Pointer,0},{TAG_DONE,0}};
		SetWindowPointerA(window,tags);
	}
	data->current_pointer = NULL;
}

static void cursor_release(struct Display_Data *data, struct Window *window)
{
	if (!data) return;
	cursor_detach(data, window);
	if (data->cursor_object) {
		DisposeObject(data->cursor_object);
		data->cursor_object = NULL;
	}
	if (data->cursor_bitmap) {
		FreeBitMap(data->cursor_bitmap);
		data->cursor_bitmap = NULL;
	}
	// Force cursor_apply() to rebuild/reinstall after a real hide/dispose.
	data->cursor_serial = 0;
}

static bool cursor_apply(Object *obj, struct Display_Data *data)
{
	if (!obj || !data || !use_hardware_cursor || !cursor_valid) return false;
	ULONG serial = cursor_serial;
	if (serial == data->cursor_serial) return true;
	struct Window *window = _window(obj);
	if (!window) return false;

	uint8 local[68];
	memcpy(local, cursor_data, sizeof(local));

	if (!cursor_visible) {
		static const UWORD blank_pointer[] = {0,0,0,0};
		SetPointer(window, (UWORD *)blank_pointer, 1, 16, 0, 0);
		data->cursor_serial = serial;
		return true;
	}

	uint32 argb[16 * 16];
	const uint8 *image = local + 4;
	const uint8 *mask = local + 36;
	for (int y=0; y<16; ++y) {
		uint16 ib = ((uint16)image[y*2] << 8) | image[y*2+1];
		uint16 mb = ((uint16)mask[y*2] << 8) | mask[y*2+1];
		for (int x=0; x<16; ++x) {
			uint16 bit = (uint16)(0x8000U >> x);
			argb[y*16+x] = (mb & bit) ? ((ib & bit) ? 0xff000000UL : 0xffffffffUL) : 0;
		}
	}

	struct BitMap *bitmap = AllocBitMap(16,16,32,BMF_CLEAR|BMF_MINPLANES,
		window->RPort ? window->RPort->BitMap : NULL);
	if (!bitmap) {
		static const UWORD blank_pointer[] = {0,0,0,0};
		SetPointer(window, (UWORD *)blank_pointer, 1, 16, 0, 0);
		return false;
	}
	struct RastPort rp;
	InitRastPort(&rp);
	rp.BitMap = bitmap;
	WritePixelArray(argb,0,0,16*sizeof(uint32),&rp,0,0,16,16,RECTFMT_ARGB);

	Object *pointer = (Object *)NewObject(NULL, POINTERCLASS,
		POINTERA_BitMap, (ULONG)bitmap,
		POINTERA_XOffset, -(LONG)local[2],
		POINTERA_YOffset, -(LONG)local[3],
		POINTERA_XResolution, POINTERXRESN_SCREENRES,
		POINTERA_YResolution, POINTERYRESN_SCREENRES,
		TAG_DONE);
	if (!pointer) {
		static const UWORD blank_pointer[] = {0,0,0,0};
		FreeBitMap(bitmap);
		SetPointer(window, (UWORD *)blank_pointer, 1, 16, 0, 0);
		return false;
	}
	struct TagItem tags[2] = {{WA_Pointer,(ULONG)pointer},{TAG_DONE,0}};
	SetWindowPointerA(window,tags);
	if (data->cursor_object) DisposeObject(data->cursor_object);
	if (data->cursor_bitmap) FreeBitMap(data->cursor_bitmap);
	data->cursor_object = pointer;
	data->cursor_bitmap = bitmap;
	data->cursor_serial = serial;
	return true;
}

static void cursor_reassert(Object *obj, struct Display_Data *data)
{
	if (!obj || !data || !use_hardware_cursor || !cursor_valid) return;
	struct Window *window = _window(obj);
	if (!window) return;
	if (!cursor_visible) {
		static const UWORD blank_pointer[] = {0,0,0,0};
		SetPointer(window, (UWORD *)blank_pointer, 1, 16, 0, 0);
	} else if (data->cursor_object) {
		struct TagItem tags[2] = {{WA_Pointer,(ULONG)data->cursor_object},{TAG_DONE,0}};
		SetWindowPointerA(window,tags);
	}
}

static void overlay_destroy(struct Display_Data *data)
{
	if (!data || !data->VLayer) return;
	if (data->overlay_active) DetachVLayer(data->VLayer);
	DeleteVLayerHandle(data->VLayer);
	data->VLayer = NULL;
	data->overlay_active = 0;
	data->overlay_geometry_valid = 0;
	overlay_host_geometry_valid = false;
}

/* Fit the Mac framebuffer into the MUI display object without distorting it.
 * The returned local rectangle is relative to the MUI object; the window
 * rectangle is relative to the Intuition window's inner area and is suitable
 * for CGXVideo VOA_*Indent tags. */
static bool overlay_fit_rect(Object *obj, struct Display_Data *data,
	LONG *local_left, LONG *local_top, LONG *dest_width, LONG *dest_height,
	LONG *window_left, LONG *window_top, LONG *window_right, LONG *window_bottom)
{
	if (!obj || !data) return false;
	struct Window *window = _window(obj);
	if (!window) return false;

	LONG aw = (LONG)_mwidth(obj);
	LONG ah = (LONG)_mheight(obj);
	if (aw < 1 || ah < 1 || data->width < 1 || data->height < 1) return false;

	LONG dw = aw, dh = ah;
	if ((UQUAD)(ULONG)aw * (UQUAD)data->height > (UQUAD)(ULONG)ah * (UQUAD)data->width) {
		dh = ah;
		dw = (LONG)(((UQUAD)(ULONG)ah * (UQUAD)data->width) / (UQUAD)data->height);
	} else {
		dw = aw;
		dh = (LONG)(((UQUAD)(ULONG)aw * (UQUAD)data->height) / (UQUAD)data->width);
	}
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;
	if (dw > aw) dw = aw;
	if (dh > ah) dh = ah;

	LONG ll = (aw - dw) / 2;
	LONG lt = (ah - dh) / 2;
	LONG inner_width = window->Width - window->BorderLeft - window->BorderRight;
	LONG inner_height = window->Height - window->BorderTop - window->BorderBottom;
	LONG wl = (LONG)_mleft(obj) - window->BorderLeft + ll;
	LONG wt = (LONG)_mtop(obj) - window->BorderTop + lt;
	LONG wr = inner_width - wl - dw;
	LONG wb = inner_height - wt - dh;
	if (wl < 0) wl = 0;
	if (wt < 0) wt = 0;
	if (wr < 0) wr = 0;
	if (wb < 0) wb = 0;

	if (local_left) *local_left = ll;
	if (local_top) *local_top = lt;
	if (dest_width) *dest_width = dw;
	if (dest_height) *dest_height = dh;
	if (window_left) *window_left = wl;
	if (window_top) *window_top = wt;
	if (window_right) *window_right = wr;
	if (window_bottom) *window_bottom = wb;
	return true;
}

static bool overlay_geometry(Object *obj, struct Display_Data *data)
{
	if (!obj || !data || !data->overlay_active || !data->VLayer) return false;
	struct Window *window = _window(obj);
	if (!window) return false;

	LONG ll, lt, dw, dh, wl, wt, wr, wb;
	if (!overlay_fit_rect(obj, data, &ll, &lt, &dw, &dh, &wl, &wt, &wr, &wb)) return false;

	/* CGXVideo attaches the VLayer to an Intuition window, but on older
	 * MorphOS/CGX combinations a moved window does not necessarily cause the
	 * hardware overlay position to be recomputed.  The indent rectangle is
	 * unchanged by a pure window move, so the old cache used to suppress the
	 * SetVLayerAttrTags() call exactly when it was needed most.  Track the
	 * absolute host position too (including a movable screen) and reassert the
	 * VLayer geometry whenever it changes. */
	LONG screen_left = window->WScreen ? window->WScreen->LeftEdge : 0;
	LONG screen_top = window->WScreen ? window->WScreen->TopEdge : 0;
	LONG host_left = screen_left + window->LeftEdge;
	LONG host_top = screen_top + window->TopEdge;
	bool window_moved = !overlay_host_geometry_valid ||
		overlay_host_left_cache != host_left || overlay_host_top_cache != host_top;
	bool window_resized = !overlay_host_geometry_valid ||
		overlay_window_width_cache != (ULONG)window->Width ||
		overlay_window_height_cache != (ULONG)window->Height;
	bool rect_changed = !data->overlay_geometry_valid ||
		data->overlay_dest_left != wl || data->overlay_dest_top != wt ||
		data->overlay_dest_width != (ULONG)dw || data->overlay_dest_height != (ULONG)dh;

	if (!window_moved && !window_resized && !rect_changed)
		return false;

	SetVLayerAttrTags(data->VLayer,
		VOA_LeftIndent, (ULONG)wl, VOA_RightIndent, (ULONG)wr,
		VOA_TopIndent, (ULONG)wt, VOA_BottomIndent, (ULONG)wb,
		TAG_DONE);
	data->overlay_dest_left = wl;
	data->overlay_dest_top = wt;
	data->overlay_dest_width = (ULONG)dw;
	data->overlay_dest_height = (ULONG)dh;
	overlay_host_left_cache = host_left;
	overlay_host_top_cache = host_top;
	overlay_window_width_cache = (ULONG)window->Width;
	overlay_window_height_cache = (ULONG)window->Height;
	overlay_host_geometry_valid = true;
	data->overlay_geometry_valid = 1;

	/* Refill the next VLayer buffer after a move/resize.  Some overlay drivers
	 * invalidate or expose stale VRAM while the hardware window is relocated;
	 * a full present prevents that memory from becoming visible. */
	if (window_moved || window_resized)
		mark_full_dirty(data);
	return true;
}

static bool overlay_create(Object *obj, struct Display_Data *data)
{
	if (!use_overlay || is_fullscreen || !CGXVideoBase || !obj || !data) return false;
	struct Window *window = _window(obj);
	if (!window || !window->WScreen) return false;
	ULONG error = 0;
	data->VLayer = CreateVLayerHandleTags(window->WScreen,
		VOA_SrcType, SRCFMT_RGB16,
		VOA_SrcWidth, data->width,
		VOA_SrcHeight, data->height,
		VOA_DoubleBuffer, TRUE,
		VOA_UseFilter, TRUE,
		VOA_Error, (ULONG)&error,
		TAG_DONE);
	if (!data->VLayer) return false;
	if (AttachVLayerTags(data->VLayer, window,
		VOA_LeftIndent, 0, VOA_RightIndent, 0,
		VOA_TopIndent, 0, VOA_BottomIndent, 0,
		TAG_DONE) != 0) {
		overlay_destroy(data);
		return false;
	}
	data->overlay_active = 1;
	data->overlay_geometry_valid = 0;
	overlay_host_geometry_valid = false;
	overlay_geometry(obj,data);
	return true;
}

static inline uint16 rgb_to_rgb16pc(uint8 r, uint8 g, uint8 b)
{
	uint16 r5=r>>3, g6=g>>2, b5=b>>3;
	return (uint16)(((g6 & 7) << 13) | (b5 << 8) | (r5 << 3) | ((g6 >> 3) & 7));
}

static inline void store_rgb16pc(uint8 *d, uint8 r, uint8 g, uint8 b)
{
	uint16 p=rgb_to_rgb16pc(r,g,b);
	d[0]=p>>8; d[1]=p;
}

static void overlay_convert_full(struct Display_Data *data, uint8 *dst, ULONG modulo)
{
	const uint8 *srcbase=(const uint8 *)data->pixelarray;
	ULONG srcbpr=VideoMonitor.bytes_per_row;
	for (ULONG y=0; y<data->height; ++y) {
		const uint8 *src=srcbase+y*srcbpr;
		uint8 *d=dst+y*modulo;
		if (VideoMonitor.mode==VMODE_8BIT) {
			for (ULONG x=0;x<data->width;++x) {
				ULONG c=palette[src[x]];
				store_rgb16pc(d+x*2,(uint8)(c>>16),(uint8)(c>>8),(uint8)c);
			}
		} else if (VideoMonitor.mode==VMODE_16BIT) {
			ULONG x=0;
			if (altivec_active) x=BasiliskMorphOSAltiVecRGB555ToRGB16PC(src,d,data->width);
			for (;x<data->width;++x) {
				uint16 p=((uint16)src[x*2]<<8)|src[x*2+1];
				uint8 r5=(p>>10)&31,g5=(p>>5)&31,b5=p&31;
				store_rgb16pc(d+x*2,(r5<<3)|(r5>>2),(g5<<3)|(g5>>2),(b5<<3)|(b5>>2));
			}
		} else {
			for (ULONG x=0;x<data->width;++x)
				store_rgb16pc(d+x*2,src[x*4+1],src[x*4+2],src[x*4+3]);
		}
	}
}

static bool overlay_present(struct Display_Data *data)
{
	if (!data || !data->overlay_active || !data->VLayer) return false;
	if (!LockVLayer(data->VLayer)) return false;
	uint8 *base=(uint8 *)GetVLayerAttr(data->VLayer,VOA_BaseAddress);
	ULONG modulo=GetVLayerAttr(data->VLayer,VOA_Modulo);
	if (!modulo) modulo=data->width*2;
	if (!base) { UnlockVLayer(data->VLayer); return false; }
	// Full conversion is intentional: double-buffered VLayers do not guarantee
	// that the next writable buffer contains the previous frame.
	overlay_convert_full(data,base,modulo);
	UnlockVLayer(data->VLayer);
	WaitTOF();
	SwapVLayerBuffer(data->VLayer);
	return true;
}

static void update_draw_mode(struct Display_Data *data)
{
	if (is_classic) data->drawmode=DRAW_CLASSIC;
	else if (VideoMonitor.mode==VMODE_8BIT) data->drawmode=DRAW_LUT_8BIT;
	else data->drawmode=DRAW_PIXELARRAY;
	data->bytes_per_row=VideoMonitor.bytes_per_row;
	data->last_mode=VideoMonitor.mode;
	data->last_guest_bpr=VideoMonitor.bytes_per_row;
	if (data->shadow_buffer) memset(data->shadow_buffer,0xa5,data->shadow_size);
	mark_full_dirty(data);
}

static bool scan_dirty(struct Display_Data *data)
{
	if (!data || !data->shadow_buffer || !data->pixelarray) return true;
	const ULONG bpp=guest_bytes_per_pixel();
	const ULONG meaningful=data->width*bpp;
	const ULONG bpr=VideoMonitor.bytes_per_row;
	uint8 *src=(uint8 *)data->pixelarray;
	uint8 *shadow=(uint8 *)data->shadow_buffer;
	bool changed=false;
	ULONG left=data->width,right=0,top=data->height,bottom=0;
	for (ULONG y=0;y<data->height;++y) {
		uint8 *s=src+y*bpr;
		uint8 *d=shadow+y*bpr;
		if (memcmp(s,d,meaningful)==0) continue;
		ULONG first=0,last=meaningful-1;
		while (first<meaningful && s[first]==d[first]) ++first;
		while (last>first && s[last]==d[last]) --last;
		ULONG px0=first/bpp, px1=last/bpp+1;
		if (px0<left) left=px0;
		if (px1>right) right=px1;
		if (y<top) top=y;
		if (y+1>bottom) bottom=y+1;
		memcpy(d,s,bpr);
		changed=true;
	}
	if (changed) {
		if (!data->dirty_valid) {
			data->dirty_left=left; data->dirty_top=top; data->dirty_right=right; data->dirty_bottom=bottom;
		} else {
			if (left<data->dirty_left) data->dirty_left=left;
			if (top<data->dirty_top) data->dirty_top=top;
			if (right>data->dirty_right) data->dirty_right=right;
			if (bottom>data->dirty_bottom) data->dirty_bottom=bottom;
		}
		data->dirty_valid=1;
	}
	return changed;
}

ULONG DisplayDataSize(void)
{
	return sizeof(struct Display_Data);
}

static ULONG mNew(struct IClass *cl, Object *obj, Msg msg)
{
	obj=(Object *)DoSuperMethodA(cl,obj,msg);
	if (!obj) return 0;
	struct Display_Data *data=(struct Display_Data *)INST_DATA(cl,obj);
	memset(data,0,sizeof(*data));
	display_input_handler_added = false;
	display_event_handler_added = false;
	display_timer_pending = false;
	display_shown = false;
	display_host_window = NULL;
	display_host_refresh_pending = false;
	overlay_host_geometry_valid = false;
	data->current_pointer=(UWORD *)-1;
	data->width=display_width; data->height=display_height; data->last_mode=-1;
	LONG fs=PrefsFindInt32("frameskip")+1; if (fs<1) fs=1; if (fs>10) fs=10; data->frameskip=fs;

	if (is_classic) {
		data->pix_array_size=(512*(342+2))/8;
		data->pixelarray=AllocMem(data->pix_array_size,MEMF_CLEAR);
		VideoMonitor.bytes_per_row=512/8;
		data->alloc_bytes_per_row=512/8;
	} else {
		data->alloc_bytes_per_row=row_bytes_for_mode(VMODE_32BIT);
		UQUAD alloc=(UQUAD)data->alloc_bytes_per_row*((UQUAD)data->height+2);
		if (alloc<=0xffffffffULL) {
			data->pix_array_size=(ULONG)alloc;
			data->pixelarray=AllocMemAligned(data->pix_array_size,MEMF_CLEAR,32,0);
			data->conversion_size=data->alloc_bytes_per_row*data->height;
			data->conversion_buffer=AllocMemAligned(data->conversion_size,MEMF_CLEAR,32,0);
			data->shadow_size=data->pix_array_size;
			data->shadow_buffer=AllocMemAligned(data->shadow_size,MEMF_CLEAR,32,0);
		}
		VideoMonitor.bytes_per_row=row_bytes_for_mode(VideoMonitor.mode);
	}
	if (!data->pixelarray || (!is_classic && (!data->conversion_buffer || !data->shadow_buffer))) {
		CoerceMethod(cl,obj,OM_DISPOSE); return 0;
	}
	VideoMonitor.mac_frame_base=(uint32)Host2MacAddr((uint8 *)data->pixelarray);
	update_draw_mode(data);

	data->timerport=CreateMsgPort();
	if (data->timerport) data->timer_io=(struct timerequest *)CreateIORequest(data->timerport,sizeof(struct timerequest));
	if (!data->timer_io || OpenDevice("timer.device",UNIT_VBLANK,(struct IORequest *)data->timer_io,0)!=0) {
		CoerceMethod(cl,obj,OM_DISPOSE); return 0;
	}
	data->timer_ok=1;
	data->ihnode.ihn_Object=obj;
	data->ihnode.ihn_Signals=1<<data->timerport->mp_SigBit;
	data->ihnode.ihn_Method=MM_Display_Update;
	data->ehnode.ehn_Object=obj;
	data->ehnode.ehn_Class=cl;
	data->ehnode.ehn_Events=IDCMP_MOUSEBUTTONS|IDCMP_MOUSEMOVE|IDCMP_RAWKEY|IDCMP_CHANGEWINDOW|IDCMP_NEWSIZE|IDCMP_REFRESHWINDOW;
	return (ULONG)obj;
}

static VOID StopTimer(struct Display_Data *data)
{
	if (!data || !data->timer_io) {
		display_timer_pending = false;
		return;
	}
	if (display_timer_pending) {
		if (!CheckIO((struct IORequest *)data->timer_io))
			AbortIO((struct IORequest *)data->timer_io);
		WaitIO((struct IORequest *)data->timer_io);
	}
	/* Drain a completed request message, if one is still queued. */
	if (data->timerport)
		while (GetMsg(data->timerport)) {}
	display_timer_pending = false;
}

static VOID RemoveDisplayHandlers(Object *obj, struct Display_Data *data)
{
	if (!obj || !data) return;
	if (display_input_handler_added) {
		Object *a = _app(obj);
		if (a) DoMethod(a,MUIM_Application_RemInputHandler,&data->ihnode);
		display_input_handler_added = false;
	}
	if (display_event_handler_added) {
		Object *w = _win(obj);
		if (w) DoMethod(w,MUIM_Window_RemEventHandler,&data->ehnode);
		display_event_handler_added = false;
	}
}

static VOID mDispose(Object *obj, struct Display_Data *data)
{
	if (!data) return;

	/* OM_DISPOSE may run after MUI has already invalidated the window link.
	 * Never call _window(obj), ClearPointer() or SetWindowPointerA() here: a
	 * poisoned Window pointer is exactly what caused the shutdown crash.
	 * Normal shown objects are detached/released by MUIM_Hide while the cached
	 * Intuition Window is still valid.  If MUI ever skips MUIM_Hide, abandon
	 * the tiny cursor resources rather than freeing an object Intuition may
	 * still reference; the process is terminating and MorphOS will reclaim it. */
	bool cursor_may_still_be_attached = display_host_window != NULL;
	display_shown = false;
	/* Normal teardown already removed handlers in MUIM_Hide.  If Hide was
	 * skipped, do not send methods through a MUI window/application that is
	 * itself in OM_DISPOSE; simply make our local callback state inert. */
	display_input_handler_added = false;
	display_event_handler_added = false;
	StopTimer(data);
	overlay_destroy(data);
	if (cursor_may_still_be_attached) {
		data->current_pointer = NULL;
		data->cursor_object = NULL;
		data->cursor_bitmap = NULL;
		data->cursor_serial = 0;
	} else {
		cursor_release(data,NULL);
	}
	display_host_window = NULL;
	display_host_refresh_pending = false;

	if (data->timer_ok && data->timer_io) {
		CloseDevice((struct IORequest *)data->timer_io);
		data->timer_ok=0;
	}
	if (data->timer_io) { DeleteIORequest(data->timer_io); data->timer_io=NULL; }
	if (data->timerport) { DeleteMsgPort(data->timerport); data->timerport=NULL; }

	if (data->pixelarray) { FreeMem(data->pixelarray,data->pix_array_size); data->pixelarray=NULL; }
	if (data->conversion_buffer) { FreeMem(data->conversion_buffer,data->conversion_size); data->conversion_buffer=NULL; }
	if (data->shadow_buffer) { FreeMem(data->shadow_buffer,data->shadow_size); data->shadow_buffer=NULL; }
}

static ULONG mAskMinMax(struct IClass *cl,Object *obj,Msg msg,struct Display_Data *data)
{
	DoSuperMethodA(cl,obj,msg);
	struct MUI_MinMax *mmm=((struct MUIP_AskMinMax *)msg)->MinMaxInfo;
	mmm->MinWidth=data->width; mmm->MinHeight=data->height;
	mmm->DefWidth=data->width; mmm->DefHeight=data->height;
	mmm->MaxWidth=(use_overlay&&!is_fullscreen)?MUI_MAXMAX:data->width;
	mmm->MaxHeight=(use_overlay&&!is_fullscreen)?MUI_MAXMAX:data->height;
	return 0;
}

static VOID StartTimer(struct Display_Data *data)
{
	if (!data || !data->timer_io || !display_shown || display_timer_pending) return;
	data->timer_io->tr_node.io_Command=TR_ADDREQUEST;
	data->timer_io->tr_time.tv_secs=0;
	data->timer_io->tr_time.tv_micro=16667*data->frameskip;
	SendIO((struct IORequest *)data->timer_io);
	display_timer_pending = true;
}

static ULONG mShow(struct IClass *cl,Object *obj,Msg msg,struct Display_Data *data)
{
	ULONG rc=DoSuperMethodA(cl,obj,msg);
	if (!rc) return rc;
	display_host_window = _window(obj);
	display_shown = true;
	if (use_hardware_cursor) {
		cursor_sync_classic();
		if (!cursor_apply(obj,data) && display_host_window) {
			static const UWORD blank_pointer[]={0,0,0,0};
			SetPointer(display_host_window,(UWORD *)blank_pointer,1,16,0,0);
		}
	} else if (display_host_window) {
		static const UWORD blank_pointer[]={0,0,0,0};
		SetPointer(display_host_window,(UWORD *)blank_pointer,1,16,0,0);
	}
	overlay_create(obj,data);
	if (!display_input_handler_added && _app(obj)) {
		DoMethod(_app(obj),MUIM_Application_AddInputHandler,&data->ihnode);
		display_input_handler_added = true;
	}
	if (!display_event_handler_added && _win(obj)) {
		DoMethod(_win(obj),MUIM_Window_AddEventHandler,&data->ehnode);
		display_event_handler_added = true;
	}
	display_host_refresh_pending = false;
	mark_full_dirty(data);
	StartTimer(data);
	return rc;
}

static VOID mHide(Object *obj,struct Display_Data *data)
{
	/* Capture the Window from MUIM_Show instead of asking the MUI object again
	 * during teardown.  _window(obj) can already be poisoned at this point. */
	struct Window *window = display_host_window;
	display_shown = false;
	RemoveDisplayHandlers(obj,data);
	StopTimer(data);
	overlay_destroy(data);
	cursor_release(data,window);
	display_host_window = NULL;
	display_host_refresh_pending = false;
}

static bool map_mouse_to_guest(Object *obj, struct Display_Data *data, LONG win_x, LONG win_y, LONG *guest_x, LONG *guest_y)
{
	if (!obj || !data) return false;
	LONG aw=(LONG)_mwidth(obj), ah=(LONG)_mheight(obj);
	LONG mx=win_x-(LONG)_mleft(obj), my=win_y-(LONG)_mtop(obj);
	if (aw<=0 || ah<=0 || mx<0 || my<0 || mx>=aw || my>=ah) return false;

	if (data->overlay_active) {
		LONG ll,lt,dw,dh;
		if (!overlay_fit_rect(obj,data,&ll,&lt,&dw,&dh,NULL,NULL,NULL,NULL)) return false;
		if (mx<ll || my<lt || mx>=ll+dw || my>=lt+dh) return false;
		mx-=ll; my-=lt;
		mx=(LONG)(((UQUAD)(ULONG)mx*(UQUAD)data->width)/(UQUAD)(ULONG)dw);
		my=(LONG)(((UQUAD)(ULONG)my*(UQUAD)data->height)/(UQUAD)(ULONG)dh);
	}

	if (mx<0) mx=0; if (my<0) my=0;
	if (mx>=(LONG)data->width) mx=data->width-1;
	if (my>=(LONG)data->height) my=data->height-1;
	if (guest_x) *guest_x=mx;
	if (guest_y) *guest_y=my;
	return true;
}

static ULONG mHandleEvent(Object *obj,Msg msg,struct Display_Data *data)
{
	if (!display_shown || !data) return 0;
	struct IntuiMessage *imsg=((struct MUIP_HandleEvent *)msg)->imsg;
	if (!imsg) return 0;
	ULONG code=imsg->Code;
	switch (imsg->Class) {
		case IDCMP_MOUSEMOVE: {
			struct Window *win=_window(obj); if (!win) break;
			LONG mx,my;
			if (!map_mouse_to_guest(obj,data,imsg->MouseX,imsg->MouseY,&mx,&my)) {
				data->mouse_in_display=0;
				if (use_hardware_cursor) cursor_detach(data,win);
				break;
			}
			data->mouse_in_display=1;
			ADBMouseMoved(mx,my);
			if (use_hardware_cursor) { cursor_sync_classic(); cursor_apply(obj,data); cursor_reassert(obj,data); }
			return MUI_EventHandlerRC_Eat;
		}
		case IDCMP_MOUSEBUTTONS: {
			LONG mx,my;
			if (!map_mouse_to_guest(obj,data,imsg->MouseX,imsg->MouseY,&mx,&my)) {
				data->mouse_in_display=0;
				if (use_hardware_cursor && _window(obj)) cursor_detach(data,_window(obj));
				/* Never leave a guest button stuck if it was pressed over the Mac
				 * image and released later over a black aspect-ratio bar. */
				if (code==SELECTUP) ADBMouseUp(0);
				else if (code==MENUUP) ADBMouseUp(1);
				else if (code==MIDDLEUP) ADBMouseUp(2);
				break;
			}
			data->mouse_in_display=1;
			ADBMouseMoved(mx,my);
			if (code==SELECTDOWN) ADBMouseDown(0); else if (code==SELECTUP) ADBMouseUp(0);
			else if (code==MENUDOWN) ADBMouseDown(1); else if (code==MENUUP) ADBMouseUp(1);
			else if (code==MIDDLEDOWN) ADBMouseDown(2); else if (code==MIDDLEUP) ADBMouseUp(2);
			break;
		}
		case IDCMP_CHANGEWINDOW:
		case IDCMP_NEWSIZE:
			/* Do not rely solely on the 60 Hz timer here. Reassert the CGXVideo
			 * geometry as soon as Intuition reports a window move/resize, then
			 * redraw through the normal update path. MADF_DRAWOBJECT is reserved
			 * for host/MUI full repaints so fast guest updates remain partial. */
			if (data->overlay_active) {
				data->overlay_geometry_valid=0;
				overlay_geometry(obj,data);
				mark_full_dirty(data);
				MUI_Redraw(obj,MADF_DRAWUPDATE);
			}
			break;
		case IDCMP_REFRESHWINDOW:
			/* Fast screen updates compare the guest framebuffer against a shadow
			 * buffer. An Intuition expose/damage does not change guest memory, so
			 * scan_dirty() cannot notice it. Remember the host damage explicitly;
			 * the display timer below will request a full repaint on the next tick.
			 * Do not eat the event: MUI still owns BeginRefresh()/EndRefresh(). */
			display_host_refresh_pending = true;
			mark_full_dirty(data);
			break;
		case IDCMP_RAWKEY: {
			ULONG q=imsg->Qualifier; if (q&IEQUALIFIER_REPEAT) break;
			if ((q&(IEQUALIFIER_LALT|IEQUALIFIER_LSHIFT|IEQUALIFIER_CONTROL))==(IEQUALIFIER_LALT|IEQUALIFIER_LSHIFT|IEQUALIFIER_CONTROL) && code==0x5f) { SetInterruptFlag(INTFLAG_NMI); TriggerInterrupt(); break; }
			uint8 mc=keycode2mac[code&0x7f]; if (mc!=0xff) { if (code&IECODE_UP_PREFIX) ADBKeyUp(mc); else ADBKeyDown(mc); }
			break;
		}
	}
	return 0;
}

static ULONG mUpdate(Object *obj,struct Display_Data *data)
{
	if (!display_shown || !data || !data->timerport) return 0;
	if (!GetMsg(data->timerport)) return 0;
	display_timer_pending = false;
	if (mode_changed || data->last_mode!=VideoMonitor.mode || data->last_guest_bpr!=VideoMonitor.bytes_per_row) {
		mode_changed=0; update_draw_mode(data);
	}
	if (palette_changed) { palette_changed=0; mark_full_dirty(data); }
	bool geometry_changed = overlay_geometry(obj,data);
	if (use_hardware_cursor) {
		cursor_sync_classic();
		if (data->mouse_in_display) cursor_apply(obj,data);
	}
	if (display_host_refresh_pending) mark_full_dirty(data);
	if (use_gfxaccel && !is_classic) scan_dirty(data); else mark_full_dirty(data);
	if (data->dirty_valid || geometry_changed) MUI_Redraw(obj,MADF_DRAWUPDATE);
	StartTimer(data);
	return 0;
}

static void convert_16_to_argb(struct Display_Data *data,ULONG left,ULONG top,ULONG right,ULONG bottom)
{
	const uint8 *src=(const uint8 *)data->pixelarray;
	uint8 *dst=(uint8 *)data->conversion_buffer;
	ULONG sbpr=VideoMonitor.bytes_per_row, dbpr=data->alloc_bytes_per_row;
	for (ULONG y=top;y<bottom;++y) for (ULONG x=left;x<right;++x) {
		uint16 p=((uint16)src[y*sbpr+x*2]<<8)|src[y*sbpr+x*2+1];
		uint8 r5=(p>>10)&31,g5=(p>>5)&31,b5=p&31;
		uint8 *d=dst+y*dbpr+x*4;
		d[0]=0xff; d[1]=(r5<<3)|(r5>>2); d[2]=(g5<<3)|(g5>>2); d[3]=(b5<<3)|(b5>>2);
	}
}

static ULONG mDraw(struct IClass *cl,Object *obj,Msg msg,struct Display_Data *data)
{
	/* The superclass must run first; MUI normalizes MUIP_Draw::flags there. */
	DoSuperMethodA(cl,obj,msg);
	if (!display_shown || !data) return 0;

	struct MUIP_Draw *dmsg=(struct MUIP_Draw *)msg;
	if ((dmsg->flags & MADF_DRAWOBJECT) || display_host_refresh_pending)
		mark_full_dirty(data);

	/* The VLayer only covers the aspect-correct destination rectangle.  Paint
	 * the complete MUI display object black first so the uncovered area becomes
	 * deterministic letterbox/pillarbox bars rather than MUI/window background. */
	if (data->overlay_active) {
		overlay_geometry(obj,data);
		struct RastPort *orp=_rp(obj);
		LONG ow=(LONG)_mwidth(obj), oh=(LONG)_mheight(obj);
		if (orp && ow>0 && oh>0)
			FillPixelArray(orp,(UWORD)_mleft(obj),(UWORD)_mtop(obj),(UWORD)ow,(UWORD)oh,0xff000000);
		if (!data->dirty_valid) return 0;
		if (overlay_present(data)) {
			data->dirty_valid=0;
			display_host_refresh_pending=false;
			return 0;
		}
	}

	if (!data->dirty_valid) return 0;
	ULONG l=data->dirty_left,t=data->dirty_top,r=data->dirty_right,b=data->dirty_bottom;
	if (r>data->width) r=data->width; if (b>data->height) b=data->height;
	if (r<=l||b<=t) { data->dirty_valid=0; return 0; }
	struct RastPort *rp=_rp(obj); ULONG ml=_mleft(obj), mt=_mtop(obj);
	if (is_classic) {
		BltTemplate((UBYTE *)data->pixelarray,0,512/8,rp,ml,mt,data->width,data->height);
	} else if (VideoMonitor.mode==VMODE_8BIT) {
		WriteLUTPixelArray(data->pixelarray,l,t,VideoMonitor.bytes_per_row,rp,&palette,ml+l,mt+t,r-l,b-t,CTABFMT_XRGB8);
	} else if (VideoMonitor.mode==VMODE_16BIT) {
		convert_16_to_argb(data,l,t,r,b);
		WritePixelArray(data->conversion_buffer,l,t,data->alloc_bytes_per_row,rp,ml+l,mt+t,r-l,b-t,RECTFMT_ARGB);
	} else {
		WritePixelArray(data->pixelarray,l,t,VideoMonitor.bytes_per_row,rp,ml+l,mt+t,r-l,b-t,RECTFMT_ARGB);
	}
	data->dirty_valid=0;
	display_host_refresh_pending=false;
	return 0;
}

static ULONG DisplayDispatcher(void)
{
	struct IClass *cl=(struct IClass *)REG_A0; Msg msg=(Msg)REG_A1; Object *obj=(Object *)REG_A2;
	struct Display_Data *data=(struct Display_Data *)INST_DATA(cl,obj);
	switch (msg->MethodID) {
		case OM_NEW: return mNew(cl,obj,msg);
		case OM_DISPOSE: mDispose(obj,data); break;
		case MUIM_AskMinMax: return mAskMinMax(cl,obj,msg,data);
		case MUIM_Show: return mShow(cl,obj,msg,data);
		case MUIM_HandleEvent: return mHandleEvent(obj,msg,data);
		case MUIM_Hide: mHide(obj,data); break;
		case MUIM_Draw: return mDraw(cl,obj,msg,data);
		case MM_Display_Update: return mUpdate(obj,data);
	}
	return DoSuperMethodA(cl,obj,msg);
}

struct EmulLibEntry DisplayTrap={TRAP_LIB,0,(void (*)())&DisplayDispatcher};

bool VideoInit(bool classic)
{
	int width=512,height=342;
	MainScreen=NULL; quitflag=0; is_classic=classic;
	is_fullscreen=PrefsFindBool("fullscreen");
	use_gfxaccel=PrefsFindBool("gfxaccel");
	use_overlay=PrefsFindBool("rgb24overlay");
	use_hardware_cursor=PrefsFindBool("hardwarecursor");
	cursor_valid=false; cursor_visible=false; cursor_serial=1;
	memset(cursor_data,0,sizeof(cursor_data)); cursor_data[0]=16; cursor_data[1]=1;
	use_altivec=PrefsFindBool("altivecgfx");
	altivec_active=false;
	if (use_overlay) {
		CGXVideoBase=OpenLibrary("cgxvideo.library",0);
		if (use_altivec && BasiliskMorphOSHostHasAltiVec() && BasiliskMorphOSAltiVecCompiled() && BasiliskMorphOSAltiVecSelfTest()) altivec_active=true;
	}
	if (!classic) {
		height=384; const char *mode_str=PrefsFindString("screen");
		if (mode_str) { int w,h; if (sscanf(mode_str,"win/%d/%d",&w,&h)==2 && w>=64&&w<=8192&&h>=64&&h<=8192) { width=w;height=h; } }
	}
	int depth=PrefsFindInt32("gfxdepth");
	if (depth!=8&&depth!=16&&depth!=32) depth=PrefsFindBool("8bitgfx")?8:32;
	if (is_fullscreen && !classic) {
		ULONG modeid=BestCModeIDTags(CYBRBIDTG_Depth,32,CYBRBIDTG_NominalWidth,width,CYBRBIDTG_NominalHeight,height,TAG_DONE);
		if (modeid!=(ULONG)INVALID_ID) {
			width=GetCyberIDAttr(CYBRIDATTR_WIDTH,modeid); height=GetCyberIDAttr(CYBRIDATTR_HEIGHT,modeid);
			MainScreen=OpenScreenTags(NULL,SA_DisplayID,modeid,SA_ShowTitle,FALSE,SA_Quiet,TRUE,SA_AutoScroll,TRUE,SA_Depth,32,SA_GammaControl,TRUE,TAG_DONE);
		}
		if (!MainScreen) is_fullscreen=false;
		else gamma_init_screen();
	}
	display_width=width; display_height=height;
	VideoMonitor.x=width; VideoMonitor.y=height;
	VideoMonitor.mode=classic?VMODE_1BIT:(depth==8?VMODE_8BIT:depth==16?VMODE_16BIT:VMODE_32BIT);
	ADBSetRelMouseMode(false);
	return SendGUICmd(GUICMD_InitVideo);
}

bool RunVideo(void)
{
	static CONST struct TagItem ScreenTags[]={
		{MUIA_Window_Borderless,TRUE},{MUIA_Window_DepthGadget,FALSE},{MUIA_Window_DragBar,FALSE},{MUIA_Window_SizeGadget,FALSE},
		{MUIA_Window_LeftEdge,0},{MUIA_Window_TopEdge,0},{MUIA_Window_CloseGadget,FALSE},{TAG_DONE,0}};
	static CONST struct TagItem WinTags[]={
		{MUIA_Window_ID,MAKE_ID('M','A','I','N')},{MUIA_Window_UseBottomBorderScroller,TRUE},{MUIA_Window_UseRightBorderScroller,TRUE},{TAG_DONE,0}};
	Object *display=(Object *)NewObject(CL_Display->mcc_Class,NULL,MUIA_FillArea,FALSE,TAG_DONE);
	if (!display) return false;
	if (!is_fullscreen && !use_overlay) {
		Object *temp=ScrollgroupObject,MUIA_Scrollgroup_UseWinBorder,TRUE,MUIA_Scrollgroup_Contents,VirtgroupObject,Child,display,End,End;
		display=temp;
	}
	Object *win=WindowObject,
		is_fullscreen?MUIA_Window_ScreenTitle:MUIA_Window_Title,(ULONG)GetString(STR_WINDOW_TITLE),
		is_fullscreen?MUIA_Window_Width:TAG_IGNORE,display_width,
		is_fullscreen?MUIA_Window_Height:TAG_IGNORE,display_height,
		is_fullscreen?MUIA_Window_Screen:TAG_IGNORE,MainScreen,
		MUIA_Window_DisableKeys,0xffffffff,
		WindowContents,VGroup,MUIA_InnerBottom,0,MUIA_InnerLeft,0,MUIA_InnerRight,0,MUIA_InnerTop,0,Child,display,End,
		TAG_MORE,is_fullscreen?ScreenTags:WinTags);
	if (!win) return false;
	DoMethod(app,OM_ADDMEMBER,win);
	video_window = win;
	DoMethod(win,MUIM_Notify,MUIA_Window_CloseRequest,TRUE,MUIV_Notify_Self,3,MUIM_WriteLong,1,&quitflag);
	SetAttrs(win,MUIA_Window_Open,TRUE,TAG_DONE);
	return true;
}

void VideoPrepareGUIShutdown(void)
{
	/* Closing the MUI window explicitly guarantees that MUIM_Hide is delivered
	 * before MUI tears down the underlying Intuition Window.  The old shutdown
	 * path disposed the whole Application directly, and _window(display) could
	 * already contain MorphOS' 0xdeadbeef poison by the time cursor cleanup ran. */
	if (video_window) {
		SetAttrs(video_window,MUIA_Window_Open,FALSE,TAG_DONE);
		video_window = NULL;
	}
}

void VideoExit(void)
{
	// The GUI thread owns MainScreen and closes it during its own shutdown.
	// Only release video-specific resources here.
	if (CGXVideoBase) { CloseLibrary(CGXVideoBase); CGXVideoBase=NULL; }
}

void video_set_palette(uint8 *pal)
{
	for (int i=0;i<256;i++) {
		ULONG r=pal[i*3],g=pal[i*3+1],b=pal[i*3+2];
		palette[i]=(r<<16)|(g<<8)|b;
	}
	palette_changed=1;
}

void video_set_gamma(const uint8 *red, const uint8 *green, const uint8 *blue)
{
	if (red && green && blue) {
		memcpy(mac_gamma_red, red, sizeof(mac_gamma_red));
		memcpy(mac_gamma_green, green, sizeof(mac_gamma_green));
		memcpy(mac_gamma_blue, blue, sizeof(mac_gamma_blue));
	} else {
		gamma_identity(mac_gamma_red, mac_gamma_green, mac_gamma_blue);
	}
	// Only a private fullscreen screen may have its gamma changed. In a window
	// the Ambient screen already carries the MorphOS monitor gamma.
	gamma_apply();
}

void video_set_mode(int mode)
{
	(void)mode;
	mode_changed=1;
}


void VideoInterrupt(void)
{
	static int counter=60*5;
	if (--counter<=0 || quitflag) {
		counter=60*5;
		if (quitflag || (SetSignal(0,SIGBREAKF_CTRL_C)&SIGBREAKF_CTRL_C)) QuitEmulator();
	}
}
