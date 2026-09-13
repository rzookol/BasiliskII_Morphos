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

static LONG read_block(struct IOStdReq *io, ULONG block, APTR buffer, ULONG buffersize)
{
	io->io_Command = CMD_READ;
	io->io_Length = buffersize;
	io->io_Actual = 0;
	io->io_Offset = block * buffersize;
	io->io_Data = buffer;

	return DoIO((struct IORequest *)io);
}


static ULONG read_rdb(struct IOStdReq *io, struct RigidDiskBlock *rdb)
{
	ULONG i, rc;

	rc = 0;

	for (i = 0; i < RDB_LOCATION_LIMIT * 2; i++)
	{
		if (read_block(io, i, rdb, 512))
			break;

		if (rdb->rdb_ID == IDNAME_RIGIDDISK)
		{
			rc = 1;
			break;
		}
	}

	return rc;
}


static void read_partitions(struct IOStdReq *io, struct PartitionBlock *pb, ULONG blocknum, ULONG blocksize, ULONG blocks_per_cyl, Object *listview)
{
	for (;;)
	{
		struct PartInfo info;
		struct DosEnvec *env;
		UQUAD size;

		if (read_block(io, blocknum, pb, blocksize))
			break;

		if (pb->pb_ID != IDNAME_PARTITION)
			break;

		stccpy((char *)&info.name, (const char *)&pb->pb_DriveName[1], MIN((ULONG)(UBYTE)pb->pb_DriveName[0] + 1, sizeof(info.name)));

		env = (struct DosEnvec *)&pb->pb_Environment;

		info.lowcyl = env->de_LowCyl;
		info.highcyl = env->de_HighCyl;
		info.blocksize = env->de_SizeBlock * 4;
		info.flags = pb->pb_DevFlags;

		size  = env->de_HighCyl - env->de_LowCyl;
		size *= env->de_SizeBlock * blocks_per_cyl;
		size  = size * 4 / 1024 / 1024;

		if (size >= 1024)
		{
			DOUBLE s = size / 1024.f;

			snprintf((char *)&info.size, sizeof(info.size), GetLocaleString(MSG_CHOOSE_PARTITION_FORMAT_GB), s);
		}
		else
		{
			snprintf((char *)&info.size, sizeof(info.size), GetLocaleString(MSG_CHOOSE_PARTITION_FORMAT_MB), size);
		}

		DoMethod(listview, MUIM_List_InsertSingle, &info, MUIV_List_Insert_Sorted);

#if 0
		printf("found partition %s at block %ld\n", name, blocknum);
		printf("sizeblock: %ld\n", env->de_SizeBlock * 4);
		printf("lowcyl: %ld\n", env->de_LowCyl);
		printf("highcyl: %ld\n", env->de_HighCyl);
		printf("flags: %ld\n", pb->pb_DevFlags);
#endif

		blocknum++;
	}
}


void scan_rdb(CONST_STRPTR devname, ULONG unit, Object *listview)
{
	struct MsgPort *port;

	port = CreateMsgPort();

	if (port)
	{
		struct IOStdReq io;

		io.io_Message.mn_ReplyPort = port;
		io.io_Message.mn_Length = sizeof(io);

		if (!OpenDevice(devname, unit, (struct IORequest *)&io, 0))
		{
			UBYTE buffer[512];

			if (read_rdb(&io, (struct RigidDiskBlock *)&buffer))
			{
				ULONG blocksize, blocksize2, blocks_per_cyl;
				struct RigidDiskBlock *rdb;
				struct PartitionBlock *pb;

				rdb = (struct RigidDiskBlock *)&buffer;

				blocksize = rdb->rdb_BlockBytes;
				blocks_per_cyl = rdb->rdb_Sectors * rdb->rdb_Heads;
				blocksize2 = MIN(512, blocksize);

				pb = (struct PartitionBlock *)AllocMem(blocksize2, MEMF_ANY);

				if (pb)
				{
					read_partitions(&io, pb, rdb->rdb_PartitionList, blocksize, blocks_per_cyl, listview);

					FreeMem(pb, blocksize2);
				}
			}

			CloseDevice((struct IORequest *)&io);
		}

		DeleteMsgPort(port);
	}

}
