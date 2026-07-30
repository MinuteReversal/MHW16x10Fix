#include "log.hpp"

namespace mhw {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::~Logger() {
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
    }
}

void Logger::open(const std::filesystem::path& file, bool enabled) {
    std::scoped_lock lock(mutex_);
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
    enabled_ = enabled;
    if (enabled_) {
        file_ = CreateFileW(file.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            enabled_ = false;
        }
    }
}

void Logger::write(std::wstring_view message) {
    std::scoped_lock lock(mutex_);
    if (!enabled_ || file_ == INVALID_HANDLE_VALUE) {
        return;
    }

    SYSTEMTIME time{};
    GetLocalTime(&time);
    const auto line =
        std::format(L"[{:04}-{:02}-{:02} {:02}:{:02}:{:02}] {}\r\n",
                    time.wYear, time.wMonth, time.wDay, time.wHour,
                    time.wMinute, time.wSecond, message);
    const auto required = WideCharToMultiByte(
        CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0,
        nullptr, nullptr);
    if (required <= 0) {
        return;
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.data(),
                        static_cast<int>(line.size()), utf8.data(), required,
                        nullptr, nullptr);
    DWORD written{};
    WriteFile(file_, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
              nullptr);
    FlushFileBuffers(file_);
}

}  // namespace mhw
