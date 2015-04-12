#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <prussdrv.h>
#include <pruss_intc_mapping.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <values.h>
#include <time.h>
#include <stdbool.h>
#include <signal.h>
#include <getopt.h>

#define ERROR(msg, args...) fprintf(stderr, "error: " msg "\n", ##args)
#define SIGSAFE_MSG(msg) write(STDERR_FILENO, msg, sizeof(msg))

#define PRU_NUM 0 /* which of the two PRUs are we using? */

bool flag_test_mode = 0;
int flag_capture_choke = 23;
const char *flag_out_file = NULL;
sig_atomic_t interrupt_requested = 0;

struct bit_output {
	uint32_t buf[4096];
	size_t next_idx;
	size_t next_bit;
	uint32_t cur_word;
	int fd;
};

struct bit_output *
bit_output_create(const char *filename)
{
	struct bit_output *bo = malloc(sizeof(*bo));
	if (bo == NULL) {
		ERROR("out of memory");
		return NULL;
	}

	memset(bo, 0, sizeof(*bo));

	bo->fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (bo->fd == -1) {
		free(bo);
		return NULL;
	}

	return bo;
}

static inline int
bit_output_add(struct bit_output *bo, int b)
{
	int result;

	bo->cur_word <<= 1;
	bo->cur_word |= b;
	bo->next_bit++;

	if (bo->next_bit == 32) {
		bo->buf[bo->next_idx] = bo->cur_word;
		bo->next_idx++;
		if (bo->next_idx == 4096) {
			result = write(bo->fd, bo->buf, 4096 * sizeof(bo->buf[0]));
			if (result == -1 || result == 0) {
				perror("write");
				return -1;
			}

			bo->next_idx = 0;
		}

		bo->cur_word = 0;
		bo->next_bit = 0;
	}

	return 0;
}

void
signal_handler(int sig)
{
	SIGSAFE_MSG("Got signal.\n");
	interrupt_requested = 1;
}

int setup_signal_handler(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));

	sa.sa_handler = signal_handler;

	if (sigemptyset(&sa.sa_mask) == -1) {
		ERROR("sigemptyset failed");
		return -1;
	}

	if (sigaction(SIGINT, &sa, NULL) == -1) {
		perror("sigaction");
		return -1;
	}

	return 0;
}

static inline uint64_t
clock_get_rel_time(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
		ERROR("clock_gettime() returned error?!");
		return 0;
	}

	uint64_t retval;
	retval = ts.tv_sec;
	retval *= 1000000000;
	retval += ts.tv_nsec;

	return retval;
}

int hex2void(const char *s, void **out)
{
	char *endptr;
	unsigned long val;

	errno = 0;
	val = strtoul(s, &endptr, 16);

	if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN))
	        || (errno != 0 && val == 0))
	{
		perror("strtol");
		return -1;
	}
	
	if (endptr == s) {
		fprintf(stderr, "No digits were found\n");
		return -1;
	}

	*out = (void *) val;

	return 0;
}

#define MEM_ADDR_FILE "/sys/class/uio/uio0/maps/map1/addr"
#define MEM_SIZE_FILE "/sys/class/uio/uio0/maps/map1/size"
int get_extmem_address_from_module(void **addr_out, uint32_t *size_out)
{
	FILE *fp = fopen(MEM_ADDR_FILE, "r");
	if (fp == NULL) {
		return -1;
	}

	char buf[20];

	if (fscanf(fp, "%s", buf) != 1) {
		return -1;
	}

	if (strlen(buf) < 3) {
		return -1;
	}

	if (buf[0] != '0' ||
		buf[1] != 'x')
	{
		return -1;
	}

	if (hex2void(&buf[2], addr_out) == -1) {
		return -1;
	}

	fclose(fp);

	/* Now get the length of the memory area */

	fp = fopen(MEM_SIZE_FILE, "r");
	if (fp == NULL) {
		return -1;
	}

	if (fscanf(fp, "%s", buf) != 1) {
		return -1;
	}

	if (strlen(buf) < 3) {
		return -1;
	}

	if (buf[0] != '0' ||
		buf[1] != 'x')
	{
		return -1;
	}

	void *size_out_void;
	if (hex2void(&buf[2], &size_out_void) == -1) {
		return -1;
	}
	*size_out = (uint32_t) size_out_void;

	return 0;
}

static int pru_setup(const char * const path)
{
	int rtn;

	if((rtn = prussdrv_exec_program(PRU_NUM, path)) < 0) {
		fprintf(stderr, "prussdrv_exec_program() failed\n");
		return rtn;
	}

	return rtn;
}

