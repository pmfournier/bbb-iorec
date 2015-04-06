#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <getopt.h>
#include "log.h"

#define BUFFERED_INPUT_BUF_SIZE 65536

struct buffered_input {
	int fd;
	char *buf;
	ssize_t buf_len;
	size_t buf_size;
	size_t next;
};

int write_n_same(int fd, char c, size_t n)
{
	const size_t buf_size = 32768;
	char buf[buf_size];
	memset(buf, c, buf_size);

	while (n) {
		size_t n_this_iter = n;
		if (n_this_iter > buf_size) {
			n_this_iter = buf_size;
		}

		int result = write(fd, buf, n_this_iter);
		if (result == -1) {
			perror("write");
			return -1;
		}
		if (result == 0) {
			return 0;
		}

		n -= result;
	}

	return 1;
}

int
buffered_input_get_one_u32(struct buffered_input *bi, uint32_t *v)
{
	if (bi->next + sizeof(uint32_t) <= bi->buf_len) {
		*v = *(uint32_t *) &bi->buf[bi->next];
		bi->next += sizeof(uint32_t);
		return 1;
	}

	bi->buf_len = read(bi->fd, bi->buf, bi->buf_size);
	if (bi->buf_len == -1) {
		perror("read");
		return -1;
	} else if (bi->buf_len == 0) {
		return 0;
	}

	bi->next = sizeof(uint32_t);
	*v = *(uint32_t *) &bi->buf[0];
	return 1;
}

int
buffered_input_get_one(struct buffered_input *bi, char *b)
{
	if (bi->next < bi->buf_len) {
		*b = bi->buf[bi->next];
		bi->next++;
		return 1;
	}

	bi->buf_len = read(bi->fd, bi->buf, bi->buf_size);
	if (bi->buf_len == -1) {
		perror("read");
		return -1;
	} else if (bi->buf_len == 0) {
		return 0;
	}

	bi->next = 1;
	*b = bi->buf[0];
	return 1;
}

struct buffered_input *
buffered_input_create(int fd)
{
	struct buffered_input *bi = malloc(sizeof(*bi));
	if (bi == NULL) {
		ERROR("out of memory");
		return NULL;
	}

	bi->fd = fd;
	bi->buf_size = BUFFERED_INPUT_BUF_SIZE;
	bi->buf_len = 0;
	bi->next = 0;
	bi->buf = malloc(bi->buf_size);
	if (bi->buf == NULL) {
		ERROR("out of memory");
		return NULL;
	}

	return bi;
}

