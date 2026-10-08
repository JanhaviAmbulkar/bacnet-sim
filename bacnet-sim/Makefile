# Standalone Makefile (Linux/macOS). For Windows or IDE use, see CMakeLists.txt.
CC      ?= gcc
CXX     ?= g++
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CXXFLAGS?= -std=c++17 -Wall -Wextra -O2
LDLIBS  := -pthread
INC     := -Iinclude -Ic
BUILD   := build-make
DEBUG   ?= 0
ifeq ($(DEBUG),1)
  CFLAGS   += -g -O0
  CXXFLAGS += -g -O0
endif

C_OBJS   := $(BUILD)/bytebuf.o
CORE_OBJS:= $(BUILD)/bacnet.o $(BUILD)/net.o $(BUILD)/device.o $(BUILD)/ipc.o
BINS     := $(BUILD)/bacnet_sim $(BUILD)/bacnet_client $(BUILD)/bacnet_logger $(BUILD)/test_codec

.PHONY: all test debug clean
all: $(BINS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: c/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -MMD -MP -c $< -o $@
$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) -MMD -MP -c $< -o $@
$(BUILD)/test_codec.o: tests/test_codec.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) -MMD -MP -c $< -o $@

$(BUILD)/bacnet_sim: $(BUILD)/simulator.o $(CORE_OBJS) $(C_OBJS)
	$(CXX) $^ $(LDLIBS) -o $@
$(BUILD)/bacnet_client: $(BUILD)/client.o $(CORE_OBJS) $(C_OBJS)
	$(CXX) $^ $(LDLIBS) -o $@
$(BUILD)/bacnet_logger: $(BUILD)/logger.o $(CORE_OBJS) $(C_OBJS)
	$(CXX) $^ $(LDLIBS) -o $@
$(BUILD)/test_codec: $(BUILD)/test_codec.o $(CORE_OBJS) $(C_OBJS)
	$(CXX) $^ $(LDLIBS) -o $@

test: all
	$(BUILD)/test_codec
	BUILD_DIR=$(BUILD) python3 -m pytest -q tests

debug:
	$(MAKE) DEBUG=1 BUILD=build-debug all

clean:
	rm -rf build-make build-debug build

-include $(wildcard $(BUILD)/*.d)
