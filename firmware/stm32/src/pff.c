/*----------------------------------------------------------------------------/
/  Petit FatFs - FAT file system module  R0.03a
/-----------------------------------------------------------------------------/
/
/ Copyright (C) 2019, ChaN, all right reserved.
/
/ Petit FatFs module is an open source software. Redistribution and use of
/ Petit FatFs in source and binary forms, with or without modification, are
/ permitted provided that the following condition is met:
/
/ 1. Redistributions of source code must retain the above copyright notice,
/    this condition and the following disclaimer.
/
/ This software is provided by the copyright holder and contributors "AS IS"
/ and any warranties related to this software are DISCLAIMED.
/ The copyright owner or contributors be NOT LIABLE for any damages caused
/ by use of this software.
/-----------------------------------------------------------------------------/
/ Jun 15,'09  R0.01a  First release.
/
/ Dec 14,'09  R0.02   Added multiple code page support.
/                     Added write funciton.
/                     Changed stream read mode interface.
/ Dec 07,'10  R0.02a  Added some configuration options.
/                     Fixed fails to open objects with DBCS character.

/ Jun 10,'14  R0.03   Separated out configuration options to pffconf.h.
/                     Added _USE_LCC option.
/                     Added _FS_FAT16 option.
/
/ Jan 30,'19  R0.03a  Supported stdint.h for C99 and later.
/                     Removed _WORD_ACCESS option.
/                     Changed prefix of configuration options, _ to PF_.
/                     Added some code pages.
/                     Removed some code pages actually not valid.
/----------------------------------------------------------------------------*/

// Reduced greatly for moviecart application -> Rob Bairos 2021-2023

#include "mclog.h"
#include <string.h> // memcpy
#include "pff.h"		// Petit FatFs configurations and declarations 
#include "sd_reader.h"	// Declarations of low level disk I/O functions 
#include "core.h"
#include "moviecart_yield.h"
#include "defines.h"
#include "bus_service.h"	// emulate_cartridge() for MOUNT_PHASE bisection


//--------------------------------------------------------------------------
//   Module Private Definitions
//---------------------------------------------------------------------------

// FatFs refers the members in the FAT structures with byte offset instead
// of structure member because there are incompatibility of the packing option
// between various compilers. 

#define BS_jmpBoot			0
#define BS_OEMName			3
#define BPB_BytsPerSec		11
#define BPB_SecPerClus		13
#define BPB_RsvdSecCnt		14
#define BPB_NumFATs			16
#define BPB_RootEntCnt		17
#define BPB_TotSec16		19
#define BPB_Media			21
#define BPB_FATSz16			22
#define BPB_SecPerTrk		24
#define BPB_NumHeads		26
#define BPB_HiddSec			28
#define BPB_TotSec32		32
#define BS_55AA				510

#define BS_DrvNum			36
#define BS_BootSig			38
#define BS_VolID			39
#define BS_VolLab			43
#define BS_FilSysType		54

#define BPB_FATSz32			36
#define BPB_ExtFlags		40
#define BPB_FSVer			42
#define BPB_RootClus		44
#define BPB_FSInfo			48
#define BPB_BkBootSec		50
#define BS_DrvNum32			64
#define BS_BootSig32		66
#define BS_VolID32			67
#define BS_VolLab32			71
#define BS_FilSysType32		82

#define MBR_Table			446



__attribute__((section(".fileSystemInfo"))) struct fileSystemInfo	fsInfo;

//-----------------------------------------------------------------------
// Load multi-byte word in the FAT structure                             
//-----------------------------------------------------------------------

// little endian architecture
#if 0	// doesn't work, do src + dst need to be word aligned?
#define ld_word(X) ( *((uint16_t*)(X)) )
#define ld_dword(X) ( *((uint32_t*)(X)) )
#endif

bool pf_open_many( uint32_t *numFrames, int which);

