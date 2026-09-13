/*
 *  main_morphos.cpp - Startup code for MorphOS
 *
 *  Basilisk II (C) 1997-2001 Christian Bauer
 *                  2005-2006 Ilkka Lehtoranta
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

#include <exec/execbase.h>
#include <exec/tasks.h>
#include <dos/dostags.h>
#include <intuition/intuition.h>
#include <devices/timer.h>
#include <devices/ahi.h>
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include <proto/locale.h>
#include <proto/muimaster.h>

#include "guithread.h"

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "xpram.h"
#include "timer.h"
#include "sony.h"
#include "disk.h"
#include "cdrom.h"
#include "scsi.h"
#include "audio.h"
#include "video.h"
#include "serial.h"
#include "ether.h"
#include "clip.h"
#include "emul_op.h"
#include "rom_patches.h"
#include "prefs.h"
#include "prefs_editor.h"
#include "sys.h"
#include "user_strings.h"
#include "version.h"

#define DEBUG 0
#include "debug.h"


extern APTR _WBenchMsg;

// Options for libnix
const int __nocommandline = 1;			// Disable command line parsing


// Constants
static const char ROM_FILE_NAME[] = "ROM";
static const char __ver[] = "$VER: " VERSION_STRING " " __DATE__;
static const int SCRATCH_MEM_SIZE = 65536;

// CPU and FPU type, addressing mode
int CPUType = 4;
bool CPUIs68060;
int FPUType;
bool TwentyFourBitAddressing;


// Global variables
struct Library *GfxBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;
struct Library *IFFParseBase = NULL;
struct Library *AslBase = NULL;
struct Library *CyberGfxBase = NULL;
struct Library *TimerBase = NULL;
struct Library *MUIMasterBase;
struct Library *IconBase;
struct Library *LocaleBase;
struct Library *KeymapBase;
struct Library *UtilityBase;

struct Task *MainTask;							// Our task
uint8 *ScratchMem = NULL;						// Scratch memory for Mac ROM writes

ULONG	SubTaskCount;
struct MsgPort	*StartupMsgPort, *GUIPort;
struct Process	*SoundProc;

static struct timerequest timereq;				// IORequest for timer
static struct Process *xpram_proc = NULL;		// XPRAM watchdog
static struct Process *tick_proc = NULL;		// 60Hz process
struct DiskObject *dobj;

// Prototypes
static void xpram_func(void);
static void tick_func(void);

static bool ChoiceAlert2(const char *text, const char *pos, const char *neg);

struct Catalog *catalog;

// Startup state used to make QuitEmulator() safe after partial initialization.
static bool prefs_initialized = false;
static bool sys_initialized = false;
static bool gui_init_attempted = false;
static bool timer_open = false;
static bool init_all_started = false;

/*
 * Open libraries
 */

static int openlibs(void)
{
	if ((UtilityBase = OpenLibrary("utility.library", 36)))
	if ((GfxBase = OpenLibrary("graphics.library", 39)))
	if ((IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39)))
	if ((IFFParseBase = OpenLibrary("iffparse.library", 39)))
	if ((AslBase = OpenLibrary("asl.library", 36)))
	if ((IconBase = OpenLibrary("icon.library", 0)))
	if ((MUIMasterBase = OpenLibrary("muimaster.library", 0)))
	if ((CyberGfxBase = OpenLibrary("cybergraphics.library", 0)))
	if ((LocaleBase = OpenLibrary("locale.library", 0)))
	{
		catalog = OpenCatalog(NULL, "basilisk.catalog", OC_BuiltInLanguage, "english", TAG_DONE);

		KeymapBase = OpenLibrary("keymap.library", 51);

		return 1;
	}

	return 0;
}

/*
 *  Main program
 */

