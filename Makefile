# Fern-RTLSDR, an RTL-SDR input module for FernSDR.
# SPDX-License-Identifier: GPL-2.0-or-later
#
#   make                        build/fern-rtlsdr, linked against the system libusb
#   make static [ARCH=...]      build/static-ARCH/fern-rtlsdr, fully static
#   make package [ARCH=...]     dist/rtlsdr-VERSION-linux-ARCH.fernmod
#   make test                   unit, protocol, command line and package tests
#   make test-asan              the unit, protocol and driver tests under ASan and UBSan
#   make test-tsan              the driver tests under TSan
#
# ARCH is x86_64, aarch64 or armhf (ARMv7 with hardware floating point) and
# defaults to the machine's own.

VERSION := 0.1.0

HOST_ARCH := $(shell uname -m | sed -e 's/^armv7.*/armhf/' -e 's/^arm64$$/aarch64/')
ARCH ?= $(HOST_ARCH)
ifeq ($(filter $(ARCH),x86_64 aarch64 armhf),)
$(error ARCH must be x86_64, aarch64 or armhf, not "$(ARCH)")
endif

CROSS_x86_64 := x86_64-linux-gnu-
CROSS_aarch64 := aarch64-linux-gnu-
CROSS_armhf := arm-linux-gnueabihf-
ifeq ($(ARCH),$(HOST_ARCH))
CROSS ?=
else
CROSS ?= $(CROSS_$(ARCH))
endif
ARCH_FLAGS_armhf := -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard
ARCH_FLAGS := $(ARCH_FLAGS_$(ARCH))

OPT ?= -O2
CXXFLAGS_BASE := -std=c++17 $(OPT) -g -pthread -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
	-DFERN_RTLSDR_VERSION='"$(VERSION)"'
# Upstream librtlsdr builds with these warning options.
RTLSDR_CFLAGS := -std=gnu11 $(OPT) -g -pthread -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter \
	-Wno-unused -Drtlsdr_STATIC -DDETACH_KERNEL_DRIVER=1 -Ithird_party/rtl-sdr-blog/include
LIBUSB_CFLAGS := -std=gnu11 $(OPT) -g -pthread -fvisibility=hidden -Wall -Wextra \
	-Ithird_party/libusb-config -Ithird_party/libusb/libusb
# Third-party headers are included as system headers, so that the warning
# options above apply to the module's own code only.
RTLSDR_INCLUDE := -isystem third_party/rtl-sdr-blog/include
VENDOR_LIBUSB_INCLUDE := -isystem third_party/libusb/libusb
# Only evaluated when the native build needs it.
SYSTEM_LIBUSB_CFLAGS = $(patsubst -I%,-isystem %,$(shell pkg-config --cflags libusb-1.0))
SYSTEM_LIBUSB_LIBS = $(shell pkg-config --libs libusb-1.0)

MODULE_SRCS := src/json.cpp src/io.cpp src/log.cpp src/settings.cpp src/receiver.cpp src/stream.cpp \
	src/session.cpp src/listing.cpp src/tuning.cpp src/gain_control.cpp
PROGRAM_SRCS := src/main.cpp src/rtlsdr_backend.cpp
TEST_SRCS := tests/test_main.cpp tests/fake_backend.cpp tests/test_json.cpp tests/test_settings.cpp \
	tests/test_receiver.cpp tests/test_stream.cpp tests/test_session.cpp tests/test_listing.cpp tests/test_tuning.cpp \
	tests/test_gain_control.cpp
# librtlsdr.c itself against a fake libusb, in a program of its own: the
# tuning tests stand in for parts of librtlsdr.c.
DRIVER_TEST_SRCS := tests/test_main.cpp tests/test_driver.cpp tests/fake_libusb.cpp src/log.cpp
RTLSDR_SRCS := librtlsdr.c tuner_e4k.c tuner_fc0012.c tuner_fc0013.c tuner_fc2580.c tuner_r82xx.c
LIBUSB_SRCS := core.c descriptor.c hotplug.c io.c sync.c strerror.c os/linux_usbfs.c os/linux_netlink.c \
	os/events_posix.c os/threads_posix.c

# The licences and authors of the libraries in the executable, for
# fern-rtlsdr --notices; librtlsdr's licence is the module's own.
NOTICES := LICENSE third_party/rtl-sdr-blog/AUTHORS third_party/libusb/COPYING third_party/libusb/AUTHORS
NOTICES_SRC := build/gen/notices.cpp

cxx_objs = $(patsubst %.cpp,$(1)/obj/%.o,$(2))
rtlsdr_objs = $(patsubst %.c,$(1)/obj/rtlsdr/%.o,$(RTLSDR_SRCS))
libusb_objs = $(patsubst %.c,$(1)/obj/libusb/%.o,$(LIBUSB_SRCS))

