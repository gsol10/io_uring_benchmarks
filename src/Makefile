LIBURING_PATH=../deps

run:
	./echo $(TN_ARGS)

build: echo

print-nf-name:
	echo "Simple NF/IO_uring test"

echo: main.c pci.c
	gcc main.c pci.c -O2 -I$(LIBURING_PATH)/include -L$(LIBURING_PATH)/lib -luring -o echo

is-linux: ;

debug:main.c
	gcc main.c pci.c -g -DDEBUG -I$(LIBURING_PATH)/include -L$(LIBURING_PATH)/lib -luring -o echo

clean:
	rm echo
