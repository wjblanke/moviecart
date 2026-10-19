/*
 * MovieCart host test: runs the real firmware boot and playback code against a
 * FAT32 disk image and an emulated 6507, and checks every frame:
 *   - each field put on screen equals the original .mvc file's field
 *   - the graphics, colour and audio the 6507 is served match that field
 *   - frame line counts, HMOVE positions and input capture
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <dirent.h>

#define main fw_main
#include "main.c"
#undef main

#include "host.h"

#ifndef EXAMPLES_DIR
#define EXAMPLES_DIR "examples"		/* reference .mvc files */
#endif

extern struct fileSystemInfo fsInfo;
extern struct extentMap extentMap;
extern uint8_t mc_aud_pack[32];
extern unsigned host_stat_sync_reads, host_stat_async_reads, host_stat_field_reads;
extern unsigned host_stat_aborts, host_stat_injected;
extern unsigned host_stat_relaxed_field_calls, host_stat_unrelaxed_field_calls;

#define FIELD_BYTES	(FIELD_NUM_BLOCKS * 512)
#define OSD_ROWS	22	/* bottom rows the overlay may draw over */

static jmp_buf host_exit;
static char fail_msg[512];
static int fatal_code;
static long errors;

void
host_fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(fail_msg, sizeof fail_msg, fmt, ap);
	va_end(ap);
	longjmp(host_exit, 2);
}

void
host_fatal_blink(int code)
{
	fatal_code = code;
	longjmp(host_exit, 1);
}

static void
error(const char *fmt, ...)
{
	va_list ap;

	errors++;
	if (errors > 25)
		return;
	va_start(ap, fmt);
	printf("  ERROR: ");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
}

bool
host_is_field_buffer(const uint8_t *p)
{
	return (p >= mr_buffer1 && p < mr_buffer1 + FIELD_SIZE) ||
	       (p >= mr_buffer2 && p < mr_buffer2 + FIELD_SIZE);
}

/* ---- golden movie files ---- */

struct golden { char name[64]; uint8_t *data; size_t size; };
static struct golden goldens[32];
static int ngoldens;
static struct { char name[12]; int g; } golden_by_file[32];
static int nmapped;

static void
load_goldens(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	while (d && (e = readdir(d)) && ngoldens < 32) {
		size_t len = strlen(e->d_name);
		char path[512];
		FILE *f;

		if (len < 4 || strcmp(e->d_name + len - 4, ".mvc"))
			continue;
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		if (!(f = fopen(path, "rb")))
			continue;
		fseek(f, 0, SEEK_END);
		goldens[ngoldens].size = ftell(f);
		fseek(f, 0, SEEK_SET);
		goldens[ngoldens].data = malloc(goldens[ngoldens].size);
		fread(goldens[ngoldens].data, 1, goldens[ngoldens].size, f);
		fclose(f);
		snprintf(goldens[ngoldens].name, sizeof goldens[0].name, "%s", e->d_name);
		ngoldens++;
	}
	if (d)
		closedir(d);
}

/* Compare the parts of a field updateBuffer() leaves alone: header, and the
 * graphics, colour and background rows above the overlay area. Colour row 0
 * and background byte 0 are skipped: updateColor() blacks them out on purpose
 * ("square off edges"), as the PIC firmware does. */
static const char *
field_diff(const uint8_t *buf, const uint8_t *gfield)
{
	static uint8_t copy[FIELD_BYTES];
	struct frameInfo gi;

	memcpy(copy, gfield, FIELD_BYTES);
	gi.buffer = copy;
	frameInit(&gi);

	size_t hdr = (size_t)(gi.audioBuf - copy);
	unsigned rows = gi.visibleLines > OSD_ROWS ? gi.visibleLines - OSD_ROWS : 0;

	if (memcmp(buf, copy, hdr))
		return "header";
	if (memcmp(buf + (gi.graphBuf - copy), gi.graphBuf, 5u * rows))
		return "graphics";
	if (memcmp(buf + (gi.colorBuf - copy) + 5, gi.colorBuf + 5, 5u * rows - 5)) {
		static char why[160];
		const uint8_t *fb = buf + (gi.colorBuf - copy);
		for (unsigned i = 5; i < 5u * rows; i++)
			if (fb[i] != gi.colorBuf[i]) {
			{
				unsigned ndiff = 0, nbw = 0, nzero = 0;
				for (unsigned j = 5; j < 5u * rows; j++) {
					if (fb[j] == gi.colorBuf[j]) continue;
					ndiff++;
					if (fb[j] == (gi.colorBuf[j] & 0x0f)) nbw++;
					if (!fb[j]) nzero++;
				}
				snprintf(why, sizeof why, "colour (first at byte %u of %u: firmware %02x, file %02x; "
					 "%u differ, %u are hue-stripped, %u are zero)",
					 i, 5u * rows, fb[i], gi.colorBuf[i], ndiff, nbw, nzero);
			}
				break;
			}
		return why;
	}
	if (memcmp(buf + (gi.colorBKBuf - copy) + 1, gi.colorBKBuf + 1, rows - 1))
		return "background";
	return NULL;
}