uint16_t ld_word (const uint8_t* ptr)   /*   Load a 2-byte little-endian word */
{
    uint16_t rv;

    rv = ptr[1];
    rv = rv << 8 | ptr[0];
    return rv;
}

uint32_t ld_dword (const uint8_t* ptr) /* Load a 4-byte little-endian word */
{
    uint32_t rv;

    rv = ptr[3];
    rv = rv << 8 | ptr[2];
    rv = rv << 8 | ptr[1];
    rv = rv << 8 | ptr[0];

    return rv;
}


//-----------------------------------------------------------------------
// FAT access - Read value of a FAT entry                                
//-----------------------------------------------------------------------

uint32_t get_fat (
	uint32_t clst	// Cluster# to get the link information 
)
{
	uint8_t*	buf = disk_read_block1(fsInfo.fatbase + (clst >> 7));
	moviecart_bus_yield();
	uint16_t	offset =  (((uint16_t)clst & 127) << 2);

	return ld_dword(&buf[offset]) & 0x0FFFFFFF;
}

//-----------------------------------------------------------------------
// Mount/Unmount a Locical Drive                                         
//-----------------------------------------------------------------------


/* 64 KB: in SRAM1 rather than CCM, which also holds the stack. */
__attribute__((section(".sram1"))) static struct extentMap extentMap;

/*
 * FAT sectors read per CMD18 while mapping: 6 x 128 = 768 chain entries per
 * frame. Own buffer, because at boot both display buffers hold the title.
 */
#define FAT_BATCH		6
#define FAT_READ_ATTEMPTS	4

static uint8_t fatBatch[FAT_BATCH * 512]
	__attribute__((aligned(16), section(".sram1")));


bool pf_mount()
{
	uint8_t* buf;
	uint32_t bsect, fsize, tsect;

	// Check if the drive is ready or not 
	if (!mc_disk_initialize())
		return false;

#if MOVIECART_INIT_PHASE == 11
	emulate_cartridge();
#endif

#if MOVIECART_MOUNT_PHASE == 1
	emulate_cartridge();	/* card init only — no sector DMA yet */
#endif

	/* Search FAT partition. One fresh $F1C1 between each parse/DMA step. */
	bsect = 0;
	buf = disk_read_block1(bsect);
	mc_log("mount: sector 0 signature (want aa55)", ld_word(&buf[510]));
	moviecart_wait_ram_kernel_entry();

#if MOVIECART_INIT_PHASE == 12
	emulate_cartridge();
#endif

#if MOVIECART_MOUNT_PHASE == 2
	emulate_cartridge();
#endif

	if (ld_word(&buf[510]) != 0xAA55)
		return false;

	if (ld_word(&buf[BS_FilSysType32]) != 0x4146) {
		uint8_t *part = buf + MBR_Table;

		if (!part[4])
			return false;
		bsect = ld_dword(&part[8]);
		mc_log("mount: partition 1 boot sector", bsect);
		moviecart_wait_ram_kernel_entry();
#if MOVIECART_INIT_PHASE == 131
		emulate_cartridge();
#endif
		buf = disk_read_block1(bsect);
		moviecart_wait_ram_kernel_entry();
		if (ld_word(&buf[510]) != 0xAA55)
			return false;
		if (ld_word(&buf[BS_FilSysType32]) != 0x4146)
			return false;
	}

	moviecart_wait_ram_kernel_entry();
#if MOVIECART_INIT_PHASE == 131
	emulate_cartridge();
#endif
#if MOVIECART_INIT_PHASE == 132
	emulate_cartridge();
#endif

	moviecart_wait_ram_kernel_entry();
	fsize = ld_word(buf + BPB_FATSz16);
	if (!fsize)
		fsize = ld_dword(buf + BPB_FATSz32);
	fsize *= buf[BPB_NumFATs];
	fsInfo.fatbase = bsect + ld_word(buf + BPB_RsvdSecCnt);
	fsInfo.n_rootdir = ld_word(buf + BPB_RootEntCnt);
#if MOVIECART_INIT_PHASE == 133
	emulate_cartridge();
#endif

	moviecart_wait_ram_kernel_entry();
	fsInfo.csize = buf[BPB_SecPerClus];
#if MOVIECART_INIT_PHASE == 1341
	emulate_cartridge();
#endif

	moviecart_wait_ram_kernel_entry();
	{
		uint8_t cs = fsInfo.csize;
		uint8_t bits = 0;

		if (cs) {
			cs >>= 1;
			while (cs) {
				bits++;
				cs >>= 1;
			}
		}
		fsInfo.csize_bits = bits;
	}
#if MOVIECART_INIT_PHASE == 1342
	emulate_cartridge();
#endif

	moviecart_wait_ram_kernel_entry();
	fsInfo.csize = (uint8_t)(1u << fsInfo.csize_bits);
#if MOVIECART_INIT_PHASE == 1343
	emulate_cartridge();
#endif

	moviecart_wait_ram_kernel_entry();
	fsInfo.csize_mask = fsInfo.csize - 1;
#if MOVIECART_INIT_PHASE == 134
	emulate_cartridge();
#endif

	moviecart_wait_ram_kernel_entry();
	tsect = ld_word(buf + BPB_TotSec16);
	if (!tsect)
		tsect = ld_dword(buf + BPB_TotSec32);
	fsInfo.n_fatent = ((tsect - ld_word(buf + BPB_RsvdSecCnt) - fsize -
			    (fsInfo.n_rootdir >> 4)) >> fsInfo.csize_bits) + 2;
	fsInfo.dirbase = ld_dword(buf + BPB_RootClus);
	fsInfo.database = fsInfo.fatbase + fsize + (fsInfo.n_rootdir >> 4);

	mc_log("mount: FAT32 clusters (need >= 0xfff7)", fsInfo.n_fatent);
	mc_log("mount: sectors per cluster", fsInfo.csize);
	if (fsInfo.n_fatent >= 0xFFF7)
		return true;

	return false;
}

