#include "runtime_patch.hpp"

#include "log.hpp"
#include "pattern.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace mhw {
namespace {

constexpr float kWidescreenAspect = 16.0F / 9.0F;
constexpr float kUltrawideAspect = 2.37F;
constexpr std::ptrdiff_t kSecondaryResolutionOffset = 0x23D90;
constexpr std::ptrdiff_t kRenderTargetResetOffset = 0x78;

void append_i32(std::vector<std::byte>& output, std::int32_t value) {
    const auto* bytes = reinterpret_cast<const std::byte*>(&value);
    output.insert(output.end(), bytes, bytes + sizeof(value));
}

std::vector<std::byte> make_search_structure(const Config& config) {
    const auto selected_aspect =
        config.game_ultrawide_mode ? kUltrawideAspect : kWidescreenAspect;
    const auto width = static_cast<std::int32_t>(config.width);
    const auto height = static_cast<std::int32_t>(config.height);

    std::int32_t offset_x{};
    std::int32_t offset_y{};
    std::int32_t window_right{};
    std::int32_t window_bottom{};
    std::int32_t render_width{};
    std::int32_t render_height{};

    if (config.aspect() >= kWidescreenAspect) {
        render_width = static_cast<std::int32_t>(
            std::lround(static_cast<float>(height) * selected_aspect));
        render_height = height;
        offset_x = static_cast<std::int32_t>(
            std::floor((static_cast<float>(width) - render_width) / 2.0F));
        window_right = width - offset_x;
        window_bottom = height;
    } else {
        render_width = width;
        render_height = static_cast<std::int32_t>(
            std::lround(static_cast<float>(width) / selected_aspect));
        offset_y = static_cast<std::int32_t>(
            std::floor((static_cast<float>(height) - render_height) / 2.0F));
        window_right = width;
        window_bottom = height - offset_y;
    }

    std::vector<std::byte> structure;
    structure.reserve(24);
    append_i32(structure, offset_x);
    append_i32(structure, offset_y);
    append_i32(structure, window_right);
    append_i32(structure, window_bottom);
    append_i32(structure, render_width);
    append_i32(structure, render_height);
    return structure;
}

std::vector<std::byte> make_primary_replacement(const Config& config) {
    std::vector<std::byte> structure;
    structure.reserve(24);
    append_i32(structure, 0);
    append_i32(structure, 0);
    append_i32(structure, static_cast<std::int32_t>(config.width));
    append_i32(structure, static_cast<std::int32_t>(config.height));
    append_i32(structure, static_cast<std::int32_t>(config.width));
    append_i32(structure, static_cast<std::int32_t>(config.height));
    return structure;
}

void find_in_region(std::span<std::byte> region,
                    std::span<const std::byte> needle,
                    std::vector<std::byte*>& matches) {
    if (needle.empty() || region.size() < needle.size()) {
        return;
    }
    std::array<std::size_t, 256> skip{};
    skip.fill(needle.size());
    for (std::size_t index = 0; index + 1 < needle.size(); ++index) {
        skip[std::to_integer<std::uint8_t>(needle[index])] =
            needle.size() - index - 1;
    }
    std::size_t offset{};
    while (offset <= region.size() - needle.size() && matches.size() < 2) {
        const auto last =
            std::to_integer<std::uint8_t>(region[offset + needle.size() - 1]);
        if (region[offset + needle.size() - 1] == needle.back() &&
            std::memcmp(region.data() + offset, needle.data(), needle.size()) ==
                0) {
            matches.push_back(region.data() + offset);
            ++offset;
        } else {
            offset += skip[last];
        }
    }
}

std::vector<std::byte*> find_exact_writable_process(
    std::span<const std::byte> needle) {
    std::vector<std::byte*> matches;
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    auto* cursor =
        static_cast<std::byte*>(system_info.lpMinimumApplicationAddress);
    const auto* maximum =
        static_cast<const std::byte*>(system_info.lpMaximumApplicationAddress);
    while (cursor < maximum && matches.size() < 2) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(cursor, &region, sizeof(region)) != sizeof(region) ||
            region.RegionSize == 0) {
            break;
        }
        const auto protection = region.Protect & 0xFFU;
        if (region.State == MEM_COMMIT && protection == PAGE_READWRITE &&
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0) {
            find_in_region(
                std::span(static_cast<std::byte*>(region.BaseAddress),
                          region.RegionSize),
                needle, matches);
        }
        const auto next = reinterpret_cast<std::uintptr_t>(region.BaseAddress) +
                          region.RegionSize;
        if (next <= reinterpret_cast<std::uintptr_t>(cursor)) {
            break;
        }
        cursor = reinterpret_cast<std::byte*>(next);
    }
    return matches;
}

