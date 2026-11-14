# Builds and runs the PC simulation of the Teensy sketch's test logic. C only.
#   make            build and run all scenarios
#   make clean
CC ?= cc
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined

SRC = test_main.c sim.c ../PtsaSleepTest/src/sleep_test_core.c

test: sim_tests
	./sim_tests

sim_tests: $(SRC) sim.h ../PtsaSleepTest/src/sleep_test_core.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	rm -f sim_tests

.PHONY: test clean