int main(int argc, char **argv)
{
	static int wbstart = _WBenchMsg ? 1 : 0;

	// Initialize variables
	RAMBaseHost = NULL;
	ROMBaseHost = NULL;
	MainTask = FindTask(NULL);
	struct DateStamp ds;
	DateStamp(&ds);
	srand(ds.ds_Tick);

	// Keep shell startup quiet. Errors and warnings are still reported.

	if (openlibs() == 0)
	{
		if (MUIMasterBase == NULL)
			exit(1);
		else
			QuitEmulator();
	}

	// Find program icon

#if 1
	if (wbstart)
	{
		struct WBStartup *wbmsg = (struct WBStartup *)_WBenchMsg;	//argv;
		struct WBArg	*wargs;
		BPTR	lock;

		wargs	= wbmsg->sm_ArgList;

		if (wbmsg->sm_NumArgs > 1)
			wargs++;

		lock	= CurrentDir(wargs->wa_Lock);
		dobj	= GetDiskObject((char *const)wargs->wa_Name);

		CurrentDir(lock);
	}

	if (dobj == NULL)
#endif
	{
		dobj	= GetDiskObject("PROGDIR:BasiliskII");
	}

	StartupMsgPort = CreateMsgPort();
	if (StartupMsgPort == NULL)
		QuitEmulator();

	gui_init_attempted = true;
	if (InitGUIThread() == 0)
		QuitEmulator();

	// Read preferences
	PrefsInit(argc, argv);
	prefs_initialized = true;

	// Init system routines
	SysInit();
	sys_initialized = true;

	// Show preferences editor
	if (!PrefsFindBool("nogui"))
	{
		if (!PrefsEditor())
			QuitEmulator();
	}

	// Open timer.device
	if (OpenDevice(TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&timereq, 0)) {
		ErrorAlert(GetString(STR_NO_TIMER_DEV_ERR));
		QuitEmulator();
	}
	TimerBase = (struct Library *)timereq.tr_node.io_Device;
	timer_open = true;

	// Allocate scratch memory
	ScratchMem = (uint8 *)AllocTaskPooled(SCRATCH_MEM_SIZE);
	if (ScratchMem == NULL) {
		ErrorAlert(GetString(STR_NO_MEM_ERR));
		QuitEmulator();
	}
	ScratchMem += SCRATCH_MEM_SIZE/2;	// ScratchMem points to middle of block

	// Create area for Mac RAM and ROM (ROM must be higher in memory,
	// so we allocate one big chunk and put the ROM at the top of it)
	RAMSize = PrefsFindInt32("ramsize") & 0xfff00000;	// Round down to 1MB boundary
	if (RAMSize < 1024*1024) {
		WarningAlert(GetString(STR_SMALL_RAM_WARN));
		RAMSize = 1024*1024;
	}

	RAMBaseHost = (uint8 *)AllocTaskPooled(RAMSize + 0x100000);
	if (RAMBaseHost == NULL)
	{
		int64 newRAMSize = AvailMem(MEMF_LARGEST);
		newRAMSize -= 0x100000;
		newRAMSize -= 0x100000*16;
		newRAMSize &= ~((int64)0xfffff);	// Keep Mac RAM on a 1 MB boundary

		if (newRAMSize >= (1024*1024))
		{
			char xText[120];

			snprintf(xText, sizeof(xText), GetString(STR_NOT_ENOUGH_MEM_WARN), RAMSize / 1024 / 1024, (int32)(newRAMSize / 1024 / 1024));

			if (ChoiceAlert2(xText, "Use", "Quit") != 1)
				QuitEmulator();

			RAMSize = newRAMSize;
			RAMBaseHost = (uint8 *)AllocTaskPooled(RAMSize + 0x100000);
		}

		if (RAMBaseHost == NULL)
		{
			ErrorAlert(GetString(STR_NO_MEM_ERR));
			QuitEmulator();
		}
	}

	RAMBaseMac	= 0;
	ROMBaseMac	= RAMBaseMac + RAMSize;
	ROMBaseHost = RAMBaseHost + RAMSize;
	MEMBaseDiff	= (uintptr)RAMBaseHost;
//	InitMEMBaseDiff(RAMBaseHost, RAMBaseMac);

	// Get rom file path from preferences
	const char *rom_path = PrefsFindString("rom");

	// Load Mac ROM
	BPTR rom_fh = Open(rom_path ? rom_path : ROM_FILE_NAME, MODE_OLDFILE);
	if (rom_fh == NULL)
	{
		ErrorAlert(GetString(STR_NO_ROM_FILE_ERR));
		QuitEmulator();
	}
	struct FileInfoBlock fib;
	ExamineFH(rom_fh, &fib);

	ROMSize = fib.fib_Size;

	if (ROMSize != 512*1024 && ROMSize != 1024*1024)
	{
		ErrorAlert(GetString(STR_ROM_SIZE_ERR));
		Close(rom_fh);
		QuitEmulator();
	}

	if (Read(rom_fh, ROMBaseHost, ROMSize) != (LONG)ROMSize)
	{
		Close(rom_fh);
		ErrorAlert(GetString(STR_ROM_FILE_READ_ERR));
		QuitEmulator();
	}

	Close(rom_fh);

	// Check the ROM before marking the subsystem set as initialized. InitAll()
	// starts with the same check, but this prevents ExitAll() from saving an
	// uninitialized XPRAM image when the ROM itself is unsupported.
	if (!CheckROM()) {
		ErrorAlert(GetString(STR_UNSUPPORTED_ROM_TYPE_ERR));
		QuitEmulator();
	}

	// Initialize everything
	init_all_started = true;
	if (!InitAll())
		QuitEmulator();

	struct Message msg1, msg2;

	msg1.mn_Node.ln_Type	= NT_MESSAGE;
	msg1.mn_ReplyPort		= StartupMsgPort;
	msg1.mn_Length			= sizeof(msg1);
	msg2.mn_Node.ln_Type	= NT_MESSAGE;
	msg2.mn_ReplyPort		= StartupMsgPort;
	msg2.mn_Length			= sizeof(msg2);

	// Start XPRAM watchdog process
	xpram_proc = CreateNewProcTags(
		NP_Entry, (ULONG)xpram_func,
		NP_Name, (ULONG)"Basilisk II XPRAM Watchdog",
		NP_Priority, 0,
		NP_CodeType, CODETYPE_PPC,
		NP_StartupMsg, &msg1,
		TAG_END
	);

	if (xpram_proc)
	{
		SubTaskCount++;
	}

	// Start 60Hz process
	ULONG dummyport;

	tick_proc = CreateNewProcTags(
		NP_Entry, (ULONG)tick_func,
		NP_Name, (ULONG)"Basilisk II 60Hz",
		NP_Priority, 5,
		NP_CodeType, CODETYPE_PPC,
		NP_StartupMsg, &msg2,
		NP_TaskMsgPort, &dummyport,
		TAG_END
	);

	if (tick_proc)
	{
		SubTaskCount++;

		// Set task priority to -5 so we don't use all processing time
		SetTaskPri(MainTask, -5);

		// Jump to ROM boot routine
		//VNewRawDoFmt("Start emulation\n", (APTR (*)(APTR, UBYTE))1, NULL, NULL);
		Start680x0();
	}

	QuitEmulator();
	return 0;
}

