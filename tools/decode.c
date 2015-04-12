#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include "bitinput.h"
#include "log.h"

#define SYNC_FRAME_LENGTH 81
#define SYNC_FRAME_LENGTH_TOL 5

int sync_frame_length = 81;
int sync_frame_length_tol = 5;

enum phase {
	PHASE_SYNC=0,
	PHASE_DECODE,
};

struct state {
	enum phase phase;

	/* sync phase */
	int last; /* were we high or low */
	int consecutive_highs;

	/* frame decode */
	int frame_length;
	char *frame_samples;
	size_t n_frame_samples;
	size_t frame_required_samples;
	off_t beginning_of_frame;
};

int annotation_fd = 0;

bool
open_annotation(const char *annotation_out_file)
{
	if (annotation_out_file == NULL) {
		annotation_out_file = "/dev/null";
	}
	annotation_fd = open(annotation_out_file, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	if (annotation_fd == -1) {
		perror("open");
		return false;
	}

	return true;
}

void
annotate(off_t offset, char c)
{
	if (lseek(annotation_fd, offset, SEEK_SET) == -1) {
		perror("seek");
		abort();
	}

	if (write(annotation_fd, &c, 1) == -1) {
		perror("write");
		abort();
	}
}

bool
is_within_tol(int v, int ref, int tol)
{
	return (v > ref - tol && v < ref + tol);
}

void
enter_sync(struct state *s)
{
	s->phase = PHASE_SYNC;
	s->consecutive_highs = 0;
}

void
enter_decode_frames(struct state *s, uint32_t frame_length, off_t off)
{
	s->phase = PHASE_DECODE;
	s->frame_length = frame_length;
	// FIXME HACK we want decode_frames() to see this first bit which
	// was already consumed so we add it here
	s->n_frame_samples = 1;
	s->frame_samples[0] = 0; // also part of this hack
	s->frame_required_samples = frame_length + frame_length / 8;
	s->beginning_of_frame = off;
}

int
decode_frames(struct state *s, int b, size_t off)
{
	/* FIXME: no boundary checks */
	int i,j;

	if (b != 0 && b!= 1) {
		abort();
	}
	s->frame_samples[s->n_frame_samples++] = b;

	if (s->n_frame_samples < s->frame_required_samples) {
		return;
	}

	/* Ok we have a full frame, decode bits */
	unsigned char bits = 0;

	for (i = 0; i < 10; i++) {
		size_t offset = s->frame_length * i / 10;
		int bit = 1;
		for (j = 0; j < 3; j++) {
			/* look at 3 samples; if any is low, consider the bit low */
			if (!s->frame_samples[offset + j]) {
				bit = 0;
			}
		}
		annotate(s->beginning_of_frame + offset, (bit)?'B':'b');

		if (i == 0) {
			/* Start bit */
			if (bit != 0) {
				ERROR("didn't find start bit, resetting sync");
				enter_sync(s);
				return 0;
			}
		} else if (i == 9) {
			/* Stop bit */
			if (bit != 1) {
				ERROR("didn't find stop bit, resetting sync");
				enter_sync(s);
				return 0;
			}
		} else {
			bits >>= 1;
			bits |= (bit << 7);
		}
	}

	/* We're done; use this byte */
	if (write(STDOUT_FILENO, &bits, 1) == -1) {
		perror("write");
		abort();
	}

	/* Now reset the state machine for the next frame */

	/* From the offset of the 9th (0-based) bit, search for a low sample */
	for (i = s->frame_length * 9 / 10 + 1; i < s->n_frame_samples; i++) {
		annotate(s->beginning_of_frame + i, '>');
		if (s->frame_samples[i] == 0) {
			annotate(s->beginning_of_frame + i, 'v');
			/* Found the beginning of the next frame. Reuse the samples */
			memmove(s->frame_samples, &s->frame_samples[i], sizeof(s->frame_samples[0]) * s->n_frame_samples - i);
			s->beginning_of_frame = s->beginning_of_frame + i;
			s->n_frame_samples = s->n_frame_samples - i;
			return 0;
		}
	}

	ERROR("couldn't find next frame");
	enter_sync(s);
	return 0;
}

int
decode_sync(struct state *s, int b, size_t off)
{
	if (s->last && !b) {
		/* Transition from high to low */

		if (s->consecutive_highs >= sync_frame_length * 2) {
			annotate(off-1, '!');
			enter_decode_frames(s, sync_frame_length, off);
		}

		s->consecutive_highs = 0;
	} else if (b) {
		s->consecutive_highs++;
	}

	s->last = b;
}

int decode(struct state *s, int b, size_t off)
{
	if (s->phase == PHASE_SYNC) {
		return decode_sync(s, b, off);
	} else if (s->phase == PHASE_DECODE) {
		return decode_frames(s, b, off);
	} else {
		ERROR("unknown phase %d", s->phase);
		return -1;
	}
}

void
usage(char *progname)
{
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "\t%s -h\n", progname);
	fprintf(stderr, "\t%s [ --annotation-out ANNOTATION_FILE ] <FILE_IN\n", progname);
}

char *flag_annotation_out_file = NULL;

bool
parse_opt(int argc, char **argv)
{
	struct option opts[] = {
		{ "annotation-out", 1, NULL, 1 },
		{ "help", 0, NULL, 'h' },
		{ "frame-length", 1, NULL, 'f' },
		{ "frame-length-tol", 1, NULL, 't' },
		{ NULL, 0, NULL, 0 },
	};

	for (;;) {
		int opt;
		opt = getopt_long(argc, argv, "h", opts, NULL);
		if (opt == -1) {
			/* Finished */
			break;
		}

		switch (opt) {
		case 1:
			flag_annotation_out_file = optarg;
			break;
		case 'f':
			sync_frame_length = atoi(optarg);
			break;
		case 't':
			sync_frame_length_tol = atoi(optarg);
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			return false;
		};
	}

	return true;
}

int
main(int argc, char **argv)
{
	struct state s;
	memset(&s, 0, sizeof(s));
	size_t read_offset = 0;

	if (!parse_opt(argc, argv)) {
		ERROR("failed to parse arguments");
		exit(1);
	}

	/* FIXME HACK HACK HACK: reserving twice the amount of memory is a terrible
	 * approximation.
	 */
	s.frame_samples = malloc(2 * sync_frame_length * sizeof(s.frame_samples[0]));
	if (s.frame_samples == 0) {
		ERROR("out of memory");
		exit(1);
	}

	if (!open_annotation(flag_annotation_out_file)) {
		ERROR("failed to open annotation output");
		return false;
	}

	struct bit_input *bi = bit_input_create(STDIN_FILENO);
	if (bi == NULL) {
		ERROR("failed to create bit input");
		abort();
	}

	for (;;) {
		int result;
		int d;

		result = bit_input_get(bi, &d);
		if (result == -1) {
			ERROR("error getting next bit");
			abort();
		} else if (result == 0) {
			break;
		}

		decode(&s, d, read_offset);

		read_offset++;
	}

	return 0;
}