static int
golden_for(const char *name83, uint32_t nframes, int32_t idx, const uint8_t *buf)
{
	for (int i = 0; i < nmapped; i++)
		if (!memcmp(golden_by_file[i].name, name83, 11))
			return golden_by_file[i].g;
	for (int g = 0; g < ngoldens; g++) {
		if (goldens[g].size / FIELD_BYTES != nframes)	/* same length, not just same intro */
			continue;
		if (!field_diff(buf, goldens[g].data + (size_t)idx * FIELD_BYTES)) {
			memcpy(golden_by_file[nmapped].name, name83, 11);
			golden_by_file[nmapped++].g = g;
			return g;
		}
	}
	return -1;
}

/* ---- field loads ---- */

static long pf;			/* frames since playback started (defined below) */
struct load { int32_t intended; char file[11]; uint32_t nframes; bool ok; unsigned seq; };
static struct load loads[2];
static unsigned checked_seq[2];
static long content_checks;

static int buf_index(const uint8_t *p) { return p >= mr_buffer2 && p < mr_buffer2 + FIELD_SIZE; }

void
host_on_field_read_begin(uint8_t *dst, uint32_t sector, unsigned count)
{
	struct load *l = &loads[buf_index(dst)];

	(void)sector; (void)count;
	l->intended = state.io_frameNumber;
	l->nframes = state.i_numFrames;
	memcpy(l->file, fsInfo.name, 11);
}

void
host_on_field_read_done(uint8_t *dst, bool ok)
{
	struct load *l = &loads[buf_index(dst)];

	l->ok = ok;
	l->seq++;
}

static void
check_displayed_content(int b)
{
	struct load *l = &loads[b];
	const uint8_t *buf = b ? mr_buffer2 : mr_buffer1;
	char name[12];

	memcpy(name, l->file, 11);
	name[11] = 0;
	if (!l->ok) {
		error("buffer %d shown although its last read failed (%s field %d)", b + 1, name, l->intended);
		return;
	}
	int g = golden_for(l->file, l->nframes, l->intended, buf);
	if (g < 0) {
		error("%s field %d on screen matches no original file", name, l->intended);
		return;
	}
	const char *diff = field_diff(buf, goldens[g].data + (size_t)l->intended * FIELD_BYTES);
	if (diff)
		error("%s field %d on screen: %s differs from %s", name, l->intended, diff, goldens[g].name);
	content_checks++;
}

/* ---- per-frame capture of what the 6507 is served ---- */

#define MAXSEQ 4096
static struct frameInfo snap;		/* displayed field at $F09D */
static uint8_t snap_audio[400];		/* its samples, copied: work between frames may change the buffer */
static uint8_t snap_pack[32];		/* the blanking-audio pack OsHead will copy */
static bool snap_after_swap;
static uint8_t last_index;
static long pack_checks;
static bool have_snap, in_visible;
static uint8_t grp[MAXSEQ], colu[MAXSEQ], bk[MAXSEQ], pfc[MAXSEQ], aud[MAXSEQ];
static int ngrp, ncolu, nbk, npf, naud, aud_oshead_at, aud_tail_at;
static long frames, served_checks, audio_checks, buzz_runs;
static uint64_t vsync_line;
static int prev_odd = -1;		/* parity of the field shown before this one */
static long same_parity;		/* frames whose parity repeated the previous one */
static long field_mismatch;		/* playback frames whose field parity is not the RAM kernel's */
static long title_mismatch;		/* the same, title frames */
static long line_hist[400];
static long hmove_hist[76];

