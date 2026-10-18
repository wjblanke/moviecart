/*---------------------------------------------------------------------------/
/  Petit FatFs - FAT file system module include file  R0.03a
/----------------------------------------------------------------------------/
/ Petit FatFs module is an open source software to implement FAT file system to
/ small embedded systems. This is a free software and is opened for education,
/ research and commercial developments under license policy of following trems.
/
/  Copyright (C) 2019, ChaN, all right reserved.
/
/ * The Petit FatFs module is a free software and there is NO WARRANTY.
/ * No restriction on use. You can use, modify and redistribute it for
/   personal, non-profit or commercial use UNDER YOUR RESPONSIBILITY.
/ * Redistributions of source code must retain the above copyright notice.
/
/ Adapted from above for MovieCart use
/----------------------------------------------------------------------------*/

#ifndef PF_DEFINED
#define PF_DEFINED

#include <stdint.h>
#include <stdbool.h>

/*
 * The open file's cluster chain as runs of consecutive clusters, built once by
 * pf_map_file() so a seek during playback is arithmetic with no FAT access.
 * EXTENT_MAX runs take 64 KB of SRAM1; a file copied to a freshly formatted
 * card is usually a single run.
 */
#define EXTENT_MAX		8192

/* pf_map_file() results */
#define PF_MAP_OK		0
#define PF_MAP_READ		1	/* a FAT read failed after retries */
#define PF_MAP_CHAIN		2	/* chain ends early or leaves the FAT */
#define PF_MAP_FRAGMENTED	3	/* more than EXTENT_MAX runs */

#define	DIR_Name			0
#define	DIR_Attr			11
#define	DIR_NTres			12
#define	DIR_CrtTime			14
#define	DIR_CrtDate			16
#define	DIR_FstClusHI		20
#define	DIR_WrtTime			22
#define	DIR_WrtDate			24
#define	DIR_FstClusLO		26
#define	DIR_FileSize		28

// File attribute bits for directory entry 

#define	AM_RDO	0x01	// Read only 
#define	AM_HID	0x02	// Hidden 
#define	AM_SYS	0x04	// System 
#define	AM_VOL	0x08	// Volume label 
#define AM_LFN	0x0F	// LFN entry 
#define AM_DIR	0x10	// Directory 
#define AM_ARC	0x20	// Archive 
#define AM_MASK	0x3F	// Mask of defined bits 
						//
struct extentMap
{
    uint32_t    file[EXTENT_MAX];	// first file cluster index of each run
    uint32_t    card[EXTENT_MAX];	// card cluster where that run starts
    uint32_t    runs;			// runs in use
    uint32_t    total;			// clusters in the file
};

bool		pf_mount();							/* Mount/Unmount a logical drive */
bool		pf_open_file(uint32_t *numFrames, int num);	/* Open first 'num' archived non-deleted file */
uint8_t		pf_map_file(void);			/* Map the open file's clusters; PF_MAP_* */
bool		pf_seek_block(uint32_t block);		/* Move file pointer; needs pf_map_file() */
bool        pf_read_block(uint8_t *dst);		/* Read full block, false on read failure */
bool        pf_read_blocks(uint8_t *dst, uint8_t count); /* One contiguous multi-block read */
bool        pf_read_blocks_begin(uint8_t *dst, uint8_t count);
bool        pf_read_blocks_finish(void);
uint32_t    pf_current_sector(void);			/* Absolute sector the next read will fetch */

uint32_t ld_dword (const uint8_t* ptr);
uint32_t get_fat (uint32_t clst);   // Cluster# to get the link information


//-----------------------------------------------------------------------
// Get sector# from cluster# / Get cluster field from directory entry
//-----------------------------------------------------------------------

#define clust2sect(X) ((((X) - 2) << fsInfo.csize_bits) + fsInfo.database)

// File system object structure 

struct fileSystemInfo
{
    uint8_t		name[11];	// 8.3 format
        
	uint8_t		csize;		// Number of sectors per cluster N
	uint8_t		csize_bits;	// number of bits 2^N
	uint8_t		csize_mask;	// 2^N-1 

	uint16_t	n_rootdir;	// Number of root directory entries (0 on FAT32) 
	uint32_t	n_fatent;	// Number of FAT entries (= number of clusters + 2) 
	uint32_t	fatbase;		// FAT start sector 
	uint32_t	dirbase;		// Root directory start sector (Cluster# on FAT32) 
	uint32_t	database;	// Data start sector 
	uint32_t	fsize;		// File size 
	uint32_t	org_clust;	// File start cluster 
	uint32_t	curr_clust;	// File current cluster 
	uint32_t	block;		// Current block, start of cluster 
	uint32_t	file_block;	// block index within cluster 


};

#endif /* _PFATFS */
