/*
 *  timer_amiga.cpp - Time Manager emulation, Amiga specific stuff
 *
 *  Basilisk II (C) 1997-2001 Christian Bauer
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

#include <devices/timer.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include "sysdeps.h"
#include "timer.h"

#define DEBUG 0
#include "debug.h"


// Main emulator task (created in main_morphos.cpp).
extern struct Task *MainTask;

// Exec signal used to wake the emulation task from SynchIdleTime().
// It is allocated lazily by the emulation task itself.
static BYTE idle_sigbit = -1;


/*
 *  Suspend execution of the emulator task while the guest is idle.
 */

void idle_wait(void)
{
	if (idle_sigbit < 0) {
		idle_sigbit = AllocSignal(-1);
		if (idle_sigbit < 0) {
			// Very unlikely fallback if no Exec signal bit is available.
			Delay(1);
			return;
		}
	}

	Wait(1UL << idle_sigbit);
}


/*
 *  Wake the emulation task when a host-side event/interrupt arrives.
 */

void idle_resume(void)
{
	if (idle_sigbit >= 0 && MainTask != NULL)
		Signal(MainTask, 1UL << idle_sigbit);
}


/*
 *  Release the Exec signal allocated by idle_wait().
 *
 *  AllocSignal() changes the signal allocation mask of the current task, so
 *  every successful allocation must be paired with FreeSignal() before the
 *  process exits. QuitEmulator() calls this from MainTask after all helper
 *  tasks have stopped, so there can be no later idle_resume() racing the free.
 */

void idle_exit(void)
{
	if (idle_sigbit >= 0 && FindTask(NULL) == MainTask) {
		ULONG mask = 1UL << idle_sigbit;
		SetSignal(0, mask);      // Drop a stale wakeup, if one is pending.
		FreeSignal(idle_sigbit);
		idle_sigbit = -1;
	}
}


/*
 *  Return microseconds since boot (64 bit)
 */

void Microseconds(uint32 &hi, uint32 &lo)
{
	D(bug("Microseconds\n"));
	struct timeval tv;
	GetSysTime(&tv);
	uint64 tl = (uint64)tv.tv_secs * 1000000 + tv.tv_micro;
	hi = tl >> 32;
	lo = tl;
}


/*
 *  Return local date/time in Mac format (seconds since 1.1.1904)
 */

uint32 TimerDateTime(void)
{
	// timer.device system time uses the native MorphOS epoch, 1-Jan-1978.
	// Classic MacOS uses local seconds since 1-Jan-1904.
	struct timeval tv;
	GetSysTime(&tv);
	return tv.tv_secs + TIME_OFFSET;
}


/*
 *  Get current time
 */

void timer_current_time(tm_time_t &t)
{
	GetSysTime(&t);
}


/*
 *  Add times
 */

void timer_add_time(tm_time_t &res, tm_time_t a, tm_time_t b)
{
	res = a;
	AddTime(&res, &b);
}


/*
 *  Subtract times
 */

void timer_sub_time(tm_time_t &res, tm_time_t a, tm_time_t b)
{
	res = a;
	SubTime(&res, &b);
}


/*
 *  Compare times (<0: a < b, =0: a = b, >0: a > b)
 */

int timer_cmp_time(tm_time_t a, tm_time_t b)
{
	return CmpTime(&b, &a);
}


/*
 *  Convert Mac time value (>0: microseconds, <0: microseconds) to tm_time_t
 */

void timer_mac2host_time(tm_time_t &res, int32 mactime)
{
	if (mactime > 0) {
		res.tv_secs = mactime / 1000;			// Time in milliseconds
		res.tv_micro = (mactime % 1000) * 1000;
	} else {
		int64 usec = -(int64)mactime;
		res.tv_secs = usec / 1000000;		// Time in negative microseconds
		res.tv_micro = usec % 1000000;
	}
}


/*
 *  Convert positive tm_time_t to Mac time value (>0: microseconds, <0: microseconds)
 *  A negative input value for hosttime results in a zero return value
 *  As long as the microseconds value fits in 32 bit, it must not be converted to milliseconds!
 */

int32 timer_host2mac_time(tm_time_t hosttime)
{
	if (hosttime.tv_secs < 0)
		return 0;
	else {
		UQUAD t = (UQUAD)hosttime.tv_secs * 1000000 + hosttime.tv_micro;
		if (t > 0x7fffffff) {
			UQUAD msec = t / 1000;
			return msec > 0x7fffffff ? 0x7fffffff : (int32)msec;	// Time in milliseconds
		} else
			return -(int32)t;		// Time in negative microseconds
	}
}
