// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/signalfd.h>
#include <unistd.h>

#include "failure.h"
#include "io.h"
#include "json.h"
#include "listing.h"
#include "log.h"
#include "rtlsdr_backend.h"
#include "session.h"
#include "settings.h"

using namespace fern;

namespace fern {
extern const char embedded_notices[];  // build/gen/notices.cpp, from tools/embed_notices.py
}

namespace {

const char usage_text[] =
    "usage: fern-rtlsdr --describe          print the module and its settings as JSON\n"
    "       fern-rtlsdr --list-devices      print the RTL-SDRs this machine can see as JSON\n"
    "       fern-rtlsdr --fernsdr-module 1  run as a FernSDR input module (FernSDR starts it so)\n"
    "       fern-rtlsdr --notices           print the licences of this module and the libraries in it\n"
    "       fern-rtlsdr --version\n";

int print(const std::string& text) {
    return write_all(STDOUT_FILENO, text.data(), text.size()) == 0 ? 0 : exit_status::internal;
}

int describe() {
    const std::string text = json::serialize(describe_module()) + "\n";
    if (text.size() > max_report_bytes) {
        std::fprintf(stderr, "fern-rtlsdr: the description is larger than %zu bytes\n", max_report_bytes);
        return exit_status::internal;
    }
    return print(text);
}

int list() {
    RtlsdrBackend backend;
    // FernSDR allows 10 s; leave a margin for starting up and printing.
    const Listing listing = list_devices(backend, std::chrono::milliseconds(8500));
    const int status = print(report_text(listing.report));
    if (!listing.complete)
        _exit(status);  // a worker thread is still inside librtlsdr
    return status;
}

int notices() {
    return print(std::string("fern-rtlsdr ") + module_version() +
                 " is free software under the GNU General Public License, version 2 or later, and contains\n"
                 "librtlsdr from RTL-SDR Blog V1.4.0 (GPL-2.0-or-later) and, in the static build, libusb 1.0.30\n"
                 "(LGPL-2.1-or-later). The source of all three is at https://github.com/Steven9101/Fern-RTLSDR.\n\n" +
                 embedded_notices);
}

int module() {
    for (int fd : {STDIN_FILENO, STDOUT_FILENO, 3}) {
        if (::fcntl(fd, F_GETFD) < 0) {
            std::fprintf(stderr,
                         "fern-rtlsdr: fd %d is not open. --fernsdr-module is how FernSDR runs this program, "
                         "with commands on fd 0, samples on fd 1 and events on fd 3.\n",
                         fd);
            return exit_status::usage;
        }
    }
    std::signal(SIGPIPE, SIG_IGN);

    // SIGTERM and friends arrive through a signalfd, so that the session
    // loop sees them like any other input. Threads started later inherit
    // the mask.
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGTERM);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGHUP);
    if (pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr) != 0) {
        log_line("could not block the stop signals");
        return exit_status::internal;
    }
    const int signal_fd = signalfd(-1, &stop_signals, SFD_CLOEXEC);
    if (signal_fd < 0) {
        log_line("could not create a signalfd: %s", std::strerror(errno));
        return exit_status::internal;
    }

    log_line("fern-rtlsdr %s with librtlsdr from RTL-SDR Blog V1.4.0 and %s", module_version(),
             libusb_version_text().c_str());
    RtlsdrBackend backend;
    SessionIo io;
    io.stop = signal_fd;
    const SessionResult result = run_session(backend, io);
    if (!result.clean)
        _exit(result.status);
    ::close(signal_fd);
    return result.status;
}

}  // namespace

int main(int argc, char** argv) {
    const int n = argc - 1;
    const std::string_view first = n >= 1 ? argv[1] : "";
    if (n == 1 && first == "--describe")
        return describe();
    if (n == 1 && first == "--list-devices")
        return list();
    if (n == 2 && first == "--fernsdr-module") {
        if (std::string_view(argv[2]) == "1")
            return module();
        std::fprintf(stderr, "fern-rtlsdr: this module speaks module API 1, not %s\n", argv[2]);
        return exit_status::usage;
    }
    if (n == 1 && first == "--notices")
        return notices();
    if (n == 1 && first == "--version")
        return print(std::string("fern-rtlsdr ") + module_version() + "\n");
    if (n == 1 && (first == "--help" || first == "-h"))
        return print(usage_text);
    std::fputs(usage_text, stderr);
    return exit_status::usage;
}
