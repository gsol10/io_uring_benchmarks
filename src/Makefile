LIBURING_PATH=../deps

CFLAGS = -O2 -DSQE_SIZE=64 # -DSQPOLL -DFEAT_FAST_POLL are available

echo: main.c pci.c
	gcc $(CFLAGS) main.c pci.c -I$(LIBURING_PATH)/include -L$(LIBURING_PATH)/lib -luring -o echo

clean:
	rm echo
