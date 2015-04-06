CFLAGS+=-Wall -Werror -g3
LDLIBS+= -lpthread -lprussdrv -lrt

all: iorec.bin iorec-test.bin iorec

clean:
	rm -f iorec *.o *.bin

iorec.bin: iorec.p
	pasm -b $^

iorec-test.bin: iorec.p
	pasm -DTEST_PATTERN=1 -b $^ iorec-test

iorec: iorec.o
