/*
 *  mui.h - MorphOS MUI support functions
 *
 *  Basilisk II (C) 1997-2001 Christian Bauer
 *                  2005 Ilkka Lehtoranta
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

#ifndef MUI_MORPHOS_H
#define MUI_MORPHOS_H

#define CATCOMP_NUMBERS
#include <intuition/pointerclass.h>
#include <cybergraphx/cgxvideo.h>
#include "locale/locale.h"

// Methods and tags

#define MM_Application_About	(0xad00e000 + 0)
#define MM_Display_Update		(0xad00e000 + 0)

typedef enum
{
	DRAW_CLASSIC = 0,
	DRAW_LUT_8BIT,
	DRAW_PIXELARRAY,
	DRAW_CHUNKYPIX
} DRAWMODE;

struct Display_Data
{
	UWORD				null_pointer[6];
	UWORD				*current_pointer;
	Object				*cursor_object;
	struct BitMap		*cursor_bitmap;
	ULONG				cursor_serial;
	UBYTE				mac_cursor[68];
	DRAWMODE			drawmode;
	ULONG				width, height, frameskip, bytes_per_row;
	ULONG				alloc_bytes_per_row, last_guest_bpr;
	LONG				last_mode;
	APTR				pixelarray;
	APTR				conversion_buffer;
	APTR				shadow_buffer;
	struct VLayerHandle	*VLayer;
	Object				*parent;
	ULONG				overlay_active;
	ULONG				overlay_geometry_valid;
	LONG				overlay_dest_left, overlay_dest_top;
	ULONG				overlay_dest_width, overlay_dest_height;
	ULONG				mouse_in_display;
	ULONG				dirty_left, dirty_top, dirty_right, dirty_bottom, dirty_valid;

	struct MsgPort						*timerport;
	struct timerequest				*timer_io;
	struct MUI_InputHandlerNode	ihnode;
	struct MUI_EventHandlerNode	ehnode;
	ULONG								timer_ok;
	ULONG								pix_array_size;
	ULONG								conversion_size;
	ULONG								shadow_size;
};


#define	MUIA_Application_UsedClasses	0x8042e9a7	/* V20 STRPTR *	i..	*/
#define	MUIA_Dtpic_Name 0x80423d72
#define	MUIA_Window_DisableKeys 0x80424c36 /* V15 isg ULONG */ /* private */

CONST_STRPTR GetLocaleString(LONG id);
ULONG		getv			(Object *obj, ULONG attr);
Object *	MakePopFile	(LONG id, ULONG maxlen, Object **str_obj);
Object *	MakeCycle	(LONG id, const CONST_STRPTR *entries);
Object *	MakeRect		(ULONG weight);
Object *	MakeCheck	(LONG id, Object **ch_obj);
Object *	MakeCheckmark(LONG id);
Object *	MakeInteger	(LONG id, ULONG maxlen);
Object *	MakeString	(LONG id, ULONG maxlen);
Object *	MakeLabel	(LONG id);
Object *	MakeButton	(LONG id);
Object *	MakeSlider	(LONG id, ULONG min, ULONG max);

#define _L(id) GetLocaleString(id)

#endif /* MUI_MORPHOS_H */