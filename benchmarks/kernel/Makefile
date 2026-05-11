CXX      := g++
CXXFLAGS := -O2 -std=c++17 -Wall -I../../common
LDFLAGS  := -lm

TARGETS  := ping pong

.PHONY: all clean

all: $(TARGETS)

ping: ping.cpp ../../common/timing.h ../../common/stats.h
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

pong: pong.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)
