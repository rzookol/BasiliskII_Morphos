#ifndef __RDB_H__
#define __RDB_H__

struct PartInfo
{
	TEXT name[32];
	TEXT size[32];
	ULONG start_block;
	ULONG block_count;
	ULONG blocksize;
	ULONG flags;
};

void scan_rdb(CONST_STRPTR devname, ULONG unit, Object *listview);

#endif /* __RDB_H__ */
