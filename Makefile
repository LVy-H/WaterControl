CXX ?= g++
CPPFLAGS := -Ifirmware/WaterDistribution/src -Isimulator
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -fno-exceptions -fno-rtti
SANITIZERS := -fsanitize=address,undefined -fno-omit-frame-pointer
CORE := firmware/WaterDistribution/src/controller.cpp
PLANT := simulator/plant.cpp
HEADERS := firmware/WaterDistribution/src/water/controller.hpp simulator/plant.hpp

.PHONY: all sim check
all: .build/water-sim

.build:
	mkdir -p $@

.build/water-sim: $(CORE) $(PLANT) simulator/engine.cpp $(HEADERS) | .build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE) $(PLANT) simulator/engine.cpp -o $@

.build/scenarios: $(CORE) $(PLANT) tests/scenarios.cpp $(HEADERS) | .build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SANITIZERS) $(CORE) $(PLANT) tests/scenarios.cpp -o $@

check: .build/scenarios
	./.build/scenarios

sim:
	python3 simulator/server.py
