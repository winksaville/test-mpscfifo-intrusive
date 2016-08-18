# Turn off builtin implicit rules
.SUFFIXES:

CC=clang

CC_FLAGS = -Wall -std=c11 -O2 -g -pthread
all: test simple

msg_pool.o : msg_pool.c msg_pool.h dpf.h Makefile
	${CC} ${CC_FLAGS} -c $< -o $@

diff_timespec.o : diff_timespec.c diff_timespec.h dpf.h Makefile
	${CC} ${CC_FLAGS} -c $< -o $@

mpscfifo.o : mpscfifo.c mpscfifo.h dpf.h Makefile
	${CC} ${CC_FLAGS} -c $< -o $@

test.o : test.c mpscfifo.h msg_pool.h diff_timespec.h dpf.h Makefile
	${CC} ${CC_FLAGS} -c $< -o $@

test : test.o mpscfifo.o msg_pool.o diff_timespec.o
	${CC} ${CC_FLAGS} $^ -o $@
	objdump -d $@ > $@.txt

run : test
	@./test ${client_count} ${loops} ${msg_count}

simple.o : simple.c mpscfifo.h msg_pool.h diff_timespec.h dpf.h Makefile
	${CC} ${CC_FLAGS} -c $< -o $@

simple : simple.o mpscfifo.o msg_pool.o diff_timespec.o
	${CC} ${CC_FLAGS} $^ -o $@
	objdump -d $@ > $@.txt

runs : simple
	@./simple ${client_count} ${loops} ${msg_count}

clean :
	@rm -f *.o
	@rm -f test test.txt
	@rm -f simple simple.txt