//-----------------------------------------------------------------------
// Seek File R/W Pointer                                                 
//-----------------------------------------------------------------------


/*
 * Map the open file's whole cluster chain into extentMap, once, at open time.
 *
 * Reads FAT_BATCH consecutive FAT sectors at a time with one async CMD18: it
 * is started in one RamKernel window and drained in the next, exactly like a
 * field read, so the transfer runs while the visible kernel is served. Each
 * window then walks the chain as far as the loaded sectors reach and starts
 * the next read, so a contiguous file costs about one frame per 768 clusters.
 */
uint8_t pf_map_file(void)
{
	uint32_t	cluster_bytes_bits = 9u + fsInfo.csize_bits;
	uint32_t	total = (fsInfo.fsize + (1u << cluster_bytes_bits) - 1u)
				>> cluster_bytes_bits;
	uint32_t	clust = fsInfo.org_clust;
	uint32_t	idx = 0;		// file cluster index of clust
	uint32_t	lo = 0, cnt = 0;	// FAT sectors held in fatBatch
	int		attempts = 0;

	extentMap.runs = 0;
	extentMap.total = 0;
	if (!total || clust < 2 || clust >= fsInfo.n_fatent)
		return PF_MAP_CHAIN;

	extentMap.file[0] = 0;
	extentMap.card[0] = clust;
	extentMap.runs = 1;

	moviecart_wait_ram_kernel_entry();
	for (;;)
	{
		uint32_t sect = 0;

		// Follow the chain as far as the loaded FAT sectors reach.
		while (idx + 1u < total)
		{
			sect = fsInfo.fatbase + (clust >> 7);
			if (sect < lo || sect >= lo + cnt)
				break;

			uint32_t next = ld_dword(&fatBatch[((sect - lo) << 9) +
					((clust & 127u) << 2)]) & 0x0FFFFFFF;

			if (next < 2 || next >= fsInfo.n_fatent)
				return PF_MAP_CHAIN;
			idx++;
			if (next != clust + 1u)
			{
				if (extentMap.runs == EXTENT_MAX)
					return PF_MAP_FRAGMENTED;
				extentMap.file[extentMap.runs] = idx;
				extentMap.card[extentMap.runs] = next;
				extentMap.runs++;
			}
			clust = next;
		}
		if (idx + 1u >= total)
			break;

		// Load the FAT sectors from sect on; the DMA crosses one visible kernel.
		mc_sdio_gate_relaxed = 1;
		bool ok = disk_read_blocks_begin(sect, fatBatch, FAT_BATCH);
		mc_sdio_gate_relaxed = 0;
		moviecart_wait_ram_kernel_entry();
		if (ok)
		{
			mc_sdio_gate_relaxed = 1;
			ok = disk_read_blocks_finish();
			mc_sdio_gate_relaxed = 0;
		}
		if (!ok)
		{
			cnt = 0;
			if (++attempts >= FAT_READ_ATTEMPTS)
				return PF_MAP_READ;
			continue;
		}
		attempts = 0;
		lo = sect;
		cnt = FAT_BATCH;
	}

	extentMap.total = total;
	return PF_MAP_OK;
}