bool is_writable_range(const std::byte* address, std::size_t size) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(address, &region, sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT ||
        (region.Protect & 0xFFU) != PAGE_READWRITE ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto end = start + size;
    const auto region_start =
        reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    const auto region_end = region_start + region.RegionSize;
    return end >= start && start >= region_start && end <= region_end;
}

bool write_bytes(std::byte* address, std::span<const std::byte> bytes) {
    DWORD old_protection{};
    if (!VirtualProtect(address, bytes.size(), PAGE_READWRITE,
                        &old_protection)) {
        return false;
    }
    std::memcpy(address, bytes.data(), bytes.size());
    DWORD ignored{};
    const auto restored =
        VirtualProtect(address, bytes.size(), old_protection, &ignored);
    return restored &&
           std::memcmp(address, bytes.data(), bytes.size()) == 0;
}

}  // namespace

bool apply_runtime_resolution_patch(const Config& config, Logger& log) {
    if (config.aspect() >= kWidescreenAspect - 0.0001F) {
        log.write(L"Runtime resolution patch not needed for aspect {:.6f}",
                  config.aspect());
        return true;
    }
    if (config.game_dx12) {
        log.write(
            L"Runtime resolution patch skipped: reference algorithm does not "
            L"support DX12");
        return false;
    }
    if (config.game_dlss) {
        log.write(
            L"Runtime resolution patch skipped: reference algorithm does not "
            L"support DLSS");
        return false;
    }

    const auto search = make_search_structure(config);
    const auto replacement = make_primary_replacement(config);

    log.write(L"Waiting for runtime render structures");
    std::vector<std::byte*> matches;
    for (int attempt = 1; attempt <= 6; ++attempt) {
        Sleep(attempt == 1 ? 10000 : 5000);
        matches = find_exact_writable_process(search);
        log.write(L"Runtime structure scan {}/6: matched {}", attempt,
                  matches.size());
        if (matches.size() == 1) {
            break;
        }
        if (matches.size() > 1) {
            log.write(L"Runtime patch aborted: ambiguous matches");
            return false;
        }
    }
    if (matches.size() != 1) {
        log.write(L"Runtime patch aborted: structure not found");
        return false;
    }

    auto* primary = matches.front();
    auto* secondary = primary + kSecondaryResolutionOffset;
    auto* reset = primary + kRenderTargetResetOffset;
    if (!is_writable_range(primary, 24) ||
        !is_writable_range(secondary, 8) ||
        !is_writable_range(reset, 1)) {
        log.write(
            L"Runtime patch aborted: derived address is not in writable "
            L"memory");
        return false;
    }

    std::array<std::byte, 24> original_primary{};
    std::array<std::byte, 8> original_secondary{};
    const auto original_reset = *reset;
    std::memcpy(original_primary.data(), primary, original_primary.size());
    std::memcpy(original_secondary.data(), secondary, original_secondary.size());

    std::array<std::byte, 8> secondary_replacement{};
    const auto width = static_cast<std::int32_t>(config.width);
    const auto height = static_cast<std::int32_t>(config.height);
    std::memcpy(secondary_replacement.data(), &width, sizeof(width));
    std::memcpy(secondary_replacement.data() + sizeof(width), &height,
                sizeof(height));
    const std::array<std::byte, 1> reset_value{std::byte{1}};

    if (!write_bytes(primary, replacement) ||
        !write_bytes(secondary, secondary_replacement) ||
        !write_bytes(reset, reset_value)) {
        write_bytes(primary, original_primary);
        write_bytes(secondary, original_secondary);
        const std::array<std::byte, 1> rollback_reset{original_reset};
        write_bytes(reset, rollback_reset);
        log.write(L"Runtime patch failed; original values restored");
        return false;
    }

    log.write(
        L"Runtime resolution patch applied: {}x{} active render region",
        config.width, config.height);
    return true;
}

}  // namespace mhw
