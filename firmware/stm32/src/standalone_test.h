#ifndef MOVIECART_STANDALONE_TEST_H
#define MOVIECART_STANDALONE_TEST_H

#include <stdint.h>

/*
 * Debugger-visible progress markers. Set a breakpoint on
 * moviecart_test_checkpoint() and inspect the mc_test_* globals.
 */
enum moviecart_test_step {
	MC_TEST_RESET = 0,
	MC_TEST_SD_PINS,
	MC_TEST_DISK_INITIALIZE,
	MC_TEST_MOUNT,
	MC_TEST_OPEN_FILE,
	MC_TEST_READ_SCRATCH,
	MC_TEST_READ_SINGLE,
	MC_TEST_READ_MULTIPLE,
	MC_TEST_PF_READ_SINGLE,
	MC_TEST_PF_READ_MULTIPLE,
	MC_TEST_FRAME_PARSE,
	MC_TEST_UPDATE_INIT,
	MC_TEST_UPDATE_TRANSPORT,
	MC_TEST_UPDATE_BUFFER,
	MC_TEST_PREPARE_PLAYBACK,
	MC_TEST_ASYNC_BEGIN,
	MC_TEST_ASYNC_FINISH,
	MC_TEST_ASYNC_FRAME_PARSE,
	MC_TEST_COMPLETE,
	MC_TEST_FAILED
};

enum moviecart_test_error {
	MC_TEST_ERROR_NONE = 0,
	MC_TEST_ERROR_DISK_INITIALIZE,
	MC_TEST_ERROR_MOUNT,
	MC_TEST_ERROR_OPEN_FILE,
	MC_TEST_ERROR_SEEK,
	MC_TEST_ERROR_READ,
	MC_TEST_ERROR_HEADER,
	MC_TEST_ERROR_GEOMETRY,
	MC_TEST_ERROR_PREPARE_PLAYBACK,
	MC_TEST_ERROR_ASYNC_BEGIN,
	MC_TEST_ERROR_ASYNC_FINISH
};

extern volatile uint32_t mc_test_step;
extern volatile uint32_t mc_test_error;
extern volatile uint32_t mc_test_detail;
extern volatile uint32_t mc_test_sector;
extern volatile uint32_t mc_test_num_frames;
extern volatile uint32_t mc_test_completed;
extern volatile uint8_t mc_test_visible_lines;
extern volatile uint8_t mc_test_field_blocks;

void moviecart_test_checkpoint(enum moviecart_test_step step);
void moviecart_test_halt(void) __attribute__((noreturn));

#endif