static void
check_visible(void)
{
	unsigned pairs = snap.visibleLines / 2;
	static const int gorder[10] = { 1, 0, 3, 2, 4, 6, 5, 8, 7, 9 };
	static const int corder[10] = { 1, 0, 2, 3, 4, 6, 5, 7, 8, 9 };

	if ((unsigned)ngrp != pairs * 10 || (unsigned)ncolu != pairs * 10 ||
	    (unsigned)nbk != pairs * 2 || (unsigned)npf != pairs * 2) {
		error("frame %ld: visible kernel wrote %d GRP, %d COLUP, %d COLUBK, %d COLUPF for %u pairs",
		      frames, ngrp, ncolu, nbk, npf, pairs);
		return;
	}
	for (unsigned p = 0; p < pairs; p++) {
		for (int k = 0; k < 10; k++) {
			if (grp[p * 10 + k] != snap.graphBuf[p * 10 + gorder[k]]) {
				error("frame %ld pair %u: GRP write %d = %02x, field has %02x",
				      frames, p, k, grp[p * 10 + k], snap.graphBuf[p * 10 + gorder[k]]);
				return;
			}
			if (colu[p * 10 + k] != snap.colorBuf[p * 10 + corder[k]]) {
				error("frame %ld pair %u: COLUP write %d = %02x, field has %02x",
				      frames, p, k, colu[p * 10 + k], snap.colorBuf[p * 10 + corder[k]]);
				return;
			}
		}
		if (bk[p * 2] != snap.colorBKBuf[p * 2] || bk[p * 2 + 1] != 0 ||
		    pfc[p * 2] != snap.colorBKBuf[p * 2 + 1] || pfc[p * 2 + 1] != 0) {
			error("frame %ld pair %u: background colours differ from the field", frames, p);
			return;
		}
	}
	served_checks++;
}

static void
check_audio(void)
{
	unsigned vis = snap.visibleLines;
	int tail = naud - aud_tail_at;
	int n = (int)snap.totalLines - (int)vis - 6;

	if (n > 64)
		n = 64;
	if (aud_oshead_at != (int)vis || aud_tail_at != (int)vis + 6) {
		error("frame %ld: %d AUDV0 writes in the visible kernel, %d in OsHead (want %u and 6)",
		      frames, aud_oshead_at, aud_tail_at - aud_oshead_at, vis);
		return;
	}
	/* The RAM kernel toggles its own parity ($FB): 23 + 3 + 37 or 24 + 3 + 37. */
	if (tail != 63 && tail != 64) {
		error("frame %ld: RAM kernel played %d samples (want 63 or 64)", frames, tail);
		return;
	}
	for (int i = 0; i < naud; i++) {
		uint8_t want;

		if (i < (int)vis + 6) {
			want = snap_audio[i] & 0x0f;
		} else {
			int k = i - aud_tail_at;	/* RAM kernel plays the pack's nibbles */
			want = (k & 1) ? snap_pack[k / 2] >> 4 : snap_pack[k / 2] & 0x0f;
		}
		if ((aud[i] & 0x0f) != want) {
			const char *where = i < (int)vis ? "visible" : i < (int)vis + 6 ? "OsHead" : "RAM kernel";
			error("frame %ld: AUDV0 write %d (%s) = %x, field sample %d is %x",
			      frames, i, where, aud[i] & 0x0f, i, want);
			return;
		}
	}
	audio_checks++;

	/*
	 * Buzz: one level through the picture and OsHead, another through the
	 * RAM kernel, frame after frame (a 60 Hz square wave). Silence that a
	 * stale blanking pack breaks up looks like this.
	 */
	{
		static long run;
		int a = aud[0] & 0x0f, b = aud[aud_tail_at] & 0x0f;
		bool flat = a != b;

		for (int i = 0; flat && i < naud; i++)
			if ((aud[i] & 0x0f) != (i < aud_tail_at ? a : b))
				flat = false;
		run = flat ? run + 1 : 0;
		if (run == 3) {
			buzz_runs++;
			error("frame %ld: 60 Hz buzz, AUDV0 %x in the picture and %x in blanking for 3 frames",
			      frames, a, b);
		}
	}

	/* Right after a swap the pack must hold the new field's tail. */
	if (snap_after_swap && !memcmp(snap.buffer, "MVC", 3)) {
		for (int k = 0; k < 64; k++) {
			uint8_t got = (k & 1) ? snap_pack[k / 2] >> 4 : snap_pack[k / 2] & 0x0f;
			uint8_t want = k < n ? snap_audio[vis + 6 + k] & 0x0f : 0;
			if (got != want) {
				error("frame %ld: audio pack nibble %d = %x, field tail sample is %x", frames, k, got, want);
				return;
			}
		}
		pack_checks++;
	}
}

