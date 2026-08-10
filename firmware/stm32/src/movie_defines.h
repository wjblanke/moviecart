
/*

   Format description

*/

#ifndef __DEFINES__
#define __DEFINES__

#include <stdint.h>
#include <stdbool.h>

#define FIELD_NUM_BLOCKS	8
#define FIELD_MAX_BLOCKS	6	// pal is 6, but ntsc is only 5, anything larger will require device with more RAM

// 3K
#define FIELD_SIZE			(512*FIELD_MAX_BLOCKS)

#endif