static int pru_cleanup(void)
{
   int rtn = 0;

   /* clear the event (if asserted) */
   if(prussdrv_pru_clear_event(PRU_EVTOUT_0, PRU0_ARM_INTERRUPT)) {
      fprintf(stderr, "prussdrv_pru_clear_event() failed\n");
      rtn = -1;
   }

   /* halt and disable the PRU (if running) */
   if((rtn = prussdrv_pru_disable(PRU_NUM)) != 0) {
      fprintf(stderr, "prussdrv_pru_disable() failed\n");
      rtn = -1;
   }

   /* release the PRU clocks and disable prussdrv module */
   if((rtn = prussdrv_exit()) != 0) {
      fprintf(stderr, "prussdrv_exit() failed\n");
      rtn = -1;
   }

   return rtn;
}

static void *get_pru_mem(void)
{
	void *mem;

	prussdrv_map_prumem (PRUSS0_PRU0_DATARAM, &mem);

	return mem;
}

static void *get_designated_ddr(void *extram)
{
	void *mem;
	int fd;
	
	fd = open("/dev/mem", O_RDWR | O_DSYNC);
	if (fd < 0) {
		printf("Failed to open /dev/mem (%s)\n", strerror(errno));
		exit(1);
	}

	mem = mmap(0, 0x0FFFFFFF, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)extram);
	if (mem == NULL) {
		printf("Failed to map the device (%s)\n", strerror(errno));
		close(fd);
		exit(1);
	}

	return mem;
}

int send_extmem_addr_to_pru(void *pru0_priv_mem, void *addr, size_t sz)
{
	void **pru_priv_mem = (void **) pru0_priv_mem;

	/* We are sending data to the PRU in two 32 bit slots at its address
	 * 0x0 (the beginning of its address space) The first slot (0x0) is the address
	 * of the buffer space, the second is the size of the buffer space (0x4).  The
	 * size of the buffer space is expressed as a power of 2.
	 */
	pru_priv_mem[2] = addr;
	((uint32_t *)pru_priv_mem)[3] = sz;
	((uint32_t *)pru_priv_mem)[4] = flag_capture_choke;

	return 0;
}

void usage(const char *progname)
{
	fprintf(stderr, "Usage: %s [ --test-mode ] [ --capture-choke=CHOKE ] [ OUTPUT_FILE ]\n", progname);
	fprintf(stderr, "       %s -h | --help\n", progname);
	fprintf(stderr, "\n");
	fprintf(stderr, "Sample data from the GPIO and it to OUTPUT_FILE\n");
}

bool
test_valid(void *mem, size_t len, size_t start_val)
{
	size_t i;
	for (i = 0; i < len; i += 4) {
		uint32_t *cur = (uint32_t *)(((uint8_t *) mem) + i);
		if (*cur != start_val + i) {
			ERROR("failed test - at offset %zu got %zu", start_val+i, *cur);
			/* Don't exit yet, this could be due to an overrun */
			return false;
		}
	}

	return true;
}

