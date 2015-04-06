#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>

#define ERROR(msg, args...) fprintf(stderr, "error: " msg "\n", ##args)

#define SYNC_FRAME_LENGTH 116
#define SYNC_FRAME_LENGTH_TOL 10

enum phase {
	PHASE_SYNC=0,
	PHASE_DECODE,
};

struct state {
	enum phase phase;

	/* sync phase */
	int last; /* were we high or low */
	size_t offsets[30];
	size_t n_offsets;

	/* frame decode */
	int frame_length;
	char frame_samples[200]; /* FIXME HACK HACK this can overflow*/
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
	s->n_offsets = 0;
	s->phase = PHASE_SYNC;
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
				/* Failure; need to reset */
			}
		} else if (i == 9) {
			/* Stop bit */
			if (bit != 1) {
				/* Failure; need to reset */
			}
		} else {
			bits >>= 1;
			bits |= (bit << 7);
		}
	}

	/* We're done; use this byte */
	fprintf(stderr, "Got byte %c (%hhu) [%hhx]\n", bits, bits, bits);

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

bool
sync_is_enough(struct state *s, uint32_t *frame_length)
{
	int i;

	if (s->n_offsets < 2) {
		return false;
	}

	if (!is_within_tol(
		s->offsets[s->n_offsets-1] - s->offsets[s->n_offsets-2],
		SYNC_FRAME_LENGTH,
		SYNC_FRAME_LENGTH_TOL))
	{
		/* Basically clear everything and move the last offset to the
		 * first slot because it could be the first of a successful string of offsets
		 */
		s->offsets[0] = s->offsets[s->n_offsets-1];
		s->n_offsets = 1;
	}

	/* If we got here then all the offsets are good */
	if (s->n_offsets < 5) {
		return false;
	}

	/* Compute the average of the length */
	int average = 0;
	for (i = 0; i < s->n_offsets - 1; i++) {
		average += s->offsets[i+1] - s->offsets[i];
	}

	average /= s->n_offsets - 1;
	*frame_length = average;

	return true;
}

int
decode_sync(struct state *s, int b, size_t off)
{
	if (s->last && !b) {
		/* Transition from high to low */

		s->offsets[s->n_offsets++] = off;

		uint32_t frame_length;
		if (sync_is_enough(s, &frame_length)) {
			annotate(off-1, '!');
			enter_decode_frames(s, frame_length, off);
		}
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

	if (!open_annotation(flag_annotation_out_file)) {
		ERROR("failed to open annotation output");
		return false;
	}

	for (;;) {
		uint32_t thisval[1024];
		int result = read(STDIN_FILENO, &thisval, 1024);

		if (result == -1) {
			perror("read");
			exit(1);
		}
		if (result == 0) {
			break;
		}

		if ((result >> 2) << 2 != result) {
			fprintf(stderr, "expected not divisible by 4\n");
			exit(1);
		}

		int i;
		for (i = 0; i < result/4; i++) {
			if (thisval[i] & (1 << 15)) {
				decode(&s, 1, read_offset+i);
			} else {
				decode(&s, 0, read_offset+i);
			}
		}

		read_offset += result/4;
	}

	return 0;
}
