// Fern-RTLSDR, an RTL-SDR input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Moves samples from the librtlsdr callback to fd 1. The callback copies
// into a ring and never blocks; a writer thread empties the ring into fd 1
// with blocking writes. When the ring is full the newest samples are dropped
// and counted.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

#include "device.h"

namespace fern {

// Single producer, single consumer. The capacity is a power of two, so that
// the free-running positions can wrap around.
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity);

    size_t capacity() const { return mask_ + 1; }
    size_t size() const;

    // Producer: stores all of data, or nothing when it does not fit.
    bool push(const uint8_t* data, size_t len);

    // Consumer: the longest contiguous readable span, then release part of it.
    size_t peek(const uint8_t** data) const;
    void consume(size_t len);

private:
    std::unique_ptr<uint8_t[]> buf_;
    size_t mask_;
    std::atomic<size_t> head_{0};  // written by the producer
    std::atomic<size_t> tail_{0};  // written by the consumer
};

class Stream {
public:
    enum class WriterEnd { running, stopped, host_gone, failed };

    // notify_fd, an eventfd, is signalled when either thread ends.
    Stream(Device& device, int samples_fd, int notify_fd, size_t ring_bytes, uint32_t transfers,
           uint32_t transfer_bytes);
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    // Starts the writer and the USB reader. False when a thread could not be
    // created; nothing is left running then.
    bool start();
    // Cancels the USB transfers and stops the writer; does not wait.
    void request_stop();
    // Keeps nudging both threads until they have ended or the deadline
    // passes. Returns whether both ended; only then may the device be closed.
    bool wait(std::chrono::steady_clock::time_point deadline);

    bool reader_ended() const { return reader_ended_.load(); }
    bool stop_requested() const { return stop_requested_.load(); }
    WriterEnd writer_end() const { return writer_end_.load(); }
    int writer_errno() const { return writer_errno_.load(); }

    // Samples are I/Q pairs, two bytes each.
    uint64_t samples_delivered() const { return bytes_written_.load() / 2; }
    uint64_t samples_dropped() const { return bytes_dropped_.load() / 2; }
    uint64_t bytes_received() const { return bytes_received_.load(); }
    // steady_clock time of the last USB callback, or of start() before one.
    std::chrono::steady_clock::time_point last_data() const;

private:
    static void on_samples(unsigned char* buf, uint32_t len, void* ctx);
    void reader_main();
    void writer_main();
    void notify(int fd);

    Device& device_;
    const int samples_fd_;
    const int notify_fd_;
    const uint32_t transfers_;
    const uint32_t transfer_bytes_;
    RingBuffer ring_;
    int data_event_ = -1;

    std::thread reader_;
    std::thread writer_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> reader_ended_{false};
    std::atomic<bool> writer_stop_{false};
    std::atomic<WriterEnd> writer_end_{WriterEnd::running};
    std::atomic<int> writer_errno_{0};

    std::atomic<uint64_t> bytes_received_{0};
    std::atomic<uint64_t> bytes_dropped_{0};
    std::atomic<uint64_t> bytes_written_{0};
    std::atomic<int64_t> last_data_ns_{0};
};

}  // namespace fern
