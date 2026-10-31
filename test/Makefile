CXX ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
CORE := ../main/core
SOURCES := test_core.cpp $(wildcard $(CORE)/*.cpp)

test_core: $(SOURCES) $(wildcard $(CORE)/*.h)
	$(CXX) $(CXXFLAGS) -I$(CORE) -o $@ $(SOURCES)

run: test_core
	./test_core

.PHONY: run
.DEFAULT_GOAL := run
