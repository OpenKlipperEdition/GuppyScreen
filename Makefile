#
# Makefile
#
ifdef CROSS_COMPILE
CC 	= $(CROSS_COMPILE)gcc
CXX = $(CROSS_COMPILE)g++
CPP = $(CC) -E
AS 	= $(CROSS_COMPILE)as
LD	= $(CROSS_COMPILE)ld
AR	= $(CROSS_COMPILE)ar
NM	= $(CROSS_COMPILE)nm
STRIP 	= $(CROSS_COMPILE)strip
endif

LVGL_DIR_NAME 	?= lvgl
LVGL_DIR 		?= .

WARNINGS		:= -Wall -Wextra -Wno-unused-function -Wno-error=strict-prototypes -Wpointer-arith \
					-fno-strict-aliasing -Wno-error=cpp -Wuninitialized -Wmaybe-uninitialized -Wno-unused-parameter -Wno-missing-field-initializers -Wtype-limits -Wsizeof-pointer-memaccess \
					-Wno-format-nonliteral -Wno-cast-qual -Wunreachable-code -Wno-switch-default -Wreturn-type -Wmultichar -Wformat-security -Wno-error=pedantic \
					-Wno-sign-compare -Wdouble-promotion -Wclobbered -Wempty-body -Wtype-limits -Wshift-negative-value \
					-Wno-unused-value -Wno-unused-parameter -Wno-missing-field-initializers -Wuninitialized -Wmaybe-uninitialized -Wall -Wextra -Wno-unused-parameter \
					-Wno-missing-field-initializers -Wtype-limits -Wsizeof-pointer-memaccess -Wno-format-nonliteral -Wpointer-arith -Wno-cast-qual \
					-Wunreachable-code -Wno-switch-default -Wreturn-type -Wmultichar -Wformat-security -Wno-sign-compare
CFLAGS 			?= -O3 -g0 -MD -MP -I$(LVGL_DIR)/ $(WARNINGS) 
LDFLAGS 		?= -static -lm -Llibhv/lib -Lspdlog/build -l:libhv.a -latomic -lpthread -Lwpa_supplicant/wpa_supplicant/ -l:libwpa_client.a -lstdc++fs -l:libspdlog.a
BIN 			= guppyscreen
BUILD_DIR 		= ./build
BUILD_OBJ_DIR 	= $(BUILD_DIR)/obj
BUILD_BIN_DIR 	= $(BUILD_DIR)/bin
SPDLOG_DIR		= spdlog

prefix 			?= /usr
bindir 			?= $(prefix)/bin

