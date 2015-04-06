#ifndef BITINPUT_H
#define BITINPUT_H

#include <stdint.h>
#include "log.h"

#define BIT_INPUT_BUFFER_SIZE 1024

struct bit_input {
	uint32_t buf[1024];
	size_t next_idx;
	size_t next_bit;
	uint32_t cur_word;
	int fd;
};

static inline struct bit_input *bit_input_create(int fd)
{
	struct bit_input *bi = malloc(sizeof(*bi));
	if (bi == NULL) {
		ERROR("out of memory");
		return NULL;
	}

	memset(bi, 0, sizeof(*bi));

	bi->fd = fd;
	/* Trick the get function into reading from file at next request */
	bi->next_bit = 32;
	bi->next_idx = BIT_INPUT_BUFFER_SIZE;

	return bi;
}

static inline int
bit_input_get(struct bit_input *bi, int *b)
{
	int result;

	if (bi->next_bit == 32) {
		if (bi->next_idx == BIT_INPUT_BUFFER_SIZE) {
			result = read(bi->fd, bi->buf, BIT_INPUT_BUFFER_SIZE * sizeof(bi->buf[0]));
			if (result == -1) {
				perror("read");
				return -1;
			} else if (result == 0) {
				return 0;
			}

			bi->next_idx = 0;
		}

		bi->cur_word = bi->buf[bi->next_idx];
		bi->next_idx++;

		bi->next_bit = 0;
	}

	*b = bi->cur_word >> 31;
	bi->next_bit++;
	bi->cur_word <<= 1;

	return 1;
}

#endif /* BITINPUT_H */
