#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include "log.h"

int main()
{
	for (;;) {
		uint32_t thisval[1024];
		uint8_t output[256];
		int result = read(STDIN_FILENO, &thisval, 1024);
		int res;

		if (result == -1) {
			perror("read");
			abort();
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
			output[i] = ((thisval[i] & (1 << 15)) > 0) ? '-' : '_';
		}

		result = write(STDOUT_FILENO, output, 256);
		if (result == -1) {
			perror("write");
			abort();
		}
	}

	return 0;
}
