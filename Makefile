CFLAGS+=-Wall -Werror -g3
LDLIBS+= -lpthread -lprussdrv -lrt

all: iorec.bin iorec

clean:
	rm -f iorec *.o *.bin

iorec.bin: iorec.p
	pasm -b $^

iorec: iorec.o
