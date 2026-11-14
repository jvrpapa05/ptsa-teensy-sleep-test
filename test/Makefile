# Builds and runs the PC simulation of the Teensy sketch's test logic.
#   make            build and run all scenarios
#   make clean
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined

SRC = test_main.cpp sim.cpp ../PtsaSleepTest/src/sleep_test_core.cpp

test: sim_tests
	./sim_tests

sim_tests: $(SRC) sim.h ../PtsaSleepTest/src/sleep_test_core.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

clean:
	rm -f sim_tests

.PHONY: test clean
