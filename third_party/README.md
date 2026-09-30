# Third-party code

Both libraries are copied from their upstream repositories, with only the
files the build compiles and their licenses. libusb is unmodified; the
changes to librtlsdr are listed below. The Makefile compiles
them directly; their own build systems are not used.

## rtl-sdr-blog (librtlsdr)

- Upstream: https://github.com/rtlsdrblog/rtl-sdr-blog
- Tag: `V1.4.0`, commit `aed0ea19f3a273370a13c9009b96313c75d54c7b`
- License: GPL-2.0-or-later, see `rtl-sdr-blog/COPYING`; authors in `rtl-sdr-blog/AUTHORS`
- Files: `src/librtlsdr.c`, `src/tuner_e4k.c`, `src/tuner_fc0012.c`,
  `src/tuner_fc0013.c`, `src/tuner_fc2580.c`, `src/tuner_r82xx.c` and the
  headers in `include/`

Compiled with `-Drtlsdr_STATIC` (no export attributes) and
`-DDETACH_KERNEL_DRIVER=1`, so that a dongle claimed by the dvb_usb_rtl28xxu
kernel driver is detached on open and reattached on close, as Debian builds
the library. No version macros are needed; the CMake-generated version
header is not used by these files.

Fern-RTLSDR also calls three functions that `librtlsdr.c` exports without
declaring them in `rtl-sdr.h`: `rtlsdr_set_i2c_repeater`,
`rtlsdr_i2c_write_fn` and `rtlsdr_i2c_read_fn`. It uses them to read the
R820T/R828D PLL lock flag, which librtlsdr does not report
(`src/rtlsdr_backend.cpp`).

### Changes to librtlsdr

Each is marked with a comment starting `Fern-RTLSDR:` in `src/librtlsdr.c`,
and `tests/test_driver.cpp` runs the file against a fake libusb to check it.

- `rtlsdr_read_eeprom()` treats a short transfer as a failure, and
  `rtlsdr_open()` forces the bias tee on only when the EEPROM was read and
  asks for that. Upstream decides from an uninitialised buffer when the
  EEPROM does not answer.
- `rtlsdr_set_bias_tee_gpio()` returns -1 when a GPIO register cannot be
  read or written, where upstream always returns 0, and writes nothing after
  a failed read. The RTL-SDR Blog V4's input switch, which uses the same
  function, then fails the tune instead of passing unnoticed.
- `rtlsdr_read_async()` returns `-ENOMEM` when its transfers or buffers
  cannot be allocated, after freeing what was, where upstream goes on to
  fill and submit NULL transfers.
- `rtlsdr_read_async()` and `rtlsdr_cancel_async()` share the streaming
  state through atomic operations, and cancelling wakes the event loop with
  `libusb_interrupt_event_handler()` instead of a completion flag that
  libusb reads without synchronisation. Upstream races there when, as in
  this module, another thread cancels; `make test-tsan` shows it.

## libusb

- Upstream: https://github.com/libusb/libusb
- Tag: `v1.0.30`, commit `87a55632db62c9bdc58cd31d3ccfa673f1bb017f`. From 1.0.27 because
  of CVE-2026-23679 and CVE-2026-47104 in descriptor parsing, which neither
  librtlsdr nor this module reaches, but which scanners flag in the binary.
- License: LGPL-2.1-or-later, see `libusb/COPYING`; authors in `libusb/AUTHORS`
- Files: `libusb/core.c`, `descriptor.c`, `hotplug.c`, `io.c`, `sync.c`,
  `strerror.c`, `os/linux_usbfs.c`, `os/linux_netlink.c`,
  `os/events_posix.c`, `os/threads_posix.c` and the headers they include

Used only by `make static` and `make package`; the native `make` links the
system libusb. Built without udev: devices are found through sysfs and
usbfs, hotplug events arrive on a netlink socket. `libusb-config/config.h`
is written for this project in place of the header that libusb's configure
script generates; it defines what configure would define on glibc Linux with
`--disable-udev`.

## Updating

Clone the new tag with `git clone --depth 1 --branch <tag> <url>`, copy the
same files over the ones here, carry the changes listed above over to the
new `librtlsdr.c`, update the commit hashes above and the
librtlsdr and libusb versions in the start-up log line and in `notices()` in
`src/main.cpp`, and run
`make test`, `make static` and `make static ARCH=aarch64`. For librtlsdr, also
check that `rtlsdr_open()`, `rtlsdr_read_async()`, `rtlsdr_close()`,
`rtlsdr_set_center_freq()`, `rtlsdr_set_offset_tuning()` and
`r82xx_set_pll()` still behave as `src/receiver.cpp`, `src/stream.cpp` and
`src/rtlsdr_backend.cpp` expect.
