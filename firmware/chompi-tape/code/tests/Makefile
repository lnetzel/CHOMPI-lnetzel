# Minimal host build for the TAPE trim-control validation tests.
# Needs any host C++14 compiler (g++ or clang++). From an MSYS2 bash shell:
#   make -C firmware/chompi-tape/code/tests test
# Note: MSYS2's base install has no compiler; install one with e.g.
#   pacman -S mingw-w64-ucrt64-gcc   # then run make with CXX from that env

CXX      ?= g++
CXXFLAGS ?= -std=gnu++14 -Wall -Wextra -Werror -I../src

BUILD_DIR = build
BIN       = $(BUILD_DIR)/trim_controls_test

$(BIN): trim_controls_test.cpp ../src/TrimValidation.h
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ trim_controls_test.cpp

.PHONY: test
test: $(BIN)
	./$(BIN)

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