/*
 * Point the file at a block: find the run holding its cluster (binary search
 * of extentMap) and compute the sector. No FAT access.
 */
bool pf_seek_block (
	uint32_t block		// File pointer from top of file 
)
{
	uint32_t	target = block >> fsInfo.csize_bits;	// file cluster index
	uint32_t	lo = 0, hi = extentMap.runs;

	if (!hi || target >= extentMap.total)
		return false;

	// Last run whose first file cluster is <= target.
	while (hi - lo > 1u)
	{
		uint32_t mid = (lo + hi) >> 1;

		if (extentMap.file[mid] <= target)
			lo = mid;
		else
			hi = mid;
	}

	uint32_t clust = extentMap.card[lo] + (target - extentMap.file[lo]);

	fsInfo.block = target << fsInfo.csize_bits;
	fsInfo.curr_clust = clust;
	fsInfo.file_block = clust2sect(clust) + (block & fsInfo.csize_mask);

	return true;
}

//-----------------------------------------------------------------------
// First regular non-deleted file										 
//-----------------------------------------------------------------------


bool
pf_read_block(uint8_t *dst)
{
	return disk_read_block2(fsInfo.file_block++, dst);
}

bool
pf_read_blocks(uint8_t *dst, uint8_t count)
{
	/*
	 * CMD18 can only cross physically consecutive LBAs. Do not cross a FAT
	 * cluster boundary: the next cluster may be fragmented elsewhere.
	 * MovieCart fields begin every eight sectors, so normal >=4 KiB FAT32
	 * clusters contain each 1..6-sector field in one contiguous run.
	 */
	uint32_t cluster_offset =
		(fsInfo.file_block - fsInfo.database) & fsInfo.csize_mask;

	if (!count || count > fsInfo.csize - cluster_offset)
		return false;

	if (!disk_read_blocks(fsInfo.file_block, dst, count))
		return false;

	fsInfo.file_block += count;
	return true;
}

static uint8_t async_read_count;

bool
pf_read_blocks_begin(uint8_t *dst, uint8_t count)
{
	uint32_t cluster_offset =
		(fsInfo.file_block - fsInfo.database) & fsInfo.csize_mask;

	if (async_read_count || !count ||
	    count > fsInfo.csize - cluster_offset)
		return false;
	if (!disk_read_blocks_begin(fsInfo.file_block, dst, count))
		return false;

	async_read_count = count;
	return true;
}

bool
pf_read_blocks_finish(void)
{
	if (!async_read_count)
		return false;

	uint8_t count = async_read_count;
	async_read_count = 0;
	if (!disk_read_blocks_finish())
		return false;

	fsInfo.file_block += count;
	return true;
}

/* The absolute sector the next pf_read_block() will fetch (valid after a seek). */
uint32_t
pf_current_sector(void)
{
	return fsInfo.file_block;
}