void QuitEmulator(void)
{
	// Abort processes

	Forbid();
	if (SoundProc)
	{
		Signal((struct Task *)SoundProc, SIGBREAKF_CTRL_C);
	}

	// Stop 60Hz process
	if (tick_proc)
	{
		Signal((struct Task *)tick_proc, SIGBREAKF_CTRL_C);
	}

	// Stop XPRAM watchdog process

	if (xpram_proc)
	{
		Signal((struct Task *)xpram_proc, SIGBREAKF_CTRL_C);
	}
	Permit();

	if (gui_init_attempted) {
		FinishGUIThread();
		gui_init_attempted = false;
	}

	while (SubTaskCount && StartupMsgPort)
	{
		WaitPort(StartupMsgPort);
		GetMsg(StartupMsgPort);
		SubTaskCount--;
	}

	// idle_wait() lazily allocates an Exec signal on MainTask. All helper
	// tasks are stopped now, so no later TriggerInterrupt()/idle_resume() can
	// race the release.
	idle_exit();

	// Deinitialize everything only if InitAll() was actually entered.
	if (init_all_started) {
		ExitAll();
		init_all_started = false;
	}

	// Close timer.device
	if (timer_open) {
		CloseDevice((struct IORequest *)&timereq);
		timer_open = false;
		TimerBase = NULL;
	}

	// Exit system routines
	if (sys_initialized) {
		SysExit();
		sys_initialized = false;
	}

	// Exit preferences
	if (prefs_initialized) {
		PrefsExit();
		prefs_initialized = false;
	}

	if (StartupMsgPort) {
		DeleteMsgPort(StartupMsgPort);
		StartupMsgPort = NULL;
	}

	// Close libraries. openlibs() can fail halfway through, so every handle
	// must be checked independently.
	if (catalog) { CloseCatalog(catalog); catalog = NULL; }
	if (LocaleBase) { CloseLibrary(LocaleBase); LocaleBase = NULL; }
	if (dobj) { FreeDiskObject(dobj); dobj = NULL; }
	if (IconBase) { CloseLibrary(IconBase); IconBase = NULL; }
	if (MUIMasterBase) { CloseLibrary(MUIMasterBase); MUIMasterBase = NULL; }
	if (CyberGfxBase) { CloseLibrary(CyberGfxBase); CyberGfxBase = NULL; }
	if (AslBase) { CloseLibrary(AslBase); AslBase = NULL; }
	if (IFFParseBase) { CloseLibrary(IFFParseBase); IFFParseBase = NULL; }
	if (IntuitionBase) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; }
	if (GfxBase) { CloseLibrary(GfxBase); GfxBase = NULL; }
	if (KeymapBase) { CloseLibrary(KeymapBase); KeymapBase = NULL; }
	if (UtilityBase) { CloseLibrary(UtilityBase); UtilityBase = NULL; }

	exit(0);
}