# Compile rules for one build directory.
#   $(1) directory, $(2) C compiler, $(3) C++ compiler, $(4) extra flags,
#   $(5) libusb include flags for code that includes libusb.h
define compile_rules
$(1)/obj/src/%.o: src/%.cpp
	@mkdir -p $$(@D)
	$(3) $$(CXXFLAGS_BASE) $(4) $$(RTLSDR_INCLUDE) $(5) -MMD -MP -c $$< -o $$@
$(1)/obj/tests/%.o: tests/%.cpp
	@mkdir -p $$(@D)
	$(3) $$(CXXFLAGS_BASE) $(4) -Isrc $$(RTLSDR_INCLUDE) $(5) -MMD -MP -c $$< -o $$@
$(1)/obj/gen/notices.o: $(NOTICES_SRC)
	@mkdir -p $$(@D)
	$(3) $$(CXXFLAGS_BASE) $(4) -c $$< -o $$@
$(1)/obj/rtlsdr/%.o: third_party/rtl-sdr-blog/src/%.c
	@mkdir -p $$(@D)
	$(2) $$(RTLSDR_CFLAGS) $(4) $(5) -MMD -MP -c $$< -o $$@
$(1)/obj/libusb/%.o: third_party/libusb/libusb/%.c
	@mkdir -p $$(@D)
	$(2) $$(LIBUSB_CFLAGS) $(4) -MMD -MP -c $$< -o $$@
endef

NATIVE_DIR := build/native
STATIC_DIR := build/static-$(ARCH)
TEST_DIR := build/test
ASAN_DIR := build/asan
TSAN_DIR := build/tsan
# TSan needs the address space laid out without the randomisation newer
# kernels apply.
HOST_ARCH_UNAME := $(shell uname -m)
SANITIZE := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined

$(eval $(call compile_rules,$(NATIVE_DIR),$(CC),$(CXX),,$$(SYSTEM_LIBUSB_CFLAGS)))
$(eval $(call compile_rules,$(STATIC_DIR),$(CROSS)gcc,$(CROSS)g++,$(ARCH_FLAGS) -ffunction-sections -fdata-sections,$(VENDOR_LIBUSB_INCLUDE)))
$(eval $(call compile_rules,$(TEST_DIR),$(CC),$(CXX),,$(VENDOR_LIBUSB_INCLUDE)))
$(eval $(call compile_rules,$(ASAN_DIR),$(CC),$(CXX),$(SANITIZE),$(VENDOR_LIBUSB_INCLUDE)))
$(eval $(call compile_rules,$(TSAN_DIR),$(CC),$(CXX),-fsanitize=thread,$(VENDOR_LIBUSB_INCLUDE)))

NATIVE_OBJS := $(call cxx_objs,$(NATIVE_DIR),$(MODULE_SRCS) $(PROGRAM_SRCS)) $(call rtlsdr_objs,$(NATIVE_DIR)) \
	$(NATIVE_DIR)/obj/gen/notices.o
STATIC_OBJS := $(call cxx_objs,$(STATIC_DIR),$(MODULE_SRCS) $(PROGRAM_SRCS)) $(call rtlsdr_objs,$(STATIC_DIR)) \
	$(STATIC_DIR)/obj/gen/notices.o \
	$(call libusb_objs,$(STATIC_DIR))
# The tests run the R820T/R828D driver itself against a register file of
# their own; see tests/test_tuning.cpp.
TEST_OBJS := $(call cxx_objs,$(TEST_DIR),$(MODULE_SRCS) $(TEST_SRCS)) $(TEST_DIR)/obj/rtlsdr/tuner_r82xx.o
ASAN_OBJS := $(call cxx_objs,$(ASAN_DIR),$(MODULE_SRCS) $(TEST_SRCS)) $(ASAN_DIR)/obj/rtlsdr/tuner_r82xx.o
DRIVER_TEST_OBJS := $(call cxx_objs,$(TEST_DIR),$(DRIVER_TEST_SRCS)) $(call rtlsdr_objs,$(TEST_DIR))
DRIVER_ASAN_OBJS := $(call cxx_objs,$(ASAN_DIR),$(DRIVER_TEST_SRCS)) $(call rtlsdr_objs,$(ASAN_DIR))
DRIVER_TSAN_OBJS := $(call cxx_objs,$(TSAN_DIR),$(DRIVER_TEST_SRCS)) $(call rtlsdr_objs,$(TSAN_DIR))

