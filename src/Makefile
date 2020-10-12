LIBURING_PATH=../deps

echo: main.c
	gcc main.c -O2 -I$(LIBURING_PATH)/include -L$(LIBURING_PATH)/lib -luring -o echo

debug:main.c
	gcc main.c -g -DDEBUG -I$(LIBURING_PATH)/include -L$(LIBURING_PATH)/lib -luring -o echo

clean:
	rm echo