int run(void)
{
	bool overrun = false;

	if(geteuid()) {
		ERROR("must be run as root in order to access PRU");
		return -1;
	}

	/* initialize PRU */
	if(prussdrv_init() != 0) {
		ERROR("prussdrv_init() failed");
		return -1;
	}

	tpruss_intc_initdata intc = PRUSS_INTC_INITDATA;

	/* open the interrupt */
	if(prussdrv_open(PRU_EVTOUT_0) != 0) {
		ERROR("prussdrv_open() failed");
		return -1;
	}

	if (prussdrv_pru_reset(0) != 0) {
		ERROR("prussdrv_pru_reset() failed");
		return -1;
	}

	/* initialize interrupt */
	if(prussdrv_pruintc_init(&intc) != 0) {
		ERROR("prussdrv_pruintc_init() failed");
		return -1;
	}

	void *extmem_addr;
	size_t extmem_size;
	if (get_extmem_address_from_module(&extmem_addr, &extmem_size) == -1) {
		ERROR("failed to obtain extmem address");
		return -1;
	}

	if (extmem_size < 8388608) {
		ERROR("Buffer size is %zu, which is smaller than 8388608 bytes. Performance would suck.", extmem_size);
		return -1;
	}

	void *pru0_priv_mem = get_pru_mem();

	if (send_extmem_addr_to_pru(pru0_priv_mem, extmem_addr, extmem_size) == -1) {
		return -1;
	}

	void *ddrmem = get_designated_ddr(extmem_addr);
	if (ddrmem == NULL) {
		ERROR("failed to get designated ddr memory");
		return -1;
	}

	/* Open outfile */
	struct bit_output *bitout = NULL;
	if (flag_out_file) {
		bitout = bit_output_create(flag_out_file);
		if (bitout == NULL) {
			perror("open");
			return -1;
		}
	}

	/* initialize the library, PRU and interrupt; launch our PRU program */
	if (flag_test_mode) {
		if(pru_setup("./iorec-test.bin")) {
			pru_cleanup();
			return -1;
		}
	} else {
		if(pru_setup("./iorec.bin")) {
			pru_cleanup();
			return -1;
		}
	}

	uint64_t t1,t2;

	t1 = clock_get_rel_time();

	/* Address of the write counter which will be updated by the PRU
	 * Its value is in bytes.
	 */
	volatile uint32_t *before_write_counter_raw = &((volatile uint32_t *)pru0_priv_mem)[0];
	volatile uint32_t *after_write_counter_raw = &((volatile uint32_t *)pru0_priv_mem)[1];
	*after_write_counter_raw = 0;

	/* Do the acquisition */
	uint32_t read_counter = 0;
	uint32_t last_write_counter = 0;
	uint32_t polls = 0;
	uint32_t max_buffer_use = 0;
	for (;;) {
		if (interrupt_requested) {
			break;
		}

		polls++;

		uint32_t write_counter;
		write_counter = *after_write_counter_raw;


		/* Offsets, in bytes */
		off_t read_begin1 = last_write_counter % extmem_size;
		off_t read_end1 = write_counter % extmem_size;
		off_t read_begin2;
		off_t read_end2;
		if (read_begin1 > read_end1) {
			read_begin2 = 0;
			read_end2 = read_end1;
			read_end1 = extmem_size;
		} else {
			read_begin2 = read_end1;
			read_end2 = read_end1;
		}

		bool test_succeeded = true;
		int i;

		if (read_end1 - read_begin1) {
			if (bitout != NULL) {
				/* FIXME: look for partial write()s */
				for (i = read_begin1; i < read_end1; i+=4) {
					uint32_t v1 = *(uint32_t *) (((uint8_t *)ddrmem) + i);
					uint32_t v2 = (v1 >> 15) & 1;
					if (bit_output_add(bitout, v2) == -1) {
						perror("write");
						return -1;
					}
				}
			}

			if (flag_test_mode) {
				if (!test_valid(
					((uint8_t *)ddrmem) + read_begin1,
					read_end1 - read_begin1,
					read_counter))
				{
					test_succeeded = false;
				}
			}
		}
		if (read_end2 - read_begin2) {
			if (bitout != NULL) {
				for (i = read_begin2; i < read_end2; i+=4) {
					uint32_t v1 = *(uint32_t *) (((uint8_t *)ddrmem) + i);
					uint32_t v2 = (v1 >> 15) & 1;
					if (bit_output_add(bitout, v2) == -1) {
						perror("write");
						return -1;
					}
				}
			}

			if (flag_test_mode) {
				if (!test_valid(
					((uint8_t *)ddrmem) + read_begin2,
					read_end2 - read_begin2,
					read_counter + read_end1 - read_begin1))
				{
					test_succeeded = false;
				}
			}
		}

		if (write_counter - last_write_counter > max_buffer_use) {
			max_buffer_use = write_counter - last_write_counter;
		}

		asm volatile("" ::: "memory");
		uint32_t before_write_counter = *before_write_counter_raw;

		if (before_write_counter > last_write_counter + extmem_size) {
			ERROR("buffer overrun, diff is %" PRIu32, write_counter - last_write_counter);
			overrun = true;
			break;
		}

		if (!test_succeeded) {
			ERROR("well the before write counter is %" PRIu32 ", the last_write_counter is %" PRIu32 " and extmem_size is %" PRIu32, write_counter, last_write_counter, extmem_size);
			exit(1);
		}
		
		last_write_counter = write_counter;

		read_counter = last_write_counter;

	}

	t2 = clock_get_rel_time();

	printf("Summary: %" PRIu32 " bytes read in %f sec\n", read_counter, ((double)(t2-t1))/1000000000);
	printf("         That's %.2f MB/second transferred from the PRU\n", ((double)read_counter)/(((double)(t2-t1))/1000));
	printf("         That's %" PRIu32 " bytes/poll\n", read_counter/polls);
	printf("         The max amount of buffer required was %" PRIu32 " bytes\n", max_buffer_use);

	/* clear the event, disable the PRU and let the library clean up */
	if (pru_cleanup() < 0) {
		ERROR("failure to cleanup PRU");
		return -1;
	}

	if (overrun) {
		return -1;
	} else {
		return 0;
	}
}

bool
parse_opt(int argc, char **argv)
{
	struct option opts[] = {
		{ "test-mode", 0, NULL, 1 },
		{ "capture-choke", 1, NULL, 2 },
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
		case 1: /* test-mode */
			flag_test_mode = true;
			break;
		case 2: /* test-mode */
			flag_capture_choke = atoi(optarg);
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			return false;
		};
	}

	if (optind < argc) {
		flag_out_file = argv[optind];
	}

	return true;
}

int main(int argc, char **argv)
{
	if (!parse_opt(argc, argv)) {
		ERROR("failed parsing arguments");
		usage(argv[0]);
		exit(1);
	}

	setup_signal_handler();

	if (run() == -1) {
		exit(1);
	}

	return 0;
}
