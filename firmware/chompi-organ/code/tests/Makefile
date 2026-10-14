# Native DSP tests for chompi-organ (host compiler only, no MCU/Daisy deps).
# Usage: make -C firmware/chompi-organ/code/tests test

CXX      ?= g++
CXXFLAGS ?= -std=c++14 -O2 -Wall -Wextra -I../src

test: tonewheel_character_test plate_reverb_test distortion_test vibrato_test envelope_map_test
	./tonewheel_character_test
	./plate_reverb_test
	./distortion_test
	./vibrato_test
	./envelope_map_test

tonewheel_character_test: tonewheel_character_test.cpp ../src/TonewheelCharacter.h
	$(CXX) $(CXXFLAGS) -o $@ $<

plate_reverb_test: plate_reverb_test.cpp ../src/PlateReverb.h
	$(CXX) $(CXXFLAGS) -o $@ $<

distortion_test: distortion_test.cpp ../src/Distortion.h
	$(CXX) $(CXXFLAGS) -o $@ $<

vibrato_test: vibrato_test.cpp ../src/Vibrato.h
	$(CXX) $(CXXFLAGS) -o $@ $<

envelope_map_test: envelope_map_test.cpp ../src/EnvelopeMap.h
	$(CXX) $(CXXFLAGS) -o $@ $<

# The vendored fx_engine.h triggers -Wunused-parameter from its DelayLine
# template parameters; keep the test output readable without editing it.
plate_reverb_test: CXXFLAGS += -Wno-unused-parameter

clean:
	rm -f tonewheel_character_test tonewheel_character_test.exe \
		plate_reverb_test plate_reverb_test.exe \
		distortion_test distortion_test.exe \
		vibrato_test vibrato_test.exe \
		envelope_map_test envelope_map_test.exe

.PHONY: test clean
