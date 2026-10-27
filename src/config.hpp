#pragma once

#include "common.hpp"

namespace mhw {

struct Config {
    bool enabled{true};
    bool auto_detect_aspect{true};
    std::uint32_t width{1280};
    std::uint32_t height{800};
    bool remove_letterbox{true};
    // When letterbox removal makes the 3D view native, keep menu and HUD
    // layout on the engine's 16:9 size pair instead of the full frame.
    bool keep_ui_16x9{true};
    bool enable_log{true};
    bool trace_completion_layout{false};

    // Experimental completion-screen background overrides.
    // Zero means "leave the game's own value alone".
    // See docs/COMPLETION_SCREEN_ANALYSIS.md for the field survey.
    std::uint32_t ui_reference_width{0};
    std::uint32_t ui_reference_height{0};
    std::uint32_t secondary_layout_width{0};
    std::uint32_t secondary_layout_height{0};

    // Experimental: promote 16:9-shaped textures to the full native height at
    // the D3D12 allocation boundary. See docs/COMPLETION_SCREEN_ANALYSIS.md.
    bool promote_widescreen_textures{false};
    std::filesystem::path chain_load{};
    bool forward_chain_direct_input{true};
    bool resolution_detected{false};
    bool game_dx12{false};

    [[nodiscard]] float aspect() const noexcept;
    static Config load(const std::filesystem::path& path);
};

}  // namespace mhw