PACKAGE := dist/rtlsdr-$(VERSION)-linux-$(ARCH).fernmod
# The settings in every package come from --describe of a binary that runs
# here, built from the same sources, so all platforms carry the same list.
DESCRIBE_BIN := build/static-$(HOST_ARCH)/fern-rtlsdr

.PHONY: all static package test test-asan test-tsan clean print-version check-libusb check-toolchain FORCE

all: build/fern-rtlsdr

check-libusb:
	@pkg-config --exists libusb-1.0 || { echo "The native build needs the libusb-1.0 development files" \
		"(Debian and Ubuntu: apt install libusb-1.0-0-dev pkg-config). make static needs neither." >&2; exit 1; }

$(call rtlsdr_objs,$(NATIVE_DIR)) $(NATIVE_DIR)/obj/src/rtlsdr_backend.o: | check-libusb

$(NOTICES_SRC): $(NOTICES) tools/embed_notices.py
	@mkdir -p $(@D)
	python3 tools/embed_notices.py $(NOTICES) > $@.tmp
	mv $@.tmp $@

build/fern-rtlsdr: $(NATIVE_OBJS) | check-libusb
	$(CXX) -pthread -o $@ $^ $(SYSTEM_LIBUSB_LIBS)

TOOLCHAIN_PACKAGES_x86_64 := gcc-x86-64-linux-gnu g++-x86-64-linux-gnu
TOOLCHAIN_PACKAGES_aarch64 := gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
TOOLCHAIN_PACKAGES_armhf := gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf

check-toolchain:
	@command -v $(CROSS)g++ >/dev/null 2>&1 || { echo "make static ARCH=$(ARCH) needs $(CROSS)gcc and $(CROSS)g++" \
		"(Debian and Ubuntu: apt install $(TOOLCHAIN_PACKAGES_$(ARCH)))." >&2; exit 1; }

$(STATIC_OBJS): | check-toolchain

static: $(STATIC_DIR)/fern-rtlsdr

$(STATIC_DIR)/fern-rtlsdr: $(STATIC_OBJS)
	$(CROSS)g++ $(ARCH_FLAGS) -static -pthread -s -Wl,--gc-sections -o $@ $^

ifneq ($(ARCH),$(HOST_ARCH))
$(DESCRIBE_BIN): FORCE
	$(MAKE) static ARCH=$(HOST_ARCH)
endif

package: $(PACKAGE)

$(PACKAGE): $(STATIC_DIR)/fern-rtlsdr $(DESCRIBE_BIN) tools/mkfernmod.py tools/check_fernmod.py
	@mkdir -p dist
	python3 tools/mkfernmod.py --executable $(STATIC_DIR)/fern-rtlsdr --describe-with $(DESCRIBE_BIN) \
		--platform linux-$(ARCH) --version $(VERSION) --output $@
	python3 tools/check_fernmod.py $@

$(TEST_DIR)/fern-rtlsdr-tests: $(TEST_OBJS)
	$(CXX) -pthread -o $@ $^

$(ASAN_DIR)/fern-rtlsdr-tests: $(ASAN_OBJS)
	$(CXX) $(SANITIZE) -pthread -o $@ $^

$(TEST_DIR)/fern-rtlsdr-driver-tests: $(DRIVER_TEST_OBJS)
	$(CXX) -pthread -o $@ $^

$(ASAN_DIR)/fern-rtlsdr-driver-tests: $(DRIVER_ASAN_OBJS)
	$(CXX) $(SANITIZE) -pthread -o $@ $^

$(TSAN_DIR)/fern-rtlsdr-driver-tests: $(DRIVER_TSAN_OBJS)
	$(CXX) -fsanitize=thread -pthread -o $@ $^

test: $(TEST_DIR)/fern-rtlsdr-tests $(TEST_DIR)/fern-rtlsdr-driver-tests build/fern-rtlsdr
	$(TEST_DIR)/fern-rtlsdr-tests
	$(TEST_DIR)/fern-rtlsdr-driver-tests
	sh tests/cli_test.sh build/fern-rtlsdr
	sh tests/package_test.sh build/fern-rtlsdr

test-asan: $(ASAN_DIR)/fern-rtlsdr-tests $(ASAN_DIR)/fern-rtlsdr-driver-tests
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1 $(ASAN_DIR)/fern-rtlsdr-tests
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1 $(ASAN_DIR)/fern-rtlsdr-driver-tests

test-tsan: $(TSAN_DIR)/fern-rtlsdr-driver-tests
	TSAN_OPTIONS=halt_on_error=1 setarch $(HOST_ARCH_UNAME) -R $(TSAN_DIR)/fern-rtlsdr-driver-tests

print-version:
	@echo $(VERSION)

clean:
	rm -rf build dist

FORCE:

-include $(shell find build -name '*.d' 2>/dev/null)
