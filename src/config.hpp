#pragma once

#include "common.hpp"

namespace mhw {

struct Config {
    bool enabled{true};
    bool auto_detect_aspect{true};
    std::uint32_t width{1280};
    std::uint32_t height{800};
    bool fix_viewport{true};
    bool fix_projection{true};
    bool remove_letterbox{true};
    bool fix_hud{false};
    bool fix_cutscenes{false};
    bool enable_log{true};
    bool resolution_detected{false};
    bool game_ultrawide_mode{false};
    bool game_dx12{false};
    bool game_dlss{false};

    [[nodiscard]] float aspect() const noexcept;
    static Config load(const std::filesystem::path& path);
};

}  // namespace mhw