void
host_on_tia_write(uint8_t reg, uint8_t v, uint64_t cycle)
{
	switch (reg) {
	case 0x00:			/* VSYNC */
		if (v & 2) {
			uint64_t line = cycle / 76;
			if (vsync_line) {
				uint64_t n = line - vsync_line;
				line_hist[n < 400 ? n : 399]++;
				/* Last-commit kernel: visible + OsHead + 23/24 overscan + 3
				 * VSYNC + 37 VBLANK + the pad line, whatever the field parity.
				 * Reported in the histogram, not checked. */
			}
			vsync_line = line;
			if (have_snap) {
				if (prev_odd == (int)snap.odd)
					same_parity++;
				prev_odd = snap.odd;
			}
		}
		break;
	case 0x19: if (naud < MAXSEQ) aud[naud++] = v; break;
	case 0x1b: case 0x1c: if (in_visible && ngrp < MAXSEQ) grp[ngrp++] = v; break;
	case 0x06: case 0x07: if (in_visible && ncolu < MAXSEQ) colu[ncolu++] = v; break;
	case 0x09: if (in_visible && nbk < MAXSEQ) bk[nbk++] = v; break;
	case 0x08: if (in_visible && npf < MAXSEQ) pfc[npf++] = v; break;
	case 0x2a: if (in_visible) hmove_hist[cycle % 76]++; break;
	}
}

/* ---- scenario ---- */

enum { SCN_NONE, SCN_TRANSPORT };
static int scenario = SCN_TRANSPORT;
static long pf;			/* frames since playback started */
static long max_pf = 6000;
static long vf = -1;		/* frames since the first movie field was shown */
static uint8_t want_swcha, want_swchb, want_inpt4;
static char cur_file[12];
static int files_seen;
static int32_t last_shown = -1;
static long run_start_pf;
static int32_t run_first, run_last, run_step;
static bool run_open;

static void
flush_run(void)
{
	if (!run_open)
		return;
	if (run_first == run_last)
		printf("    pf %5ld: field %d\n", run_start_pf, run_first);
	else
		printf("    pf %5ld: fields %d..%d (step %+d)\n", run_start_pf, run_first, run_last, run_step);
	run_open = false;
}

static void
log_shown(int32_t idx)
{
	if (run_open && idx - run_last == run_step && run_step != 0) {
		run_last = idx;
	} else if (run_open && run_first == run_last && idx != run_last) {
		run_step = idx - run_last;
		run_last = idx;
	} else if (!run_open || idx != run_last) {
		flush_run();
		run_open = true;
		run_start_pf = pf;
		run_first = run_last = idx;
		run_step = 0;
	}
}

static void
scenario_inputs(void)
{
	uint8_t a = 0xff, b = 0xff, f = 0xff;

	if (scenario == SCN_TRANSPORT && vf >= 0) {
		if (vf >= 40 && vf < 45) f = 0x7f;		/* fire: pause */
		if (vf >= 100 && vf < 105) f = 0x7f;		/* fire: resume */
		if (vf >= 200 && vf < 400) a = 0x7f;		/* right: fast-forward */
		if (vf >= 500 && vf < 600) a = 0xbf;		/* left: rewind */
		if (vf >= 700 && vf < 705) b = 0xfe;		/* Reset switch */
		if (vf >= 900 && vf < 905) b = 0xfd;		/* Select switch */
	}
	host_swcha = want_swcha = a;
	host_swchb = want_swchb = b;
	host_inpt4 = want_inpt4 = f;
}

void
host_on_cart_read(uint16_t off)
{

	if (off == 0x09d) {				/* VisibleBars: a new frame */
		if (have_snap) {
			check_audio();
			frames++;
		}
		naud = ngrp = ncolu = nbk = npf = 0;
		aud_oshead_at = aud_tail_at = -1;
		snap = r_coreInfo.frameInfo;
		memcpy(snap_audio, snap.audioBuf, sizeof snap_audio);
		memcpy(snap_pack, mc_aud_pack, sizeof snap_pack);
		snap_after_swap = r_coreInfo.mr_bufferIndex != last_index;
		last_index = r_coreInfo.mr_bufferIndex;
		have_snap = true;
		in_visible = true;

		/* The RAM kernel set this frame's VBLANK from FIELD ($FB) bit 0;
		 * odd fields belong after VBLANK 37 (FIELD even). */
		if ((host_riot_read(0xfb) & 1) == snap.odd) {
			if (mc_playback_pipeline)
				field_mismatch++;
			else
				title_mismatch++;
		}

		if (mc_playback_pipeline) {
			int b = buf_index(snap.graphBuf);

			if (loads[b].seq != checked_seq[b]) {
				if (memcmp(cur_file, loads[b].file, 11)) {
					flush_run();
					memcpy(cur_file, loads[b].file, 11);
					cur_file[11] = 0;
					files_seen++;
					printf("  pf %5ld: playing %s (%u fields, %u cluster run%s)\n", pf, cur_file,
					       state.i_numFrames, extentMap.runs, extentMap.runs == 1 ? "" : "s");
				}
				checked_seq[b] = loads[b].seq;
				check_displayed_content(b);
				log_shown(loads[b].intended);
				if (vf < 0)
					vf = 0;
			}
			if (vf >= 0)
				vf++;
			scenario_inputs();
			pf++;
			if (pf >= max_pf)
				longjmp(host_exit, 3);
		}
	} else if (off == 0x160 && in_visible) {	/* end of the visible kernel */
		in_visible = false;
		aud_oshead_at = naud;
		check_visible();
	} else if (off == 0x1c1) {			/* OsHead RTS */
		aud_tail_at = naud;
		if (mc_playback_pipeline &&
		    (r_coreInfo.mr_swcha != want_swcha || r_coreInfo.mr_swchb != want_swchb ||
		     r_coreInfo.mr_inpt4 != want_inpt4))
			error("frame %ld: inputs captured as %02x/%02x/%02x, console had %02x/%02x/%02x",
			      frames, r_coreInfo.mr_swcha, r_coreInfo.mr_swchb, r_coreInfo.mr_inpt4,
			      want_swcha, want_swchb, want_inpt4);
	}
}

