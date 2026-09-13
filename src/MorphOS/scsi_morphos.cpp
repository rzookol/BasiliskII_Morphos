/*
 *  scsi_amiga.cpp - SCSI Manager, Amiga specific stuff
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

#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <proto/exec.h>

#include "sysdeps.h"
#include "main.h"
#include "prefs.h"
#include "user_strings.h"
#include "scsi.h"

#define DEBUG 0
#include "debug.h"


// Global variables
static struct SCSICmd scsi;

static IOStdReq *ios[8*8];			// IORequests for 8 units and 8 LUNs each
static IOStdReq *io = NULL;			// Active IORequest (selected target)

static struct MsgPort *the_port = NULL;	// Message port for device communication

static ULONG buffer_size = 0x10000;	// Size of data buffer
static UBYTE *buffer = NULL;			// Pointer to data buffer

static UBYTE cmd_buffer[12];			// Buffer for SCSI command

static const int SENSE_LENGTH = 256;
static UBYTE sense_buffer[SENSE_LENGTH];		// Buffer for autosense data


/*
 *  Initialization
 */

void SCSIInit(void)
{
	int id, lun;

	// Create port and buffers
	the_port = CreateMsgPort();
	buffer = (UBYTE *)AllocTaskPooled(buffer_size);
	if (the_port == NULL || buffer == NULL)
	{
		if (buffer) {
			FreeTaskPooled(buffer, buffer_size);
			buffer = NULL;
		}
		if (the_port) {
			DeleteMsgPort(the_port);
			the_port = NULL;
		}
		ErrorAlert(GetString(STR_NO_MEM_ERR));
		QuitEmulator();
		return;
	}

	memset(ios, 0, sizeof(ios));
	memset(&scsi, 0, sizeof(scsi));
	memset(cmd_buffer, 0, sizeof(cmd_buffer));
	memset(sense_buffer, 0, sizeof(sense_buffer));
	io = NULL;

	// Create and open IORequests for all 8 units (and all 8 LUNs)
	for (id=0; id<8; id++)
	{
		char prefs_name[16];
		snprintf(prefs_name, sizeof(prefs_name), "scsi%d", id);
		const char *str = PrefsFindString(prefs_name);
		if (str)
		{
			char dev_name[256];
			ULONG dev_unit = 0;
			if (sscanf(str, "%255[^/]/%lu", dev_name, &dev_unit) == 2)
			{
				for (lun=0; lun<8; lun++)
				{
					struct IOStdReq *req = (struct IOStdReq *)CreateIORequest(the_port, sizeof(struct IOStdReq));

					if (req == NULL)
						continue;

					if (OpenDevice(dev_name, dev_unit + lun * 10, (struct IORequest *)req, 0)) {
						DeleteIORequest(req);
						continue;
					}
					req->io_Data = &scsi;
					req->io_Length = sizeof(scsi);
					req->io_Command = HD_SCSICMD;
					ios[id*8+lun] = req;
				}
			}
		}
	}

	// Reset SCSI bus
	SCSIReset();

	// Init SCSICmd
	scsi.scsi_Command = cmd_buffer;
	scsi.scsi_SenseData = sense_buffer;
	scsi.scsi_SenseLength = SENSE_LENGTH;
}


/*
 *  Deinitialization
 */

void SCSIExit(void)
{
	// Close all devices
	for (int i=0; i<8; i++)
		for (int j=0; j<8; j++) {
			struct IOStdReq *io = ios[i*8+j];
			if (io)
			{
				CloseDevice((struct IORequest *)io);
				DeleteIORequest(io);
				ios[i*8+j] = NULL;
			}
		}

	// Delete port and buffers
	if (the_port) {
		DeleteMsgPort(the_port);
		the_port = NULL;
	}
	if (buffer) {
		FreeTaskPooled(buffer, buffer_size);
		buffer = NULL;
	}
	io = NULL;
}


/*
 *  Check if requested data size fits into buffer, allocate new buffer if needed
 */

static bool try_buffer(size_t size)
{
	if (size <= buffer_size)
		return true;

	if (size > 0xffffffffUL)
		return false;

	UBYTE *new_buffer = (UBYTE *)AllocTaskPooled((ULONG)size);
	if (new_buffer == NULL)
		return false;
	FreeTaskPooled(buffer, buffer_size);
	buffer = new_buffer;
	buffer_size = (ULONG)size;
	return true;
}


/*
 *  Set SCSI command to be sent by scsi_send_cmd()
 */

