#pragma once

#include <windows.h>
#include <mutex>
#include <string>
#include <cstdio>
#include <cstdarg>

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
        char logLine[2300];
        snprintf(logLine, sizeof(logLine), "%-*s | %s\n", LOCATION_WIDTH, location, buffer);

        std::lock_guard<std::mutex> lock(m_mutex);

        // Write to file handle (no flush - let OS handle buffering)
        if (m_fileHandle != INVALID_HANDLE_VALUE) {
            DWORD bytesWritten;
            WriteFile(m_fileHandle, logLine, (DWORD)strlen(logLine), &bytesWritten, NULL);
        }

        // Write to debugger
        OutputDebugStringA(logLine);
    }

    // Manually flush the log file (call this at cleanup or key moments)
    void flush() {
        if (m_enabled && m_fileHandle != INVALID_HANDLE_VALUE) {
            std::lock_guard<std::mutex> lock(m_mutex);
            FlushFileBuffers(m_fileHandle);
        }
    }

    ~SimpleLogger() {
        if (m_fileHandle != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(m_fileHandle);
            CloseHandle(m_fileHandle);
        }
    }

private:
    SimpleLogger() : m_fileHandle(INVALID_HANDLE_VALUE), m_enabled(false) {
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
            }
        }
    }

    SimpleLogger(const SimpleLogger&) = delete;
    SimpleLogger& operator=(const SimpleLogger&) = delete;

    std::mutex m_mutex;
    HANDLE m_fileHandle;
    bool m_enabled;
};

// Convenience macros for backward compatibility
#ifndef LOG
#define LOG(fmt, ...) SimpleLogger::getInstance().log(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#endif

#define LOGERR(fmt, ...) SimpleLogger::getInstance().log(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOGOUT(fmt, ...) SimpleLogger::getInstance().log(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
