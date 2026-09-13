#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include <devices/hardblocks.h>
#include <devices/trackdisk.h>
#include <dos/filehandler.h>
#include <libraries/mui.h>
#include <proto/alib.h>
#include <proto/exec.h>

#include "mui.h"
#include "rdb.h"

#define MAX_RDB_BLOCK_SIZE 65536
#define MAX_RDB_PARTITIONS 128

static bool valid_block_size(ULONG blocksize)
{
	return blocksize >= 512 && blocksize <= MAX_RDB_BLOCK_SIZE &&
	       (blocksize & (blocksize - 1)) == 0;
}

static LONG read_block(struct IOStdReq *io, ULONG block, APTR buffer, ULONG blocksize)
{
	UQUAD offset = (UQUAD)block * blocksize;
	if (offset > 0xffffffffULL)
		return -1;

	io->io_Command = CMD_READ;
	io->io_Length = blocksize;
	io->io_Actual = 0;
	io->io_Offset = (ULONG)offset;
	io->io_Data = buffer;

	if (DoIO((struct IORequest *)io) != 0)
		return -1;
	return io->io_Actual == blocksize ? 0 : -1;
}


static ULONG read_rdb(struct IOStdReq *io, struct RigidDiskBlock *rdb)
{
	for (ULONG i = 0; i < RDB_LOCATION_LIMIT * 2; i++)
	{
		if (read_block(io, i, rdb, 512))
			break;

		if (rdb->rdb_ID == IDNAME_RIGIDDISK)
			return 1;
	}

	return 0;
}


static void read_partitions(struct IOStdReq *io, struct PartitionBlock *pb, ULONG blocknum, ULONG blocksize, Object *listview)
{
	for (ULONG count = 0; blocknum != 0xffffffffUL && count < MAX_RDB_PARTITIONS; count++)
	{
		struct PartInfo info;
		struct DosEnvec *env;

		if (read_block(io, blocknum, pb, blocksize))
			break;

		if (pb->pb_ID != IDNAME_PARTITION)
			break;

		env = (struct DosEnvec *)&pb->pb_Environment;

		// We need fields through de_HighCyl and a sane geometry.
		if (env->de_TableSize < DE_UPPERCYL ||
		    env->de_SizeBlock == 0 || env->de_Surfaces == 0 ||
		    env->de_BlocksPerTrack == 0 || env->de_HighCyl < env->de_LowCyl)
			break;

		UQUAD part_blocksize = (UQUAD)env->de_SizeBlock * 4;
		if (part_blocksize == 0 || part_blocksize > 0xffffffffULL)
			break;

		UQUAD blocks_per_cyl = (UQUAD)env->de_Surfaces * env->de_BlocksPerTrack;
		UQUAD cylinders = (UQUAD)env->de_HighCyl - env->de_LowCyl + 1;
		UQUAD start_block = (UQUAD)env->de_LowCyl * blocks_per_cyl;
		UQUAD block_count = cylinders * blocks_per_cyl;

		// The preferences format stores these fields as ULONG decimal values.
		if (start_block > 0xffffffffULL || block_count == 0 || block_count > 0xffffffffULL)
			break;

		memset(&info, 0, sizeof(info));
		ULONG name_len = (ULONG)(UBYTE)pb->pb_DriveName[0];
		if (name_len >= sizeof(info.name))
			name_len = sizeof(info.name) - 1;
		memcpy(info.name, &pb->pb_DriveName[1], name_len);
		info.name[name_len] = '\0';

		info.start_block = (ULONG)start_block;
		info.block_count = (ULONG)block_count;
		info.blocksize = (ULONG)part_blocksize;
		info.flags = pb->pb_DevFlags;

		UQUAD size_mb = (block_count * part_blocksize) / (1024 * 1024);
		if (size_mb >= 1024)
		{
			DOUBLE size_gb = (DOUBLE)size_mb / 1024.0;
			snprintf((char *)info.size, sizeof(info.size), GetLocaleString(MSG_CHOOSE_PARTITION_FORMAT_GB), size_gb);
		}
		else
		{
			snprintf((char *)info.size, sizeof(info.size), GetLocaleString(MSG_CHOOSE_PARTITION_FORMAT_MB), (ULONG)size_mb);
		}

		DoMethod(listview, MUIM_List_InsertSingle, &info, MUIV_List_Insert_Sorted);

		ULONG next = pb->pb_Next;
		if (next == blocknum)
			break;
		blocknum = next;
	}
}


void scan_rdb(CONST_STRPTR devname, ULONG unit, Object *listview)
{
	struct MsgPort *port = CreateMsgPort();
	if (!port)
		return;

	struct IOStdReq io;
	memset(&io, 0, sizeof(io));
	io.io_Message.mn_ReplyPort = port;
	io.io_Message.mn_Length = sizeof(io);

	if (!OpenDevice(devname, unit, (struct IORequest *)&io, 0))
	{
		UBYTE buffer[512];
		memset(buffer, 0, sizeof(buffer));

		if (read_rdb(&io, (struct RigidDiskBlock *)buffer))
		{
			struct RigidDiskBlock *rdb = (struct RigidDiskBlock *)buffer;
			ULONG blocksize = rdb->rdb_BlockBytes;

			if (valid_block_size(blocksize) && rdb->rdb_PartitionList != 0xffffffffUL)
			{
				struct PartitionBlock *pb = (struct PartitionBlock *)AllocMem(blocksize, MEMF_ANY);
				if (pb)
				{
					read_partitions(&io, pb, rdb->rdb_PartitionList, blocksize, listview);
					FreeMem(pb, blocksize);
				}
			}
		}

		CloseDevice((struct IORequest *)&io);
	}

	DeleteMsgPort(port);
}
