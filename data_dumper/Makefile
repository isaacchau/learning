CXX = g++
CXXFLAGS = -std=c++14 -Wall -Wextra -O2 -I.

TARGET = data_dumper_demo
SRC = data_dumper_demo.cpp

UPSTREAM_TARGET = data_dumper_upstream_demo
UPSTREAM_SRC = data_dumper_upstream_demo.cpp
GENERATED = generated/market_data_dumps.h

OMNET_GENERATED = generated/nasdaq_omnet_dumps.h
OMNET_TARGET = data_dumper_omnet_demo
OMNET_TEST_TARGET = tests/test_omnet_dumper

all: $(TARGET)

$(TARGET): $(SRC) data_dumper.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

generate:
	python3 generate_dd_dump.py upstream/market_data.h -o $(GENERATED)

generate_omnet:
	python3 generate_dd_dump.py upstream/omnet_types.h upstream/omnet_core.h upstream/omnet_market_data.h upstream/omnet_protocol.h -o $(OMNET_GENERATED)

$(UPSTREAM_TARGET): $(UPSTREAM_SRC) data_dumper.h $(GENERATED)
	$(CXX) $(CXXFLAGS) -o $@ $(UPSTREAM_SRC)

$(OMNET_TARGET): data_dumper_omnet_demo.cpp data_dumper.h $(OMNET_GENERATED)
	$(CXX) $(CXXFLAGS) -o $@ data_dumper_omnet_demo.cpp

$(OMNET_TEST_TARGET): tests/test_omnet_dumper.cpp data_dumper.h $(OMNET_GENERATED)
	$(CXX) $(CXXFLAGS) -o $@ tests/test_omnet_dumper.cpp

upstream_demo: $(UPSTREAM_TARGET)

omnet_demo: $(OMNET_TARGET)
	./$(OMNET_TARGET)

test_omnet: $(OMNET_TEST_TARGET)
	./$(OMNET_TEST_TARGET)

clean:
	rm -f $(TARGET) $(UPSTREAM_TARGET) $(OMNET_TARGET) $(OMNET_TEST_TARGET)

.PHONY: all clean generate generate_omnet upstream_demo omnet_demo test_omnet

