// Switch diagnostic sink: same interface as platform/vita/diagnostic_log.h,
// written with plain stdio on the SD card (libnx maps sdmc: through newlib).
// Each record opens, writes and closes the file so a crash keeps the log.
#pragma once
#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>

namespace vita {
class DiagnosticLog {
public:
    static constexpr size_t kLimit = 1024 * 1024;
    static constexpr const char *kPath = "sdmc:/switch/" M2_ROMSET "/switch-diag.log";

    explicit DiagnosticLog(bool enabled = true) : enabled_(enabled) {}
    bool enabled() const { return enabled_; }

    bool begin() {
        if (!enabled_) return true;
        bytes_ = 0; error_ = sync_error_ = 0; limited_ = false;
        static constexpr char header[] = "DAYTONA SWITCH - stdio file logging\n";
        return write(header, sizeof(header) - 1, true);
    }
    bool append(const char *data, size_t size) { return !enabled_ || write(data, size, false); }
    template<size_t N> bool literal(const char (&data)[N]) { return append(data, N - 1); }
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    bool log(const char *format, ...) {
        if (!enabled_) return true;
        va_list args;
        va_start(args, format);
        const bool ok = formatted(format, args);
        va_end(args);
        return ok;
    }
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    bool fault(const char *format, ...) {
        va_list args;
        va_start(args, format);
        const bool ok = formatted(format, args);
        va_end(args);
        return ok;
    }
    int error() const { return error_; }
    int sync_error() const { return sync_error_; }
    bool limited() const { return limited_; }
    size_t bytes() const { return bytes_; }
    const char *state() const {
        return error_ ? "ERROR" : sync_error_ ? "SYNC WARNING" : limited_ ? "LIMIT" : enabled_ ? "OK" : "OFF";
    }

private:
    bool formatted(const char *format, va_list args) {
        if (!format) { remember(-1); return false; }
        char buffer[1536];
        const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
        if (n < 0) { remember(-1); return false; }
        size_t size = std::min(size_t(n), sizeof(buffer) - 1);
        if (size_t(n) >= sizeof(buffer)) {
            static constexpr char tail[] = " [truncated]\n";
            std::copy_n(tail, sizeof(tail) - 1, buffer + size - (sizeof(tail) - 1));
        }
        return write(buffer, size, false);
    }
    void remember(int error) { if (!error_) error_ = error; }
    bool write(const char *data, size_t size, bool truncate) {
        if (!size) return true;
        if (!data) { remember(-1); return false; }
        if (size > kLimit - bytes_) { limited_ = true; return false; }
        std::FILE *file = std::fopen(kPath, truncate ? "wb" : "ab");
        if (!file) { remember(-2); return false; }
        const size_t written = std::fwrite(data, 1, size, file);
        bytes_ += written;
        bool ok = written == size;
        if (!ok) remember(-3);
        if (std::fflush(file) != 0 && !sync_error_) sync_error_ = -4;
        if (std::fclose(file) != 0) { remember(-5); ok = false; }
        return ok;
    }
    const bool enabled_;
    size_t bytes_ = 0;
    int error_ = 0, sync_error_ = 0;
    bool limited_ = false;
};
} // namespace vita