/* ---- main ---- */

int
main(int argc, char **argv)
{
	const char *img = NULL, *examples = EXAMPLES_DIR;
	int expect_fatal = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--max") && i + 1 < argc) max_pf = atol(argv[++i]);
		else if (!strcmp(argv[i], "--quiet-transport")) scenario = SCN_NONE;
		else if (!strcmp(argv[i], "--fail-begin") && i + 1 < argc) host_fail_begin_n = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--fail-finish") && i + 1 < argc) host_fail_finish_n = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--corrupt") && i + 1 < argc) host_corrupt_n = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--slow") && i + 2 < argc) {
			host_slow_n = atoi(argv[++i]);
			host_slow_frames = atoi(argv[++i]);
		}
		else if (!strcmp(argv[i], "--stuck") && i + 1 < argc) host_stuck_n = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--expect-fatal") && i + 1 < argc) expect_fatal = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--examples") && i + 1 < argc) examples = argv[++i];
		else img = argv[i];
	}
	if (!img || !(host_img = fopen(img, "rb"))) {
		fprintf(stderr, "usage: %s image [--examples DIR] [--max N] [--quiet-transport] "
			"[--fail-begin N] [--fail-finish N] [--corrupt N] [--slow N FRAMES] [--stuck N] [--expect-fatal CODE]\n", argv[0]);
		return 2;
	}
	load_goldens(examples);
	printf("image %s, %d reference movies\n", img, ngoldens);

	int how = setjmp(host_exit);
	if (!how)
		fw_main();
	flush_run();

	printf("\nresult: ");
	if (how == 1)
		printf("firmware stopped with failure blink %d at pf %ld (%s)\n", fatal_code, pf, cur_file);
	else if (how == 2)
		printf("harness stop: %s (pf %ld, 6507 pc %04x)\n", fail_msg, pf, host_cpu_pc);
	else
		printf("ran %ld playback frames\n", pf);

	printf("  frames %ld (playback %ld), files played %d\n", frames, pf, files_seen);
	printf("  checks passed: %ld field contents, %ld visible frames, %ld audio frames, %ld audio packs\n",
	       content_checks, served_checks, audio_checks, pack_checks);
	printf("  SD: %u field reads, %u async reads, %u sync reads, %u aborts, %u injected\n",
	       host_stat_field_reads, host_stat_async_reads, host_stat_sync_reads,
	       host_stat_aborts, host_stat_injected);
	printf("  frames a read finish waited past its window: %u\n", host_stat_wait_windows);
	printf("  field read calls in relaxed mode: %u, outside: %u\n",
	       host_stat_relaxed_field_calls, host_stat_unrelaxed_field_calls);
	printf("  lines per frame:");
	for (int i = 0; i < 400; i++)
		if (line_hist[i])
			printf(" %d x%ld", i, line_hist[i]);
	printf("\n  frames repeating the previous field's parity: %ld\n", same_parity);
	printf("  playback frames whose field parity is not the RAM kernel's: %ld\n", field_mismatch);
	printf("  title frames whose field parity is not the RAM kernel's: %ld\n", title_mismatch);
	printf("  HMOVE cycle in line:");
	for (int i = 0; i < 76; i++)
		if (hmove_hist[i])
			printf(" @%d x%ld", i, hmove_hist[i]);
	printf("\n  errors: %ld\n", errors);

	bool pass = !errors && (expect_fatal ? (how == 1 && fatal_code == expect_fatal) : how == 3);
	printf("%s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
