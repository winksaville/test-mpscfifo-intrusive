# Turn off builtin implicit rules
.SUFFIXES:

CC=clang

CC_FLAGS = -Wall -std=c11 -O2 -g -pthread
all: test

mpscfifo.o : mpscfifo.c
	${CC} ${CC_FLAGS} -c $< -o $@

test.o : test.c
	${CC} ${CC_FLAGS} -c $< -o $@

test : test.o mpscfifo.o
	${CC} ${CC_FLAGS} $^ -o $@
	objdump -d $@ > $@.txt

run : test
	@./test ${client_count} ${loops} ${msg_count}

clean :
	@rm -f test *.o test.txt
