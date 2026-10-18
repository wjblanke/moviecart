#ifndef HOST_DISKIO_H
#define HOST_DISKIO_H
#include <stdint.h>
#include <stddef.h>
typedef uint8_t BYTE;
typedef uint32_t DWORD;		/* 32-bit, as on the Cortex-M4 */
typedef unsigned int UINT;
typedef BYTE DSTATUS;
typedef enum { RES_OK = 0, RES_ERROR, RES_WRPRT, RES_NOTRDY, RES_PARERR } DRESULT;
#endif
