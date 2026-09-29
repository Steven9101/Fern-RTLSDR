# Fern-RTLSDR

Fern-RTLSDR lets [FernSDR](https://github.com/Steven9101/websdr) receive with
an RTL-SDR dongle. It is an input module: a separate program that FernSDR
starts for a band and talks to over pipes, as FernSDR's `docs/MODULES.md`
describes. FernSDR sends the band's settings, the module opens and tunes the
dongle, then writes the dongle's unsigned 8-bit I/Q samples to FernSDR
unchanged, reports statistics once a second and accepts gain, AGC and bias
tee changes while it runs.

Keeping the USB and vendor code in a module means FernSDR itself links only
the C and C++ runtime. The module is one statically linked executable; the
librtlsdr and libusb it needs are built into it.

## Hardware

Any RTL2832U dongle with one of these tuners:

- Rafael Micro R820T or R820T2, as on the RTL-SDR Blog V3 and most generic dongles
- Rafael Micro R828D, as on the RTL-SDR Blog V4
- Elonics E4000
- Fitipower FC0012 and FC0013
- FCI FC2580

The module includes librtlsdr from the RTL-SDR Blog
[rtl-sdr-blog](https://github.com/rtlsdrblog/rtl-sdr-blog) V1.4.0, which
drives the RTL-SDR Blog V4 correctly: it switches the V4's inputs and uses its
built-in upconverter below 28.8 MHz, so the V4 receives HF with
`direct_sampling` off. On the RTL-SDR Blog V3, HF (below about 24 MHz) comes
through the Q branch. `direct_sampling = auto`, the default, does the right
thing for either: off on a V4, and the Q branch on any other dongle for a
band below where its tuner starts (24 MHz for the R820T). Generic R820T
dongles have nothing connected to the Q branch, so HF needs a V3, a V4 or
an upconverter there.

Sample rates from 225001 to 300000 Hz and from 900001 to 3200000 Hz are
possible. 2400000 is the highest rate that most computers sustain without
losing samples.

## Using it with FernSDR

Install the package for your machine from the admin panel, which offers the
releases of this repository, or from a shell:

```sh
fernsdr --install-module rtlsdr-0.1.0-linux-aarch64.fernmod fernsdr.conf
```

Then give a band `source = module`:

```ini
[band:20m]
name          = 20 m
source        = module
module        = rtlsdr
sample_rate   = 2400000
center        = 14200000
module.device = serial:00000001
module.gain   = auto
```

`fern-rtlsdr --list-devices` prints the serial numbers of the dongles that are
plugged in. With only one dongle, `module.device` can be left out.

### Settings

| key | type | default | changes while running | meaning |
|---|---|---|---|---|
| `device` | string | empty | no | `serial:<serial>` or `index:<n>`; empty when exactly one RTL-SDR is plugged in |
| `gain` | string | `auto` | yes | `auto` for the module's own control, `tuner` for the tuner's AGC, or a gain in dB such as `38.6` |
| `ppm` | number | 0 | no | crystal error in parts per million, -488 to 488 |
| `rtl_agc` | boolean | no | yes | the RTL2832U's digital AGC |
| `bias_tee` | boolean | no | yes | 4.5 V on the antenna input (RTL-SDR Blog V3 and V4) |
| `direct_sampling` | choice | `auto` | no | `auto`, `off`, `i` or `q` |
| `offset_tuning` | boolean | no | no | E4000, FC0012, FC0013 and FC2580 only |
| `bandwidth` | number | 0 | no | tuner IF filter in Hz, 0 to 8000000; 0 follows the sample rate. R820T, R828D and E4000 only; `ready` reports the filter the tuner chose |
| `buffers` | number | 16 | no | USB transfers in flight, 2 to 64, each about 20 ms of samples |

`fern-rtlsdr --describe` prints the same list as JSON; the package manifest
carries it too, and FernSDR checks a band's settings against it.

### Gain

With `gain = auto` the module sets the tuner's gain itself: the highest step
that keeps the RTL2832U's 8-bit converter out of clipping with 6 dB to spare.
It starts at 29.7 dB. When three tenths of a second of the last second each
clipped more than one sample in 10,000, it comes down a step, and 6 dB when
those clipped more than one in 100. It goes up a step once little more than
the odd crash has clipped for 5 seconds, in the first two minutes, or a
minute after that, and the peaks of all but the highest twentieth of those
tenths would stay 6 dB under full scale one step higher. A crash of static
or a spark that clips for a millisecond neither brings the gain down nor
keeps it from going up: a gain lowered for it would cost the band its
sensitivity for good. When even the lowest step clips, the module's log says
that only an attenuator in front of the dongle helps.

`gain = tuner` leaves the gain to the tuner's own AGC, which watches the
tuner's power detectors rather than the converter, and a number fixes the
gain. FernSDR's S-meter calibration holds at the gain it was made at, so a
calibrated band wants a fixed gain.

### What the module reports

The `ready` message tells FernSDR what the hardware actually does:

- `sample_rate` is the rate the RTL2832U resampler runs at, computed the way
  librtlsdr programs it. For 2400000 and 2048000 it is exact; for 1000000 it
  is 1000000.026 Hz.
- `center` is where the dongle really tuned. librtlsdr sets the R820T and
  R828D synthesizer with integer arithmetic, and it lands beside the
  frequency asked for: at 2.4 Msps an RTL-SDR Blog V4 within 76 Hz on HF
  (half the time within 11 Hz), within 340 Hz up to 250 MHz and within
  1.3 kHz above (half the time 230 Hz), an R820T the same above 24 MHz. The
  RTL2832U's mixer, set in 6.9 Hz steps, then adds up to 7 Hz. The module
  repeats librtlsdr's arithmetic to know the synthesizer's frequency, and
  sets the RTL2832U's mixer to put the frequency asked for at 0 Hz, which
  leaves at most 3.4 Hz.
  In direct sampling only the mixer's rounding is left to improve. For the
  E4000, FC0012, FC0013 and FC2580 `center` is the frequency asked for, as
  librtlsdr reports it. The arithmetic is checked in the tests against
  librtlsdr's own tuner driver, run on a register file of its own, over
  8,640 cases; it has not yet been measured on a dongle.
- `settings.bandwidth` is the IF filter the tuner driver chose for the
  requested bandwidth, or for the sample rate when none was requested;
  0 in direct sampling.
- `settings.gain` is `auto`, `tuner`, or the gain the tuner uses: a request
  is moved to the nearest step the tuner has, and `set` answers with the step
  it chose.
- `stats` carry `clipping`, the share of the samples since the last stats
  whose I or Q sat at 0 or 255, and with `gain = auto` the `gain` in use.
- `settings.bias_tee_effective` says whether the bias tee is really on. The
  EEPROM of RTL-SDR Blog dongles can force it on whatever the setting says
  (`rtl_eeprom -b 0` clears that); the module reads the EEPROM and reports
  `true`, `false`, or `unknown` when the EEPROM cannot be read.

The module refuses, with an error that says what to change, anything it
cannot honour instead of guessing: a frequency the tuner cannot tune or whose
PLL does not lock, HF on an R820T without direct sampling, offset tuning on
R820T and R828D tuners (librtlsdr would switch the bias tee instead), a gain
on a tuner that has none, a setting it does not know. When the dongle is
unplugged or stops sending samples for 1.5 seconds, it reports `lost` and
exits with status 5, and FernSDR starts it again.

## Permissions

The user FernSDR runs as needs read and write access to the dongle's USB
device node, and the kernel's DVB driver must leave the dongle alone.

FernSDR's `install.sh`, which installs a release as a systemd service, sets
all of this up: it writes the udev rule for the receiver's own group,
blacklists the DVB-T driver and lets the service open USB devices and nothing
else. For a FernSDR built from source and installed as a service,
`sudo tools/source-install.sh --service --usb` in its checkout does the same.
The rest of this section is what they do, for a setup neither covers.

Allow the `plugdev` group to use RTL2832U dongles, in
`/etc/udev/rules.d/60-fern-rtlsdr.rules`:

```
SUBSYSTEM=="usb", ATTRS{idVendor}=="0bda", ATTRS{idProduct}=="2838", MODE="0660", GROUP="plugdev"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0bda", ATTRS{idProduct}=="2832", MODE="0660", GROUP="plugdev"
```

These two IDs cover the RTL-SDR Blog dongles and most generic ones; `lsusb`
shows which one yours has. Then:

```sh
sudo udevadm control --reload-rules
sudo udevadm trigger
sudo usermod -aG plugdev fernsdr     # the user FernSDR runs as
```

Stop the DVB-T driver from claiming the dongle, in
`/etc/modprobe.d/blacklist-rtl-sdr.conf`:

```
blacklist dvb_usb_rtl28xxu
```

and unload it once with `sudo modprobe -r dvb_usb_rtl28xxu`, or reboot. The
module tries to detach the driver itself when it finds it attached, but the
blacklist is the reliable way.

### FernSDR under systemd

A unit that hides USB devices from the receiver (`PrivateDevices=yes`) or
forbids the netlink socket libusb uses to watch for USB devices
(`RestrictAddressFamilies` without `AF_NETLINK`), as the one
`tools/source-install.sh --service` writes without `--usb` does, is
inherited by a module FernSDR starts, which then reports exactly that in its
error message. To let it reach the dongle, add a drop-in with
`sudo systemctl edit fernsdr`:

```ini
[Service]
PrivateDevices=no
DeviceAllow=char-usb_device rw
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX AF_NETLINK
SupplementaryGroups=plugdev
```

`DeviceAllow` keeps every other device closed. The module runs under the
unit's `SystemCallFilter=@system-service` and `MemoryDenyWriteExecute=yes`.

## Running it by hand

```sh
fern-rtlsdr --describe        # the module and its settings, as JSON
fern-rtlsdr --list-devices    # the RTL-SDRs this machine can see, as JSON
```

`--fernsdr-module 1` is how FernSDR starts it, with commands on fd 0,
samples on fd 1, a log on fd 2 and events on fd 3.

## Building

You need GNU make, a C and C++ compiler with C++17 support, and Python 3 for
packaging and tests. CMake is not used.

```sh
make                          # build/fern-rtlsdr, using the system libusb
make static                   # build/static-x86_64/fern-rtlsdr, fully static
make static ARCH=aarch64      # cross build, needs aarch64-linux-gnu-gcc and g++
make static ARCH=armhf        # cross build, needs arm-linux-gnueabihf-gcc and g++
make package ARCH=aarch64     # dist/rtlsdr-0.1.0-linux-aarch64.fernmod
make test                     # unit, protocol, command line and package tests
make test-asan                # the unit and protocol tests under ASan and UBSan
```

`make` needs the libusb development files (`apt install libusb-1.0-0-dev
pkg-config`). `make static` and `make package` need none: they build the
vendored librtlsdr and libusb, the latter without udev. On Debian and Ubuntu
the cross compilers are the packages `gcc-aarch64-linux-gnu`,
`g++-aarch64-linux-gnu`, `gcc-arm-linux-gnueabihf` and
`g++-arm-linux-gnueabihf`.

`armhf` means ARMv7 with hardware floating point (`-march=armv7-a
-mfpu=vfpv3-d16 -mfloat-abi=hard`): every Raspberry Pi from the Pi 2 on, and
other ARMv7 boards. The Pi Zero and the Pi 1 are ARMv6 and are not supported.
A 64-bit operating system on a Pi 3, 4 or 5 uses the `aarch64` package.

The tests need no dongle. A fake device that streams a known tone drives the
same code as the real one through real pipes, and can simulate a busy, an
unplugged and a stalled dongle.

## Releases

Pushing a tag `v<version>` that matches `VERSION` in the Makefile runs
`.github/workflows/release.yml`: it runs the tests, builds the three static
packages, runs the cross-built programs under QEMU and attaches
`rtlsdr-<version>-linux-<platform>.fernmod` to the GitHub release.

## License

Fern-RTLSDR is free software under the GNU General Public License, version 2
or (at your option) any later version; see [LICENSE](LICENSE). It includes
librtlsdr from rtl-sdr-blog (GPL-2.0-or-later) and libusb (LGPL-2.1-or-later);
see [third_party/README.md](third_party/README.md).