void scsi_set_cmd(int cmd_length, uint8 *cmd)
{
	if (cmd == NULL || cmd_length <= 0 || cmd_length > (int)sizeof(cmd_buffer)) {
		scsi.scsi_CmdLength = 0;
		memset(cmd_buffer, 0, sizeof(cmd_buffer));
		return;
	}

	scsi.scsi_CmdLength = cmd_length;
	memcpy(cmd_buffer, cmd, cmd_length);
}


/*
 *  Check for presence of SCSI target
 */

bool scsi_is_target_present(int id)
{
	return id >= 0 && id < 8 && ios[id * 8] != NULL;
}


/*
 *  Set SCSI target (returns false on error)
 */

bool scsi_set_target(int id, int lun)
{
	if (id < 0 || id >= 8 || lun < 0 || lun >= 8)
		return false;

	struct IOStdReq *new_io = ios[id * 8 + lun];
	if (new_io == NULL)
		return false;
	if (new_io != io)
		scsi.scsi_SenseActual = 0;	// Clear sense data when selecting new target
	io = new_io;
	return true;
}


/*
 *  Send SCSI command to active target (scsi_set_command() must have been called),
 *  read/write data according to S/G table (returns false on error); timeout is in 1/60 sec
 */

bool scsi_send_cmd(size_t data_length, bool reading, int sg_size, uint8 **sg_ptr, uint32 *sg_len, uint16 *stat, uint32 timeout)
{
	if (io == NULL || stat == NULL || scsi.scsi_CmdLength == 0 ||
	    sg_size < 0 || (sg_size > 0 && (sg_ptr == NULL || sg_len == NULL)))
		return false;

	// Validate the scatter/gather table before touching the shared buffer.
	size_t sg_total = 0;
	for (int i = 0; i < sg_size; i++) {
		if (sg_ptr[i] == NULL && sg_len[i] != 0)
			return false;
		if ((size_t)sg_len[i] > data_length - sg_total)
			return false;
		sg_total += sg_len[i];
	}
	if (sg_total != data_length)
		return false;

	// Check if buffer is large enough, allocate new buffer if needed
	if (!try_buffer(data_length)) {
		char str[256];
		snprintf(str, sizeof(str), GetString(STR_SCSI_BUFFER_ERR), (ULONG)data_length);
		ErrorAlert(str);
		return false;
	}

	// Process S/G table when writing
	if (!reading) {
		D(bug(" writing to buffer\n"));
		uint8 *buffer_ptr = buffer;
		for (int i=0; i<sg_size; i++) {
			uint32 len = sg_len[i];
			D(bug("  %d bytes from %08lx\n", len, sg_ptr[i]));
			memcpy(buffer_ptr, sg_ptr[i], len);
			buffer_ptr += len;
		}
	}

	// Request Sense and autosense data valid?
	BYTE res = 0;
	if (cmd_buffer[0] == 0x03 && scsi.scsi_SenseActual) {

		// Yes, fake command
		D(bug(" autosense\n"));
		size_t sense_len = scsi.scsi_SenseActual;
		if (sense_len > data_length)
			sense_len = data_length;
		memcpy(buffer, sense_buffer, sense_len);
		if (sense_len < data_length)
			memset(buffer + sense_len, 0, data_length - sense_len);
		scsi.scsi_Status = 0;
		*stat = 0;

	} else {

		// No, send regular command
		D(bug(" sending command, length %ld\n", data_length));
		scsi.scsi_Data = (UWORD *)buffer;
		scsi.scsi_Length = data_length;
		scsi.scsi_Actual = 0;
		scsi.scsi_Flags = (reading ? SCSIF_READ : SCSIF_WRITE) | SCSIF_AUTOSENSE;
		scsi.scsi_SenseActual = 0;
		scsi.scsi_Status = 0;
		res = DoIO((struct IORequest *)io);
		D(bug(" command sent, res %d, status %d\n", res, scsi.scsi_Status));
		*stat = scsi.scsi_Status;
	}

	// Process S/G table when reading
	if (reading && res == 0) {
		D(bug(" reading from buffer\n"));
		uint8 *buffer_ptr = buffer;
		for (int i=0; i<sg_size; i++) {
			uint32 len = sg_len[i];
			D(bug("  %d bytes to %08lx\n", len, sg_ptr[i]));
			memcpy(sg_ptr[i], buffer_ptr, len);
			buffer_ptr += len;
		}
	}
	return res == 0;
}
