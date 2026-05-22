# ============================================================================
# Makefile — Build system for msg_client and msg_test_server
# ============================================================================
# GNU Make build (no CMake). Supports release, debug, test, and demo targets.
#
# Directory Structure:
#   src/               — C++ source files and local headers
#   build/             — Intermediate compiler object files
#   bin/               — Final compiled executables
#
# Key targets:
#   make        — Release build (O3 + LTO)
#   make debug  — Debug build (O0 + g + DEBUG)
#   make test   — Compile and run unit tests
#   make test_ini — Compile and run INI parser isolation tests
#   make demo   — Build metrics_demo standalone
#   make check  — Run cppcheck static analysis
#   make format — Run clang-format on all source files
# ============================================================================

CXX      = g++

# Directories
SRC_DIR   = src
BUILD_DIR = build
BIN_DIR   = bin

# Include paths
INC_FLAGS = -I$(SRC_DIR)/client -I$(SRC_DIR)/server -I$(SRC_DIR)/common

# Base and Release flags
CXXFLAGS = -std=c++14 -O3 -march=native -flto -Wall -Wextra -pthread -faligned-new $(INC_FLAGS)

# Debug flags (No optimizations, includes symbols)
CXXFLAGS_DEBUG = -std=c++14 -O0 -g -DDEBUG -Wall -Wextra -pthread -faligned-new $(INC_FLAGS)

# Linker flags
LDFLAGS  = -pthread -flto
LDFLAGS_DEBUG = -pthread

# Binaries
TARGET_CLIENT = $(BIN_DIR)/msg_client
TARGET_SERVER = $(BIN_DIR)/msg_test_server
TARGET_DEBUG  = $(BIN_DIR)/msg_client_debug
TARGET_DEMO   = $(BIN_DIR)/metrics_demo
TEST_BIN      = $(BIN_DIR)/test_runner
TEST_INI_BIN  = $(BIN_DIR)/test_ini

# Source files
CLIENT_SRCS = $(SRC_DIR)/client/main.cpp \
              $(SRC_DIR)/client/msg_client.cpp \
              $(SRC_DIR)/common/log_msg.cpp \
              $(SRC_DIR)/client/config_parser.cpp \
              $(SRC_DIR)/client/ini_parser.cpp

SERVER_SRCS = $(SRC_DIR)/server/msg_test_server.cpp \
              $(SRC_DIR)/common/log_msg.cpp

DEMO_SRC    = $(SRC_DIR)/common/metrics_demo.cpp
TEST_SRC    = tests/test_main.cpp
TEST_INI_SRC = tests/test_ini_parser.cpp

# Object files (Separate them to speed up recompilation!)
CLIENT_OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(CLIENT_SRCS))
SERVER_OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(SERVER_SRCS))
DEBUG_OBJS  = $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.dbg.o, $(CLIENT_SRCS))

# Headers for dependency tracking
HEADERS  = $(SRC_DIR)/common/lockfree_ringbuffer.h \
           $(SRC_DIR)/common/shared_ptr_pool.h \
           $(SRC_DIR)/common/protocol.h \
           $(SRC_DIR)/client/msg_client.h \
           $(SRC_DIR)/common/log_msg.h \
           $(SRC_DIR)/client/config_parser.h \
           $(SRC_DIR)/client/ini_parser.h \
           $(SRC_DIR)/common/metrics.hpp \
           $(SRC_DIR)/common/market_data/message_types.h

.PHONY: all clean debug run run-server check format test demo test_ini

all: $(TARGET_CLIENT) $(TARGET_SERVER)

# Release builds
$(TARGET_CLIENT): $(CLIENT_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_SERVER): $(SERVER_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Debug build
debug: $(TARGET_DEBUG)

$(TARGET_DEBUG): $(DEBUG_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_DEBUG) -o $@ $^ $(LDFLAGS_DEBUG)

# Demo build
demo: $(TARGET_DEMO)

$(TARGET_DEMO): $(DEMO_SRC) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEMO_SRC) -o $(TARGET_DEMO) $(LDFLAGS)

# Object file rules (Release)
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Object file rules (Debug)
$(BUILD_DIR)/%.dbg.o: $(SRC_DIR)/%.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_DEBUG) -c $< -o $@

# Helper: Run with defaults
run: $(TARGET_CLIENT)
	./$(TARGET_CLIENT)

# Helper: Run test server
run-server: $(TARGET_SERVER)
	./$(TARGET_SERVER)

# Static analysis
check:
	cppcheck --enable=all --std=c++14 --suppress=missingIncludeSystem $(INC_FLAGS) $(SRC_DIR)/

# Format code
format:
	clang-format -i $(SRC_DIR)/client/*.cpp $(SRC_DIR)/client/*.h $(SRC_DIR)/server/*.cpp $(SRC_DIR)/common/*.cpp $(SRC_DIR)/common/*.h $(SRC_DIR)/common/*.hpp tests/*.cpp 2>/dev/null || echo "clang-format not installed"

# ============================================================================
# Unit Tests
# ============================================================================

# Build test runner
test: $(TEST_BIN)
	@echo "Running unit tests..."
	@./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_DEBUG) -Itests $(TEST_SRC) \
		$(SRC_DIR)/client/config_parser.cpp \
		$(SRC_DIR)/client/ini_parser.cpp \
		$(SRC_DIR)/common/log_msg.cpp \
		$(SRC_DIR)/client/msg_client.cpp \
		-o $(TEST_BIN) $(LDFLAGS_DEBUG)

# Build INI parser tests
test_ini: $(TEST_INI_BIN)
	@echo "Running INI parser isolation tests..."
	@./$(TEST_INI_BIN)

$(TEST_INI_BIN): $(TEST_INI_SRC) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_DEBUG) -Itests $< $(SRC_DIR)/client/ini_parser.cpp -o $@ $(LDFLAGS_DEBUG)

# Clean build and binary output directories
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
