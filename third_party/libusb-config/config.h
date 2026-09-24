/*
 * libusb 1.0.27 configuration for a static Linux build without udev.
 *
 * Written for Fern-RTLSDR in place of the header that libusb's configure
 * script generates. Every macro below is one that configure.ac would define
 * on a glibc Linux system with "--disable-udev". Device enumeration and
 * hotplug then use sysfs and a netlink socket (os/linux_netlink.c).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later (same as libusb)
 */

#define DEFAULT_VISIBILITY __attribute__ ((visibility ("default")))
#define PRINTF_FORMAT(a, b) __attribute__ ((__format__ (__printf__, a, b)))

/* Messages are compiled in but stay silent unless LIBUSB_DEBUG is set. */
#define ENABLE_LOGGING 1

#define PLATFORM_POSIX 1

#define HAVE_ASM_TYPES_H 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_EVENTFD 1
#define HAVE_NFDS_T 1
#define HAVE_PIPE2 1
#define HAVE_PTHREAD_CONDATTR_SETCLOCK 1
#define HAVE_PTHREAD_SETNAME_NP 1
#define HAVE_SYS_TIME_H 1
#define HAVE_TIMERFD 1

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
