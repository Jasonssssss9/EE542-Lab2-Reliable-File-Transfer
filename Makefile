CXX := g++
CPPFLAGS := -Iinclude
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic

COMMON_SOURCES := src/protocol.cpp src/reliability.cpp src/pacer.cpp src/file_io.cpp

.PHONY: all client server test clean

all: client server

client: bin/client

server: bin/server

bin/client: src/client.cpp $(COMMON_SOURCES) | bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

bin/server: src/server.cpp $(COMMON_SOURCES) | bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

bin/protocol_test: tests/protocol_test.cpp src/protocol.cpp src/reliability.cpp | bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

test: bin/protocol_test
	./bin/protocol_test

bin:
	mkdir -p bin

clean:
	rm -rf bin build