/*
 *  Code was patched, flush caches if neccessary (i.e. when using a real 680x0
 *  or a dynamically recompiling emulator)
 */

void FlushCodeCache(void *start, uint32 size)
{
}

/*
 *  Interrupt flags (must be handled atomically!)
 */

uint32 InterruptFlags;

#if defined(__PPC__)
extern "C" {
void SetIntFlag(uint32 flag, uint32 *ptr);
void ClearIntFlag(uint32 flag, uint32 *ptr);
asm(
	".section \".text\"\n"
	".align 2\n"
	".type SetIntFlag,@function\n"
	".globl SetIntFlag\n"
	"SetIntFlag:\n"
	"lwarx   %r12,%r0,%r4\n"
	"or      %r12,%r12,%r3\n"
	"stwcx.  %r12,%r0,%r4\n"
	"bne-    SetIntFlag\n"
	"blr\n"
	".type ClearIntFlag,@function\n"
	".globl ClearIntFlag\n"
	"ClearIntFlag:\n"
	"lwarx   %r12,%r0,%r4\n"
	"andc    %r12,%r12,%r3\n"
	"stwcx.  %r12,%r0,%r4\n"
	"bne-    ClearIntFlag\n"
	"blr\n"
);
}
#else
static void SetIntFlag(uint32 flag, uint32 *ptr)
{
	Forbid();
	*ptr |= flag;
	Permit();
}

static void ClearIntFlag(uint32 flag, uint32 *ptr)
{
	Forbid();
	*ptr &= ~flag;
	Permit();
}
#endif

void SetInterruptFlag(uint32 flag)
{
	SetIntFlag(flag, &InterruptFlags);
}

void ClearInterruptFlag(uint32 flag)
{
	ClearIntFlag(flag, &InterruptFlags);
}

/*
 *  60Hz thread (really 60.15Hz)
 */