void output_compress(int fd_data_in, int fd_data_out, int fd_ann_in, int fd_ann_out)
{
	int result;
	int i;

	struct buffered_input *ann_in = buffered_input_create(fd_ann_in);
	if (ann_in == NULL) {
		ERROR("failed to create ann_in");
		abort();
	}
	struct buffered_input *data_in = buffered_input_create(fd_data_in);
	if (data_in == NULL) {
		ERROR("failed to create data_in");
		abort();
	}


	size_t data_counter_read = 0;
	size_t data_counter_write = 0;

	/* This value -1 is the location of the next annotation */
	size_t annotation_counter_read = 0;
	size_t annotation_counter_write = 0;

	char next_annotation;
	bool get_new_annotation = false;
	size_t same_data_count = 0;
	char previous_data_val = 0;
	bool verbose_mode = false;


	for (;;) {
		/* Ok get the next annotation */
		for (;;) {
			result = buffered_input_get_one(ann_in, &next_annotation);
			if (result == -1) {
				ERROR("error reading annotations");
				abort();
			} else if (result == 0) {
				annotation_counter_read += 1048576;
			}

			annotation_counter_read++;

			if (next_annotation || annotation_counter_read - data_counter_read >= 1048576) {
				break;
			}
		}

		/* Ok we have the next annotation, now read data until we get to its point in the data */
		for (;;) {
			uint32_t dr;

			result = buffered_input_get_one_u32(data_in, &dr);
			if (result == -1) {
				ERROR("error reading data");
				abort();
			} else if (result == 0) {
				/* FIXME check clean ending */
				return;
			}
			char d = ((dr & (1 << 15)) > 0);

			if (d != previous_data_val) {
				if (same_data_count > 10 && !verbose_mode) {
					/* Compressed print */
					char buf[100];
					char printable_char = previous_data_val ? '-' : '_';
					size_t n_printed =
						sprintf(buf, "%c%c%c%d%c%c%c",
							printable_char,
							printable_char,
							printable_char,
							same_data_count - 6,
							printable_char,
							printable_char,
							printable_char);
					write(fd_data_out, buf, n_printed);
					data_counter_write += n_printed;

					/* Write empty annotations to fill the space */
					write_n_same(fd_ann_out, ' ', n_printed);
					annotation_counter_write += n_printed;
				} else {
					char printable_char = previous_data_val ? '-' : '_';
					write_n_same(fd_data_out, printable_char, same_data_count);
					data_counter_write += same_data_count;
				}
				same_data_count = 0;

				/* We're at a transition so we reset the verbose until we see that we need it */
				verbose_mode = false;
			}

			same_data_count++;
			data_counter_read++;
			previous_data_val = d;

			/* Now that we've made a transition if needed, check if we have reached the next
			 * annotation point. WARNING: we are now past the incrementation stage so:
			 *  - previous_data_val represents the current value
			 *  - same_data_count is accurate
			 *  - data counter read is accurate
			 */
			if (data_counter_read == annotation_counter_read) {
				/* The corner case here is if we hit an annotation just after a transition.
				 * For this to work, we need to have access to the number of characters
				 * that were the same (1, or more if the annotation doesn't immediately
				 * follow a transition) and the character to print.
				 */
				if (next_annotation) {
					/* If the next annotation is zero, then we have a mandate to consume
					 * data, not print. We don't print what we have and we don't reset
					 * same_data_count. However the annotations are printed so that we
					 * can consume more.
					 */

					verbose_mode = true;

					///* dump what we have */
					//char printable_char = previous_data_val ? '-' : '_';
					//write_n_same(fd_data_out, printable_char, same_data_count);
					//data_counter_write += same_data_count;
					//same_data_count = 0;
				}

				/* We dump the annotation at this point */
				size_t n_blank_annotations = data_counter_write + same_data_count - annotation_counter_write - 1;
				const char blank_annotation = ' ';
				write_n_same(fd_ann_out, blank_annotation, n_blank_annotations);
				annotation_counter_write += n_blank_annotations;
				/* Write the actual annotation */
				write(fd_ann_out, &next_annotation, 1);
				annotation_counter_write += 1;

				/* Go back to the top to get a new annotation */
				get_new_annotation = true;
			}

			if (get_new_annotation == true) {
				get_new_annotation = false;
				break;
			}
		}

	}
}

void output_raw(int fd_data_in, int fd_data_out, int fd_ann_in, int fd_ann_out)
{
	char buf[1024];
	int i;

	for (;;) {
		int result = read(fd_data_in, buf, 1024);
		if (result == -1) {
			perror("read");
			abort();
		}
		if (result == 0) {
			break;
		}

		for (i = 0; i < 1024; i++) {
			if (buf[i] == 0) {
				buf[i] = '_';
			} else {
				buf[i] = '-';
			}
		}

		if (write(fd_data_out, buf, result) == -1) {
			perror("write");
			abort();
		}
	}

}

bool flag_raw = false;
char *flag_annotation_in_file = NULL;
char *flag_annotation_out_file = NULL;

void
usage(char *progname)
{
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "\t%s -h\n", progname);
	fprintf(stderr, "\t%s <FILE_IN >FILE_OUT\n", progname);
}

bool
parse_opt(int argc, char **argv)
{
	struct option opts[] = {
		{ "annotation-in", 1, NULL, 1 },
		{ "annotation-out", 1, NULL, 2 },
		{ "raw", 0, NULL, 3 },
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
		case 1: /* annotation-in */
			flag_annotation_in_file = optarg;
			break;
		case 2:
			flag_annotation_out_file = optarg;
			break;
		case 3:
			flag_raw = true;
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

int main(int argc, char **argv)
{
	if (!parse_opt(argc, argv)) {
		ERROR("failed to parse arguments");
		exit(1);
	}

	void (*output)(int, int, int, int);

	if (flag_raw) {
		output = output_raw;
	} else {
		output = output_compress;
	}

	int fd_ann_in, fd_ann_out;

	if (!flag_annotation_in_file) {
		flag_annotation_in_file = "/dev/zero";
	}
	if (!flag_annotation_out_file) {
		flag_annotation_out_file = "/dev/null";
	}

	if (!flag_annotation_out_file) {
		ERROR("annotation in file specified without out file");
		exit(1);
	}

	fd_ann_in = open(flag_annotation_in_file, O_RDONLY);
	if (fd_ann_in == -1) {
		perror("open");
		exit(1);
	}

	fd_ann_out = open(flag_annotation_out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd_ann_out == -1) {
		perror("open");
		exit(1);
	}

	output(STDIN_FILENO, STDOUT_FILENO, fd_ann_in, fd_ann_out);

	return 0;
}