/*
 * A movie file: 8.3 extension MVC and at least one field long. Skips names
 * starting with '.' or '_': macOS writes .DS_Store and an AppleDouble
 * "._name" file (short name "_NAME~1.MVC", one field long) next to every file
 * it copies, and either would end playback with a header fault.
 */
static bool
is_movie_entry(const uint8_t *dir)
{
	if (dir[DIR_Name] == '.' || dir[DIR_Name] == '_')
		return false;
	if (dir[DIR_Name + 8] != 'M' || dir[DIR_Name + 9] != 'V' ||
	    dir[DIR_Name + 10] != 'C')
		return false;
	return ld_dword(dir + DIR_FileSize) >= FIELD_NUM_BLOCKS * 512u;
}

bool
pf_open_file( uint32_t *numFrames, int num)
{
	bool		res = true;
	uint8_t		c;
	uint8_t*	buf;
	uint8_t*	dir;

	uint16_t	dir_index = 0;			// Current read/write index number 
	uint32_t	dir_clust = fsInfo.dirbase;	// Current cluster 
	uint32_t	dir_sect = clust2sect(dir_clust);	// Current sector 

	int			found = 0;

	*numFrames = 0;

	moviecart_wait_ram_kernel_entry();
	do 
	{
		moviecart_wait_ram_kernel_entry();
		buf = disk_read_block1(dir_sect);
		moviecart_bus_yield();
		dir = &buf[(dir_index & 15) << 5];

		c = dir[DIR_Name];	// First character 

		// Reached to end of table 
		if (c == 0)
		{
			break;
		}

		if (c != 0xe5)	// not deleted
		{
			/* Regular file: allow ARC plus optional RDO/HID (Windows often sets those). */
			if (!(dir[DIR_Attr] & (AM_DIR | AM_VOL | AM_LFN)) &&
			    ((dir[DIR_Attr] & AM_ARC) || (dir[DIR_Attr] == 0x00)) &&
			    is_movie_entry(dir))
			{
				// File start cluster 
				uint8_t	*b = (uint8_t*)&fsInfo.org_clust;

				// little endian architecture
				b[3] = dir[DIR_FstClusHI + 1];
				b[2] = dir[DIR_FstClusHI + 0];
				b[1] = dir[DIR_FstClusLO + 1];
				b[0] = dir[DIR_FstClusLO + 0];

				fsInfo.fsize = ld_dword(dir+DIR_FileSize);	// File size 
				*numFrames = fsInfo.fsize / (FIELD_NUM_BLOCKS * 512);

				fsInfo.block = 0;						// File pointer 
				fsInfo.curr_clust = fsInfo.org_clust;

				// store 8.3 name
				memcpy(fsInfo.name, &dir[DIR_Name], 11);

				found++;
				if (found >= num)
					break;
			}
		}

		moviecart_bus_yield();

		// Next entry 
		dir_index++;
		if (!dir_index || !dir_sect) // Report EOT when index has reached 65535 
		{
			res = false;	
		}
		else if (!(dir_index & 15)) // Sector changed? 
		{
			dir_sect++;			// Next sector 

			// Static table 
			if (dir_clust == 0)
			{
				if (dir_index >= fsInfo.n_rootdir)
					res = false;	// Report EOT when end of table 
			}
			else // Dynamic table 
			{
				// Cluster changed? 
				if (((dir_index >> 4) & fsInfo.csize_mask) == 0)
				{
					dir_clust = get_fat(dir_clust);		// Get next cluster 
					if (dir_clust >= fsInfo.n_fatent)
						res = false;			// Report EOT when it reached end of dynamic table 
					else
						dir_sect = clust2sect(dir_clust);
				}
			}
		}

	} while (res == true);

	// The caller maps the chosen file with pf_map_file() before seeking.
	extentMap.runs = 0;
	extentMap.total = 0;
	return (found == num);
}
