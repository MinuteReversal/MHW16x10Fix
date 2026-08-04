#pragma once

#include "common.hpp"

namespace mhw {

struct Config {
    bool enabled{true};
    bool auto_detect_aspect{true};
    std::uint32_t width{1280};
    std::uint32_t height{800};
    bool remove_letterbox{true};
    bool enable_log{true};
    std::filesystem::path chain_load{};
    bool forward_chain_direct_input{true};
    bool resolution_detected{false};
    bool game_dx12{false};

    [[nodiscard]] float aspect() const noexcept;
    static Config load(const std::filesystem::path& path);
};

}  // namespace mhw
