# Native DSP tests for chompi-organ (host compiler only, no MCU/Daisy deps).
# Usage: make -C firmware/chompi-organ/code/tests test

CXX      ?= g++
CXXFLAGS ?= -std=c++14 -O2 -Wall -Wextra -I../src

test: tonewheel_character_test
	./tonewheel_character_test

tonewheel_character_test: tonewheel_character_test.cpp ../src/TonewheelCharacter.h
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f tonewheel_character_test tonewheel_character_test.exe

.PHONY: test clean
