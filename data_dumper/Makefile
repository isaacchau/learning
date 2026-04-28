CXX = g++
CXXFLAGS = -std=c++14 -Wall -Wextra -O2 -I.

TARGET = data_dumper_demo
SRC = data_dumper_demo.cpp

UPSTREAM_TARGET = data_dumper_upstream_demo
UPSTREAM_SRC = data_dumper_upstream_demo.cpp
GENERATED = generated/market_data_dumps.h

all: $(TARGET)

$(TARGET): $(SRC) data_dumper.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

generate:
	python3 generate_dd_dump.py upstream/market_data.h -o $(GENERATED)

$(UPSTREAM_TARGET): $(UPSTREAM_SRC) data_dumper.h $(GENERATED)
	$(CXX) $(CXXFLAGS) -o $@ $(UPSTREAM_SRC)

upstream_demo: $(UPSTREAM_TARGET)

clean:
	rm -f $(TARGET) $(UPSTREAM_TARGET)

.PHONY: all clean generate upstream_demo
