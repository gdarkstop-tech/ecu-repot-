# Host simulator: compiles the SAME robot_logic + robot_ui sources as the firmware against the
# real U8g2 C library, so rendered frames are exactly what the SH1106 will show.
#
#   make            build ./build/robot_sim
#   make gallery    render every expression/screen to ../../docs/media (PNG + GIF)
#
# Requires the U8g2 and ArduinoJson sources that PlatformIO downloads:
#   cd firmware && pio pkg install -e robot
LIBDEPS ?= ../.pio/libdeps/robot
U8G2    := $(LIBDEPS)/U8g2/src/clib
AJSON   := $(LIBDEPS)/ArduinoJson/src
BUILD   := build

CXX ?= g++
CC  ?= gcc
CFLAGS   := -O2 -ffunction-sections -fdata-sections -I$(U8G2)
CXXFLAGS := -O2 -std=gnu++17 -Wall -Wextra -ffunction-sections -fdata-sections \
            -I$(U8G2) -I$(AJSON) -I../lib/robot_logic/src -I../lib/robot_ui/src -DROBOT_NATIVE=1

U8G2_SRC := $(wildcard $(U8G2)/*.c)
U8G2_OBJ := $(patsubst $(U8G2)/%.c,$(BUILD)/u8g2/%.o,$(U8G2_SRC))
APP_SRC  := $(wildcard ../lib/robot_logic/src/*.cpp) $(wildcard ../lib/robot_ui/src/*.cpp) sim_main.cpp
APP_OBJ  := $(patsubst %.cpp,$(BUILD)/app/%.o,$(notdir $(APP_SRC)))

vpath %.cpp ../lib/robot_logic/src ../lib/robot_ui/src .

all: $(BUILD)/robot_sim

$(BUILD)/robot_sim: $(U8G2_OBJ) $(APP_OBJ)
	$(CXX) -o $@ $^ -Wl,--gc-sections

$(BUILD)/u8g2/%.o: $(U8G2)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/app/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

gallery: $(BUILD)/robot_sim
	./$(BUILD)/robot_sim --gallery $(BUILD)/frames
	python3 render_media.py $(BUILD)/frames ../../docs/media

clean:
	rm -rf $(BUILD)

.PHONY: all gallery clean
