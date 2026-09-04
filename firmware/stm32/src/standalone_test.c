#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "stm32f4xx.h"

#include "frame.h"
#include "movie_defines.h"
#include "pff.h"
#include "sd_reader.h"
#include "standalone_test.h"
#include "update.h"

void updateInit(void);

volatile uint32_t mc_test_step;
volatile uint32_t mc_test_error;
volatile uint32_t mc_test_detail;
volatile uint32_t mc_test_sector;
volatile uint32_t mc_test_num_frames;
volatile uint32_t mc_test_completed;
volatile uint8_t mc_test_visible_lines;
volatile uint8_t mc_test_field_blocks;

static uint8_t field_buffer1[FIELD_SIZE]
	__attribute__((aligned(16), section(".sram1")));
static uint8_t field_buffer2[FIELD_SIZE]
	__attribute__((aligned(16), section(".sram2")));
static struct stateVars test_state;

/*
 * This deliberately remains a real, non-inlined function in the debug build.
 * Break here to stop immediately before each operation.
 */
__attribute__((noinline)) void
moviecart_test_checkpoint(enum moviecart_test_step step)
{
	mc_test_step = (uint32_t)step;
	__asm volatile ("nop");
}

/*
 * Stop under the debugger instead of spinning. BKPT returns control to GDB;
 * if you continue past it, WFI parks the core until an interrupt.
 */
__attribute__((noinline, noreturn)) void
moviecart_test_halt(void)
{
	__asm volatile ("bkpt #0");
	for (;;)
		__asm volatile ("wfi");
}

static __attribute__((noreturn)) void
test_fail(enum moviecart_test_error error, uint32_t detail)
{
	mc_test_error = (uint32_t)error;
	mc_test_detail = detail;
	moviecart_test_checkpoint(MC_TEST_FAILED);
	moviecart_test_halt();
}

static void
require_valid_field(const uint8_t *buffer, const struct frameInfo *frame)
{
	if (memcmp(buffer, "MVC\0", 4) != 0)
		test_fail(MC_TEST_ERROR_HEADER, mc_test_sector);

	if (!frame->visibleLines || !frame->numBlocks ||
	    frame->numBlocks > FIELD_MAX_BLOCKS)
		test_fail(MC_TEST_ERROR_GEOMETRY,
			  ((uint32_t)frame->visibleLines << 8) | frame->numBlocks);
}

static void
dwt_init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

int
main(void)
{
	struct frameInfo frame1 = { .buffer = field_buffer1 };
	struct frameInfo frame2 = { .buffer = field_buffer2 };
	uint32_t field_block = FIELD_NUM_BLOCKS;
	uint32_t num_frames;

	mc_test_error = MC_TEST_ERROR_NONE;
	mc_test_detail = 0;
	mc_test_completed = 0;
	dwt_init();
	moviecart_test_checkpoint(MC_TEST_RESET);

	moviecart_test_checkpoint(MC_TEST_SD_PINS);
	mc_sd_pins_init();

	moviecart_test_checkpoint(MC_TEST_DISK_INITIALIZE);
	if (!mc_disk_initialize())
		test_fail(MC_TEST_ERROR_DISK_INITIALIZE, 0);

	moviecart_test_checkpoint(MC_TEST_MOUNT);
	if (!pf_mount())
		test_fail(MC_TEST_ERROR_MOUNT, 0);

	moviecart_test_checkpoint(MC_TEST_OPEN_FILE);
	if (!pf_open_file(&num_frames, 1))
		test_fail(MC_TEST_ERROR_OPEN_FILE, 1);
	mc_test_num_frames = num_frames;
	if (!mc_test_num_frames)
		test_fail(MC_TEST_ERROR_OPEN_FILE, 0);

	if (!pf_seek_block(field_block))
		test_fail(MC_TEST_ERROR_SEEK, field_block);
	mc_test_sector = pf_current_sector();

	moviecart_test_checkpoint(MC_TEST_READ_SCRATCH);
	disk_read_invalidate();
	if (memcmp(disk_read_block1(mc_test_sector), "MVC\0", 4) != 0)
		test_fail(MC_TEST_ERROR_HEADER, mc_test_sector);

	moviecart_test_checkpoint(MC_TEST_READ_SINGLE);
	if (!disk_read_block2(mc_test_sector, field_buffer1))
		test_fail(MC_TEST_ERROR_READ, mc_test_sector);
	frameInit(&frame1);
	require_valid_field(field_buffer1, &frame1);
	mc_test_visible_lines = frame1.visibleLines;
	mc_test_field_blocks = frame1.numBlocks;

	moviecart_test_checkpoint(MC_TEST_READ_MULTIPLE);
	if (!disk_read_blocks(mc_test_sector, field_buffer1,
			      mc_test_field_blocks))
		test_fail(MC_TEST_ERROR_READ, mc_test_sector);

	moviecart_test_checkpoint(MC_TEST_PF_READ_SINGLE);
	if (!pf_seek_block(field_block) || !pf_read_block(field_buffer1))
		test_fail(MC_TEST_ERROR_READ, field_block);

	moviecart_test_checkpoint(MC_TEST_PF_READ_MULTIPLE);
	if (!pf_seek_block(field_block) ||
	    !pf_read_blocks(field_buffer1, mc_test_field_blocks))
		test_fail(MC_TEST_ERROR_READ, field_block);

	moviecart_test_checkpoint(MC_TEST_FRAME_PARSE);
	frameInit(&frame1);
	require_valid_field(field_buffer1, &frame1);

	memset(&test_state, 0, sizeof(test_state));
	test_state.io_frameNumber = 1;
	test_state.i_numFrames = mc_test_num_frames;
	test_state.i_swcha = 0xff;
	test_state.i_swchb = 0xff;
	test_state.i_inpt4 = 0xff;
	test_state.i_inpt5 = 0xff;
	test_state.io_bits = STATE_PLAYING;

	moviecart_test_checkpoint(MC_TEST_UPDATE_INIT);
	updateInit();

	moviecart_test_checkpoint(MC_TEST_UPDATE_TRANSPORT);
	updateTransport(&test_state);

	moviecart_test_checkpoint(MC_TEST_UPDATE_BUFFER);
	updateBuffer(&test_state, &frame1);

	moviecart_test_checkpoint(MC_TEST_PREPARE_PLAYBACK);
	if (!mc_sd_prepare_playback_read())
		test_fail(MC_TEST_ERROR_PREPARE_PLAYBACK, 0);

	/* Exercise the production asynchronous field-read path independently. */
	if (!pf_seek_block(field_block))
		test_fail(MC_TEST_ERROR_SEEK, field_block);
	mc_test_sector = pf_current_sector();

	moviecart_test_checkpoint(MC_TEST_ASYNC_BEGIN);
	if (!pf_read_blocks_begin(field_buffer2, mc_test_field_blocks))
		test_fail(MC_TEST_ERROR_ASYNC_BEGIN, mc_test_sector);

	moviecart_test_checkpoint(MC_TEST_ASYNC_FINISH);
	if (!pf_read_blocks_finish())
		test_fail(MC_TEST_ERROR_ASYNC_FINISH, mc_test_sector);

	moviecart_test_checkpoint(MC_TEST_ASYNC_FRAME_PARSE);
	frameInit(&frame2);
	require_valid_field(field_buffer2, &frame2);
	updateBuffer(&test_state, &frame2);

	mc_test_completed = 1;
	moviecart_test_checkpoint(MC_TEST_COMPLETE);
	moviecart_test_halt();
}
