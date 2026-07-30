#pragma once

#include "common.hpp"

#include <format>
#include <mutex>

namespace mhw {

class Logger {
public:
    static Logger& instance();
    ~Logger();
    void open(const std::filesystem::path& file, bool enabled);
    void write(std::wstring_view message);

    template <typename... Args>
    void write(std::wformat_string<Args...> format, Args&&... args) {
        write(std::format(format, std::forward<Args>(args)...));
    }

private:
    std::mutex mutex_;
    HANDLE file_{INVALID_HANDLE_VALUE};
    bool enabled_{false};
};

}  // namespace mhw
