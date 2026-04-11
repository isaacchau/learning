CXX      = g++

# Base and Release flags
CXXFLAGS = -std=c++14 -O3 -march=native -flto -Wall -Wextra -pthread -faligned-new
CXXFLAGS += -I.

# Debug flags (No optimizations, includes symbols)
CXXFLAGS_DEBUG = -std=c++14 -O0 -g -DDEBUG -Wall -Wextra -pthread -faligned-new -I.

# Linker flags
LDFLAGS  = -pthread -flto
LDFLAGS_DEBUG = -pthread

# Binaries
TARGET_CLIENT = msg_client
TARGET_SERVER = msg_test_server
TARGET_DEBUG  = msg_client_debug

# Source files
CLIENT_SRCS = main.cpp msg_client.cpp log_msg.cpp config_parser.cpp \
              aggregation/aggregation_manager.cpp \
              aggregation/output_queue.cpp \
              aggregation/output_formatter.cpp \
              aggregation/output_writer.cpp
SERVER_SRCS = msg_test_server.cpp log_msg.cpp

# Object files (Separate them to speed up recompilation!)
CLIENT_OBJS = $(CLIENT_SRCS:.cpp=.o)
SERVER_OBJS = $(SERVER_SRCS:.cpp=.o)
DEBUG_OBJS  = $(CLIENT_SRCS:.cpp=.dbg.o)

# Headers for dependency tracking
HEADERS  = lockfree_ringbuffer.h shared_ptr_pool.h protocol.h msg_client.h log_msg.h config_parser.h json.hpp \
           aggregation/aggregation_config.h aggregation/aggregation_state.h \
           aggregation/aggregation_manager.h aggregation/output_record.h \
           aggregation/output_queue.h aggregation/output_formatter.h \
           aggregation/output_writer.h market_data/message_types.h

# Test files
TEST_SRC   = tests/test_main.cpp
TEST_BIN   = test_runner

.PHONY: all clean debug run run-server check format test

all: $(TARGET_CLIENT) $(TARGET_SERVER)

# Release builds
$(TARGET_CLIENT): $(CLIENT_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_SERVER): $(SERVER_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Debug build
debug: $(TARGET_DEBUG)

$(TARGET_DEBUG): $(DEBUG_OBJS)
	$(CXX) $(CXXFLAGS_DEBUG) -o $@ $^ $(LDFLAGS_DEBUG)

# Object file rules (Release)
%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Object file rules (Debug)
%.dbg.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS_DEBUG) -c $< -o $@

# Helper: Run with defaults (can override via ENV vars now!)
run: $(TARGET_CLIENT)
	./$(TARGET_CLIENT)

# Helper: Run test server
run-server: $(TARGET_SERVER)
	./$(TARGET_SERVER)

# Static analysis
check:
	cppcheck --enable=all --std=c++14 --suppress=missingIncludeSystem .

# Format code
format:
	clang-format -i *.cpp *.h tests/*.cpp 2>/dev/null || echo "clang-format not installed"

# ============================================================================
# Unit Tests
# ============================================================================

# Build test runner
test: $(TEST_BIN)
	@echo "Running unit tests..."
	@./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS_DEBUG) -I. $(TEST_SRC) -o $(TEST_BIN) $(LDFLAGS_DEBUG)

# Clean test binaries too
clean:
	rm -f $(CLIENT_OBJS) $(SERVER_OBJS) $(DEBUG_OBJS) \
	      $(TARGET_CLIENT) $(TARGET_SERVER) $(TARGET_DEBUG) \
	      $(TEST_BIN)
