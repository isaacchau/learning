CXX = g++
CXXFLAGS = -std=c++14 -Wall -Wextra -O2 -I.

TARGET = data_dumper_demo
SRC = data_dumper_demo.cpp

UPSTREAM_TARGET = data_dumper_upstream_demo
UPSTREAM_SRC = data_dumper_upstream_demo.cpp
GENERATED = generated/market_data_dumps.h

MFEED_GENERATED = generated/md_mfeed_dumps.h
MFEED_TARGET = data_dumper_mfeed_demo
MFEED_TEST_TARGET = tests/test_mfeed_dumper

all: $(TARGET)

$(TARGET): $(SRC) data_dumper.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

generate:
	python3 generate_dd_dump.py upstream/market_data.h -o $(GENERATED)

generate_mfeed:
	python3 generate_dd_dump.py upstream/mfeed_types.h upstream/mfeed_core.h upstream/mfeed_market_data.h upstream/mfeed_protocol.h -o $(MFEED_GENERATED)

$(UPSTREAM_TARGET): $(UPSTREAM_SRC) data_dumper.h $(GENERATED)
	$(CXX) $(CXXFLAGS) -o $@ $(UPSTREAM_SRC)

$(MFEED_TARGET): data_dumper_mfeed_demo.cpp data_dumper.h $(MFEED_GENERATED)
	$(CXX) $(CXXFLAGS) -o $@ data_dumper_mfeed_demo.cpp

$(MFEED_TEST_TARGET): tests/test_mfeed_dumper.cpp data_dumper.h $(MFEED_GENERATED)
	$(CXX) $(CXXFLAGS) -o $@ tests/test_mfeed_dumper.cpp

upstream_demo: $(UPSTREAM_TARGET)

mfeed_demo: $(MFEED_TARGET)
	./$(MFEED_TARGET)

test_mfeed: $(MFEED_TEST_TARGET)
	./$(MFEED_TEST_TARGET)

clean:
	rm -f $(TARGET) $(UPSTREAM_TARGET) $(MFEED_TARGET) $(MFEED_TEST_TARGET)

.PHONY: all clean generate generate_mfeed upstream_demo mfeed_demo test_mfeed
