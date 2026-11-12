CXX ?= clang++
CXXFLAGS ?= -std=c++17 -O0 -g -Wall -Wextra -Wpedantic -Wconversion
CORE_INCLUDE := -Icomponents/trainer_core/include
CORE_SOURCES := $(wildcard components/trainer_core/src/*.cpp)
HOST_TEST := /tmp/trainer-loop-core-tests

.PHONY: test host-test simulator simulator-test clean

simulator:
	host/simulator/run.sh --window --provisioned

simulator-test:
	host/simulator/test.sh

host-test:
	$(CXX) $(CXXFLAGS) $(CORE_INCLUDE) host/tests/test_core.cpp $(CORE_SOURCES) -o $(HOST_TEST)
	$(HOST_TEST)

FIT_PYTHON ?= .venv-fit/bin/python

test: host-test
	cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Debug -DFIT_PYTHON="$(abspath $(FIT_PYTHON))"
	cmake --build build-host
	ctest --test-dir build-host --output-on-failure

clean:
	@true
