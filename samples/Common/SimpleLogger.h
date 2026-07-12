#pragma once

#include <windows.h>
#include <mutex>
#include <atomic>
#include <thread>
#include <string>
#include <cstdio>
#include <cstdarg>

// Logging is enabled only when an (empty) NvFBCR.log already exists beside the exe.
//
// Hot-path contract: log() never performs I/O and never blocks on another thread's I/O.
// Callers include the capture thread, whose wake-to-regrab window directly sets how many
// source flips survive capture (measured: frames submitted during that window coalesce or
// get overwritten). log() formats on the caller's stack and memcpys into a preallocated
// ring under a mutex held only for the copy; a dedicated drainer thread batches the ring
// to disk. When the ring is full, lines are DROPPED and counted, never waited for - a lost
// log line is recoverable, a lost capture frame is not.
class SimpleLogger {
public:
    static constexpr const char* LOG_FILENAME = "NvFBCR.log";
    static constexpr int LOCATION_WIDTH = 24; // Width for [filename:line] padding

    static SimpleLogger& getInstance() {
        static SimpleLogger instance;
        return instance;
    }

    bool isEnabled() const {
        return m_enabled;
    }

    void log(const char* file, int line, const char* fmt, ...) {
        // Fast early exit if logging disabled
        if (!m_enabled) {
            return;
        }

        char buffer[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        // Get just the filename without path
        const char* fileName = strrchr(file, '\\');
        fileName = fileName ? fileName + 1 : file;

        // Format location string with padding
        char location[64];
        snprintf(location, sizeof(location), "[%s:%d]", fileName, line);

        // Format the log line with padded location
        char logLine[kSlotSize];
        int len = snprintf(logLine, sizeof(logLine), "%-*s | %s\n", LOCATION_WIDTH, location, buffer);
        if (len < 0) return;
        if (len >= kSlotSize) len = kSlotSize - 1;   // truncated long line still ends in \n? ensure below
        if (logLine[len - 1] != '\n') { logLine[len - 1] = '\n'; }

#ifdef _DEBUG
        // Debugger echo costs microseconds normally and far more with a listener attached;
        // release builds must keep the hot path free of it.
        OutputDebugStringA(logLine);
#endif

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const int head = m_head.load(std::memory_order_relaxed);
            const int next = (head + 1) % kSlotCount;
            if (next == m_tail.load(std::memory_order_acquire)) {
                m_dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            memcpy(m_slots[head], logLine, (size_t)len);
            m_slotLen[head] = len;
            m_head.store(next, std::memory_order_release);
        }
    }

    // Block until everything queued so far is on disk (cleanup / key moments only).
    void flush() {
        if (!m_enabled) return;
        while (m_tail.load(std::memory_order_acquire) != m_head.load(std::memory_order_acquire)) {
            Sleep(1);
        }
        std::lock_guard<std::mutex> io(m_ioMutex);
        if (m_fileHandle != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(m_fileHandle);
        }
    }

    ~SimpleLogger() {
        m_stopDrainer.store(true);
        if (m_drainer.joinable()) {
            m_drainer.join();
        }
        drain();   // whatever arrived after the drainer exited
        if (m_fileHandle != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(m_fileHandle);
            CloseHandle(m_fileHandle);
        }
    }

private:
    static constexpr int kSlotSize = 512;    // longest current line ~400 bytes
    static constexpr int kSlotCount = 4096;  // ~2 MB; seconds of headroom at peak line rates

    SimpleLogger() : m_fileHandle(INVALID_HANDLE_VALUE), m_enabled(false),
                     m_head(0), m_tail(0), m_dropped(0), m_stopDrainer(false) {
        // Check if log file exists at startup
        WIN32_FIND_DATAA findData;
        HANDLE hFind = FindFirstFileA(LOG_FILENAME, &findData);

        if (hFind != INVALID_HANDLE_VALUE) {
            FindClose(hFind);

            // Open file using Win32 API, truncating any existing content
            m_fileHandle = CreateFileA(
                LOG_FILENAME,
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL,
                CREATE_ALWAYS,  // Always create new file, truncating if exists
                FILE_ATTRIBUTE_NORMAL,
                NULL
            );

            if (m_fileHandle != INVALID_HANDLE_VALUE) {
                m_enabled = true;
                m_drainer = std::thread(&SimpleLogger::drainLoop, this);
            }
        }
    }

    // Copy the pending ring range into a local batch and write it in one call. Slots in
    // [tail, head) are stable: producers drop rather than overwrite unconsumed slots, so
    // reading them without the producer mutex is safe; only m_tail (drainer-owned) moves.
    void drain() {
        char batch[64 * 1024];
        int used = 0;
        int tail = m_tail.load(std::memory_order_relaxed);
        const int head = m_head.load(std::memory_order_acquire);
        while (tail != head) {
            const int len = m_slotLen[tail];
            if (used + len > (int)sizeof(batch)) {
                writeBatch(batch, used);
                used = 0;
            }
            memcpy(batch + used, m_slots[tail], (size_t)len);
            used += len;
            tail = (tail + 1) % kSlotCount;
            m_tail.store(tail, std::memory_order_release);
        }
        if (used > 0) {
            writeBatch(batch, used);
        }

        const long long dropped = m_dropped.exchange(0, std::memory_order_relaxed);
        if (dropped > 0) {
            char note[128];
            int len = snprintf(note, sizeof(note),
                "%-*s | SimpleLogger: %lld line(s) dropped (ring full)\n",
                LOCATION_WIDTH, "[SimpleLogger]", dropped);
            if (len > 0) writeBatch(note, len);
        }
    }

    void writeBatch(const char* data, int len) {
        std::lock_guard<std::mutex> io(m_ioMutex);
        if (m_fileHandle != INVALID_HANDLE_VALUE) {
            DWORD bytesWritten;
            WriteFile(m_fileHandle, data, (DWORD)len, &bytesWritten, NULL);
        }
    }

    void drainLoop() {
        while (!m_stopDrainer.load(std::memory_order_acquire)) {
            drain();
            Sleep(10);
        }
    }

    SimpleLogger(const SimpleLogger&) = delete;
    SimpleLogger& operator=(const SimpleLogger&) = delete;

    std::mutex m_mutex;                 // producers: serializes slot claim + copy (sub-us hold)
    std::mutex m_ioMutex;               // WriteFile/FlushFileBuffers (drainer + flush only)
    HANDLE m_fileHandle;
    bool m_enabled;
    char m_slots[kSlotCount][kSlotSize];
    int m_slotLen[kSlotCount];
    std::atomic<int> m_head;            // producer-advanced (under m_mutex)
    std::atomic<int> m_tail;            // drainer-advanced
    std::atomic<long long> m_dropped;
    std::atomic<bool> m_stopDrainer;
    std::thread m_drainer;
};

// Convenience macros for backward compatibility
#ifndef LOG
#define LOG(fmt, ...) SimpleLogger::getInstance().log(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#endif

#define LOGERR(fmt, ...) SimpleLogger::getInstance().log(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOGOUT(fmt, ...) SimpleLogger::getInstance().log(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