static void tick_func(void)
{
	int tick_counter = 0;
	struct MsgPort *timer_port;
	struct timerequest timer_io;
	ULONG timer_mask, timer_ok = 0;

	// Start 60Hz timer
	NewGetTaskAttrs(NULL, &timer_port, sizeof(struct MsgPort *), TASKINFOTYPE_TASKMSGPORT, TAG_DONE);

	timer_mask = 1 << timer_port->mp_SigBit;
	timer_io.tr_node.io_Message.mn_ReplyPort	= timer_port;
	timer_io.tr_node.io_Message.mn_Length		= sizeof(timer_io);	// meaningless

	if (!OpenDevice(TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&timer_io, 0))
	{
		timer_ok = 1;
		timer_io.tr_node.io_Command = TR_ADDREQUEST;
		timer_io.tr_time.tv_secs = 0;
		timer_io.tr_time.tv_micro = 16625;
		SendIO((struct IORequest *)&timer_io);
	}

	while (1)
	{
		ULONG	sigs;

		// Wait for timer tick
		sigs = Wait(timer_mask | SIGBREAKF_CTRL_C);

		if (sigs & SIGBREAKF_CTRL_C)
			break;

		// Restart timer
		if (GetMsg(timer_port))
		{
			timer_io.tr_node.io_Command = TR_ADDREQUEST;
			timer_io.tr_time.tv_secs = 0;
			timer_io.tr_time.tv_micro = 16625;
			SendIO((struct IORequest *)&timer_io);

			// Pseudo Mac 1Hz interrupt, update local time
			if (++tick_counter > 60)
			{
				tick_counter = 0;
				WriteMacInt32(0x20c, TimerDateTime());
				SetInterruptFlag(INTFLAG_1HZ);
				TriggerInterrupt();
			}

			// Trigger 60Hz interrupt
			SetInterruptFlag(INTFLAG_60HZ);
			TriggerInterrupt();
		}
	}

	tick_proc	= NULL;

	// Stop timer
	if (timer_ok)
	{
		if (!CheckIO((struct IORequest *)&timer_io))
			AbortIO((struct IORequest *)&timer_io);
		WaitIO((struct IORequest *)&timer_io);
		CloseDevice((struct IORequest *)&timer_io);
	}
}


/*
 *  XPRAM watchdog thread (saves XPRAM every minute)
 */

static void xpram_func(void)
{
#if 0
	uint8 last_xpram[256];
	memcpy(last_xpram, XPRAM, 256);

	while (xpram_proc_active) {
		for (int i=0; i<60 && xpram_proc_active; i++)
			Delay(50);		// Only wait 1 second so we quit promptly when xpram_proc_active becomes false
		if (memcmp(last_xpram, XPRAM, 256)) {
			memcpy(last_xpram, XPRAM, 256);
			SaveXPRAM();
		}
	}
#else
	Wait(SIGBREAKF_CTRL_C);
	xpram_proc	= NULL;
	SaveXPRAM();
#endif
}


/*
 *  Display error alert
 */

void ErrorAlert(const char *text)
{
	if (PrefsFindBool("nogui")) {
		printf(GetString(STR_SHELL_ERROR_PREFIX), text);
		return;
	}

	MUI_Request(NULL, NULL, 0, (STRPTR)GetString(STR_ERROR_ALERT_TITLE), (STRPTR)GetString(STR_QUIT_BUTTON), (STRPTR)GetString(STR_GUI_ERROR_PREFIX), text);
}


/*
 *  Display warning alert
 */

void WarningAlert(const char *text)
{
	if (PrefsFindBool("nogui")) {
		printf(GetString(STR_SHELL_WARNING_PREFIX), text);
		return;
	}

	MUI_Request(NULL, NULL, 0, (STRPTR)GetString(STR_WARNING_ALERT_TITLE), (STRPTR)GetString(STR_OK_BUTTON), (STRPTR)GetString(STR_GUI_WARNING_PREFIX), text);
}


/*
 *  Display choice alert
 */

static bool ChoiceAlert2(const char *text, const char *pos, const char *neg)
{
	TEXT str[256];
	snprintf((char *)str, sizeof(str), "%s|%s", pos, neg);
	return MUI_Request(NULL, NULL, 0, (STRPTR)GetString(STR_WARNING_ALERT_TITLE), (STRPTR)str, (STRPTR)GetString(STR_GUI_WARNING_PREFIX), text);
}


/*
 *  Mutexes
 */

struct B2_mutex {
	int dummy;	//!!
};

B2_mutex *B2_create_mutex(void)
{
	return (struct B2_mutex *)1;
}

void B2_lock_mutex(B2_mutex *mutex)
{
}

void B2_unlock_mutex(B2_mutex *mutex)
{
}

void B2_delete_mutex(B2_mutex *mutex)
{
}