#Collect the files to compile
MAINSRC = 		$(filter-out $(LVGL_DIR)/src/kd_graphic_mode.cpp, $(wildcard $(LVGL_DIR)/src/*.cpp))

include $(LVGL_DIR)/lvgl/lvgl.mk
include $(LVGL_DIR)/lv_drivers/lv_drivers.mk

CSRCS 			+= $(wildcard $(LVGL_DIR)/assets/*.c)
CSRCS			+= $(wildcard $(LVGL_DIR)/lv_touch_calibration/*.c)

ASSET_DIR		= material
ifdef GUPPY_SMALL_SCREEN
ASSET_DIR		= material_46
DEFINES			+= -D GUPPY_SMALL_SCREEN
endif


ifdef GUPPY_ROTATE
DEFINES			+= -D GUPPY_ROTATE
endif


ifeq ($(GUPPY_THEME),zbolt)
CSRCS 			+= $(wildcard $(LVGL_DIR)/assets/zbolt/*.c)
DEFINES			+= -D ZBOLT
else
CSRCS 			+= $(wildcard $(LVGL_DIR)/assets/$(ASSET_DIR)/*.c)
endif

ifdef GUPPYSCREEN_VERSION
DEFINES			+= -D GUPPYSCREEN_VERSION="\"${GUPPYSCREEN_VERSION}\""
endif

OBJEXT 			?= .o

AOBJS 			= $(ASRCS:.S=$(OBJEXT))
COBJS 			= $(CSRCS:.c=$(OBJEXT))

MAINOBJ 		= $(MAINSRC:.cpp=$(OBJEXT))
DEPS                    = $(addprefix $(BUILD_OBJ_DIR)/, $(patsubst %.o, %.d, $(MAINOBJ)))

OBJS 			= $(AOBJS) $(COBJS) $(MAINOBJ)
TARGET 			= $(addprefix $(BUILD_OBJ_DIR)/, $(patsubst ./%, %, $(OBJS)))

INC 				:= -I./ -I./lvgl/ -I./lv_touch_calibration -I./spdlog/include -Ilibhv/include -Iwpa_supplicant/src/common
LDLIBS	 			:= -lm

DEFINES				+= -D _GNU_SOURCE -DSPDLOG_COMPILED_LIB

ifdef EVDEV_CALIBRATE
DEFINES +=  -D EVDEV_CALIBRATE
endif

# SIMULATION is enabled by default, need CROSS_COMPILE variable to do MIPS build
ifndef CROSS_COMPILE
DEFINES +=  -D LV_BUILD_TEST=0 -D SIMULATOR
LDLIBS += -lSDL2
endif

COMPILE_CC				= $(CC) $(CFLAGS) $(INC) $(DEFINES)
COMPILE_CXX				= $(CC) $(CFLAGS) $(INC) $(DEFINES)

## MAINOBJ -> OBJFILES

all: default

libhv.a:
	$(MAKE) -C libhv -j$(nproc) libhv

libspdlog.a:
	@mkdir -p $(SPDLOG_DIR)/build
	@cmake -B $(SPDLOG_DIR)/build -S $(SPDLOG_DIR)/ -DCMAKE_CXX_COMPILER=$(CXX)
	$(MAKE) -C $(SPDLOG_DIR)/build -j$(nproc)

wpaclient:
	$(MAKE) -C wpa_supplicant/wpa_supplicant -j$(nproc) libwpa_client.a

## Offline, host-native pure-logic test binaries (2026-08-06) - no LVGL, no SDL2, no
## cross-compile: these always build with the plain host g++ regardless of CROSS_COMPILE,
## since they're meant to run right here, not on the printer. See tests/minitest.h and
## docs/z_compensate_status_api.md.
.PHONY: test test-z-compensate-status test-z-offset-persistence test-contract-fixture test-integration-harness test-subscription-baseline-ordering test-config-theme-parse-safety test-wifi-credentials

test: test-z-compensate-status test-z-offset-persistence test-contract-fixture test-integration-harness test-subscription-baseline-ordering test-config-theme-parse-safety test-wifi-credentials

test-wifi-credentials:
	@mkdir -p build
	g++ -std=c++17 -Wall -Wextra -I. \
		src/wifi_credentials.cpp tests/test_wifi_credentials.cpp \
		-o build/test_wifi_credentials
	./build/test_wifi_credentials

# 2026-08-06 crash-fix regression test: Config::init()/ThemeConfig::init()
# must fall back to defaults, never crash, when a stat()-present file fails
# to actually parse (see tests/test_config_theme_parse_safety.cpp header).
test-config-theme-parse-safety: libhv.a libspdlog.a
	g++ -std=c++17 -Wall -Wextra -I./libhv/include -I./spdlog/include -I. -DSPDLOG_COMPILED_LIB \
		src/config.cpp src/theme.cpp tests/test_config_theme_parse_safety.cpp \
		-Lspdlog/build -l:libspdlog.a -lstdc++fs \
		-o build/test_config_theme_parse_safety
	./build/test_config_theme_parse_safety

test-z-compensate-status: libhv.a
	g++ -std=c++17 -Wall -Wextra -I./libhv/include -I. \
		src/z_compensate_status.cpp tests/test_z_compensate_status.cpp \
		-o build/test_z_compensate_status
	./build/test_z_compensate_status

test-z-offset-persistence:
	g++ -std=c++17 -Wall -Wextra -I. \
		src/z_offset_config_persistence.cpp tests/test_z_offset_config_persistence.cpp \
		-o build/test_z_offset_config_persistence
	./build/test_z_offset_config_persistence

# Part 7 cross-project contract test: parses tests/fixtures/z_compensate_status_contract.json
# (a literal copy of the Python backend's own generated fixture, see tests/fixtures/README.md)
# through the real C++ parser/tracker. Must be run from the repo root so the relative
# fixture path resolves - `make test` already does this.
test-contract-fixture: libhv.a
	g++ -std=c++17 -Wall -Wextra -I./libhv/include -I. \
		src/z_compensate_status.cpp tests/test_contract_fixture.cpp \
		-o build/test_contract_fixture
	./build/test_contract_fixture

# Part 3 deployment-verification regression: proves the subscription-baseline ordering
# guarantee against the REAL State class (src/state.cpp) - no LVGL runtime needed, see
# tests/test_subscription_baseline_ordering.cpp's own header comment.
test-subscription-baseline-ordering: libhv.a libspdlog.a
	g++ -std=c++17 -Wall -Wextra -I./libhv/include -I./spdlog/include -I. -I./lvgl \
		-DSPDLOG_COMPILED_LIB \
		src/state.cpp src/notify_consumer.cpp \
		tests/link_stubs_state_deps.cpp \
		tests/test_subscription_baseline_ordering.cpp \
		-Lspdlog/build -l:libspdlog.a \
		-o build/test_subscription_baseline_ordering
	./build/test_subscription_baseline_ordering

# Part 8 offline integration harness: replays the ordered success/failure traces (see
# tests/fixtures/README.md) through the real parser/tracker AND real
# ZOffsetConfigPersistence together, proving the full frontend chain end-to-end.
test-integration-harness: libhv.a
	g++ -std=c++17 -Wall -Wextra -I./libhv/include -I. \
		src/z_compensate_status.cpp src/z_offset_config_persistence.cpp \
		tests/test_integration_harness.cpp \
		-o build/test_integration_harness
	./build/test_integration_harness

$(BUILD_OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@$(COMPILE_CXX) -std=c++17 $(CFLAGS) -c $< -o $@
	@echo "CXX $<"

$(BUILD_OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@$(COMPILE_CC)  $(CFLAGS) -c $< -o $@
	@echo "CC $<"

$(BUILD_OBJ_DIR)/kd_graphic_mode.o: src/kd_graphic_mode.cpp
	@mkdir -p $(dir $@)
	@$(COMPILE_CC)  $(CFLAGS) -c $< -o $@
	@echo "CC $<"

kd_graphic_mode: $(BUILD_OBJ_DIR)/kd_graphic_mode.o
	$(CC) -o $(BUILD_BIN_DIR)/kd_graphic_mode $(BUILD_OBJ_DIR)/kd_graphic_mode.o

default: $(TARGET)
	@mkdir -p $(dir $(BUILD_BIN_DIR)/)
	$(CXX) -o $(BUILD_BIN_DIR)/$(BIN) $(TARGET) $(LDFLAGS) $(LDLIBS)
	@echo "CXX $<"

spdlogclean:
	rm -rf $(SPDLOG_DIR)/build

libhvclean:
	$(MAKE) -C libhv clean

wpaclean:
	$(MAKE) -C wpa_supplicant/wpa_supplicant clean

clean:
	rm -rf $(BUILD_DIR)

install:
	install -d $(DESTDIR)$(bindir)
	install $(BUILD_BIN_DIR)/$(BIN) $(DESTDIR)$(bindir)

uninstall:
	$(RM) -r $(addprefix $(DESTDIR)$(bindir)/,$(BIN))

build:
	$(MAKE) wpaclean
	$(MAKE) wpaclient
	$(MAKE) libhvclean
	$(MAKE) libhv.a
	$(MAKE) spdlogclean
	$(MAKE) libspdlog.a
	$(MAKE) clean
	$(MAKE) -j$(nproc)

-include			$(DEPS)
