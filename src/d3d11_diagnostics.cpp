#include "d3d11_diagnostics.hpp"

#include "config.hpp"
#include "log.hpp"
#include "pattern.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>

namespace mhw {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using CreateTexture2DFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
    const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using ResourceConstructorFn = void* (*)(
    void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uintptr_t,
    std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t,
    std::uintptr_t, std::uintptr_t, std::uintptr_t);
using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using RSSetScissorRectsFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, const D3D11_RECT*);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
    D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT);
using UpdateSubresourceFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
    const void*, UINT, UINT);
using CreateDeferredContextFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, UINT, ID3D11DeviceContext**);
using CameraParameterBuildFn = void (*)(void*, void*);

PresentFn g_original_present{};
ResizeBuffersFn g_original_resize_buffers{};
CreateTexture2DFn g_original_create_texture_2d{};
ResourceConstructorFn g_original_resource_constructor{};
RSSetViewportsFn g_original_rs_set_viewports{};
RSSetScissorRectsFn g_original_rs_set_scissor_rects{};
CreateDeferredContextFn g_original_create_deferred_context{};
CameraParameterBuildFn g_original_camera_parameter_build{};
std::atomic<bool> g_swapchain_logged{false};
std::atomic<bool> g_real_context_hook_attempted{false};
std::array<std::atomic<std::uint64_t>, 64> g_logged_viewports{};
std::array<std::atomic<std::uint64_t>, 64> g_logged_scissors{};
std::array<std::atomic<std::uint64_t>, 128> g_logged_target_viewports{};
std::array<std::atomic<std::uint64_t>, 256> g_logged_target_textures{};
std::array<std::atomic<std::uint64_t>, 256> g_logged_code_windows{};
std::array<std::atomic<std::uint64_t>, 128> g_logged_projection_candidates{};
std::atomic<bool> g_logged_runtime_regions{false};
std::atomic<bool> g_active_rect_trace_started{false};
std::atomic<bool> g_active_rect_patch_applied{false};
std::atomic<bool> g_aspect_xrefs_logged{false};
bool g_expand_720p{};
bool g_expand_scene_resources{};
bool g_patch_active_rect{};
UINT g_target_width{1280};
UINT g_target_height{800};
std::atomic<bool> g_logged_upstream_expansion{false};
std::atomic<bool> g_logged_scene_color_expansion{false};
std::atomic<bool> g_logged_scene_depth_expansion{false};
std::atomic<bool> g_logged_scene_viewport_expansion{false};
std::atomic<unsigned> g_deferred_context_count{};
std::atomic<bool> g_camera_instance_scan_complete{false};

struct CameraInstanceState {
    std::uintptr_t object{};
    std::uintptr_t vtable{};
    std::size_t size{};
    std::array<std::uint32_t, 0x17C0 / sizeof(std::uint32_t)> values{};
};

std::mutex g_camera_instance_states_mutex;
std::array<CameraInstanceState, 32> g_camera_instance_states{};
std::atomic<unsigned> g_camera_instance_log_count{};
std::atomic<bool> g_camera_singleton_probe_complete{false};
std::atomic<bool> g_native_aspect_mode_attempted{false};
std::atomic<bool> g_native_aspect_mode_result_logged{false};

std::atomic<unsigned> g_camera_parameter_log_count{};
struct CameraParameterState {
    std::uintptr_t object{};
    std::uint32_t value_2a8{};
    std::uint32_t value_2bc{};
    std::uint32_t packed_dimensions{};
};
std::mutex g_camera_parameter_states_mutex;
std::array<CameraParameterState, 128> g_camera_parameter_states{};

struct MappedConstantBuffer {
    ID3D11Resource* resource{};
    void* data{};
    UINT byte_width{};
};

thread_local MappedConstantBuffer g_mapped_constant_buffer{};

struct ContextUploadHooks {
    void** vtable{};
    MapFn map{};
    UnmapFn unmap{};
    UpdateSubresourceFn update_subresource{};
};

std::mutex g_context_upload_hooks_mutex;
std::array<ContextUploadHooks, 16> g_context_upload_hooks{};
std::size_t g_context_upload_hook_count{};

struct ResourceOrigin {
    std::uintptr_t resource{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    UINT bind_flags{};
    USHORT frame_count{};
    std::array<std::uintptr_t, 8> frames{};
};

std::mutex g_resource_origins_mutex;
std::array<ResourceOrigin, 4096> g_resource_origins{};
std::size_t g_next_resource_origin{};

void register_resource_origin(
    ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& description,
    const std::array<void*, 8>& frames, USHORT frame_count) {
    if (!texture) {
        return;
    }
    ResourceOrigin origin{};
    origin.resource = reinterpret_cast<std::uintptr_t>(texture);
    origin.format = description.Format;
    origin.bind_flags = description.BindFlags;
    origin.frame_count =
        std::min<USHORT>(frame_count,
                         static_cast<USHORT>(origin.frames.size()));
    for (USHORT index = 0; index < origin.frame_count; ++index) {
        origin.frames[index] =
            reinterpret_cast<std::uintptr_t>(frames[index]);
    }

    std::scoped_lock lock(g_resource_origins_mutex);
    for (auto& existing : g_resource_origins) {
        if (existing.resource == origin.resource) {
            existing = origin;
            return;
        }
    }
    g_resource_origins[g_next_resource_origin %
                       g_resource_origins.size()] = origin;
    ++g_next_resource_origin;
}

std::optional<ResourceOrigin> find_resource_origin(
    std::uintptr_t resource) {
    if (!resource) {
        return std::nullopt;
    }
    std::scoped_lock lock(g_resource_origins_mutex);
    for (const auto& origin : g_resource_origins) {
        if (origin.resource == resource) {
            return origin;
        }
    }
    return std::nullopt;
}

bool replace_vtable_entry(void** entry, void* replacement, void** original) {
    DWORD old_protection{};
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE,
                        &old_protection)) {
        return false;
    }
    *original = InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(entry), replacement);
    DWORD ignored{};
    return VirtualProtect(entry, sizeof(void*), old_protection, &ignored) !=
           FALSE;
}

bool mark_key_once(std::span<std::atomic<std::uint64_t>> slots,
                   std::uint64_t key) {
    key = key == 0 ? 1 : key;
    for (auto& slot : slots) {
        if (slot.load(std::memory_order_relaxed) == key) {
            return false;
        }
        std::uint64_t empty{};
        if (slot.compare_exchange_strong(empty, key,
                                         std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

std::uint64_t mix_key(std::uint64_t hash, std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
    return hash;
}

std::uint64_t viewport_key(std::uintptr_t callsite,
                           const D3D11_VIEWPORT& viewport) {
    auto hash = mix_key(1469598103934665603ULL, callsite);
    hash = mix_key(hash, std::bit_cast<std::uint32_t>(viewport.TopLeftX));
    hash = mix_key(hash, std::bit_cast<std::uint32_t>(viewport.TopLeftY));
    hash = mix_key(hash, std::bit_cast<std::uint32_t>(viewport.Width));
    hash = mix_key(hash, std::bit_cast<std::uint32_t>(viewport.Height));
    return hash;
}

std::uint64_t scissor_key(std::uintptr_t callsite, const D3D11_RECT& rect) {
    auto hash = mix_key(1469598103934665603ULL, callsite);
    hash = mix_key(hash, static_cast<std::uint32_t>(rect.left));
    hash = mix_key(hash, static_cast<std::uint32_t>(rect.top));
    hash = mix_key(hash, static_cast<std::uint32_t>(rect.right));
    hash = mix_key(hash, static_cast<std::uint32_t>(rect.bottom));
    return hash;
}

struct CallsiteDescription {
    std::wstring module{L"<unknown>"};
    std::uintptr_t offset{};
};

CallsiteDescription describe_callsite(std::uintptr_t callsite) {
    CallsiteDescription result;
    HMODULE module{};
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(callsite), &module) &&
        module) {
        std::array<wchar_t, 32768> path{};
        const auto length = GetModuleFileNameW(
            module, path.data(), static_cast<DWORD>(path.size()));
        if (length > 0 && length < path.size()) {
            result.module =
                std::filesystem::path(path.data(), path.data() + length)
                    .filename()
                    .wstring();
        }
        result.offset = callsite - reinterpret_cast<std::uintptr_t>(module);
    } else {
        result.offset = callsite;
    }
    return result;
}

void log_code_window(std::uintptr_t address) {
    if (!address || !mark_key_once(g_logged_code_windows, address)) {
        return;
    }
    constexpr std::size_t before = 96;
    constexpr std::size_t length = 128;
    const auto start = address - before;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(reinterpret_cast<const void*>(start), &memory,
                      sizeof(memory)) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return;
    }
    const auto region_end =
        reinterpret_cast<std::uintptr_t>(memory.BaseAddress) +
        memory.RegionSize;
    if (start < reinterpret_cast<std::uintptr_t>(memory.BaseAddress) ||
        start + length > region_end) {
        return;
    }

    const auto source = describe_callsite(address);
    Logger::instance().write(
        L"DX11 runtime code: center={}+0x{:X}, start_delta=-0x{:X}, "
        L"length={}",
        source.module, source.offset, before, length);
    const auto* bytes = reinterpret_cast<const unsigned char*>(start);
    for (std::size_t offset = 0; offset < length; offset += 16) {
        std::wstring hex;
        for (std::size_t index = 0; index < 16; ++index) {
            hex += std::format(L"{:02X}", bytes[offset + index]);
        }
        Logger::instance().write(L"  code[+0x{:02X}]={}", offset, hex);
    }
}

void log_code_region(std::uintptr_t address, std::size_t length) {
    MEMORY_BASIC_INFORMATION memory{};
    if (!address || !VirtualQuery(reinterpret_cast<const void*>(address),
                                  &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return;
    }
    const auto region_end =
        reinterpret_cast<std::uintptr_t>(memory.BaseAddress) +
        memory.RegionSize;
    if (address < reinterpret_cast<std::uintptr_t>(memory.BaseAddress) ||
        address + length > region_end) {
        return;
    }

    const auto source = describe_callsite(address);
    Logger::instance().write(
        L"DX11 runtime region: start={}+0x{:X}, length={}", source.module,
        source.offset, length);
    const auto* bytes = reinterpret_cast<const unsigned char*>(address);
    for (std::size_t offset = 0; offset < length; offset += 16) {
        std::wstring hex;
        const auto chunk = (std::min)(std::size_t{16}, length - offset);
        for (std::size_t index = 0; index < chunk; ++index) {
            hex += std::format(L"{:02X}", bytes[offset + index]);
        }
        Logger::instance().write(L"  region[+0x{:04X}]={}", offset, hex);
    }
}

void log_target_runtime_regions() {
    if (g_logged_runtime_regions.exchange(true)) {
        return;
    }
    const auto module =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!module) {
        return;
    }
    log_code_region(module + 0x2297000, 0xA00);
    log_code_region(module + 0x239B700, 0x1200);
    log_code_region(module + 0x2593900, 0x1800);
}

void log_runtime_aspect_xrefs() {
    if (g_aspect_xrefs_logged.exchange(true)) {
        return;
    }
    const auto image = executable_image();
    if (!image) {
        Logger::instance().write(L"Aspect xref trace: executable image missing");
        return;
    }
    constexpr std::uint32_t widescreen_bits = 0x3FE38E39U;
    std::array<const std::byte*, 16> constants{};
    std::size_t constant_count{};
    for (std::size_t offset = 0;
         offset + sizeof(widescreen_bits) <= image->size() &&
         constant_count < constants.size();
         ++offset) {
        std::uint32_t value{};
        std::memcpy(&value, image->data() + offset, sizeof(value));
        if (value == widescreen_bits) {
            constants[constant_count++] = image->data() + offset;
        }
    }
    Logger::instance().write(
        L"Aspect xref trace: runtime 16:9 constants={}", constant_count);
    for (std::size_t index = 0; index < constant_count; ++index) {
        Logger::instance().write(
            L"  aspect_constant[{}]=MonsterHunterWorld.exe+0x{:X}", index,
            constants[index] - image->data());
    }

    std::size_t xref_count{};
    for (std::size_t offset = 0; offset + 8 <= image->size() &&
                                 xref_count < 64;
         ++offset) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(
            image->data() + offset);
        std::size_t displacement_offset{};
        std::size_t instruction_size{};
        if (bytes[0] == 0xF3 && bytes[1] == 0x0F &&
            (bytes[2] == 0x10 || bytes[2] == 0x59 ||
             bytes[2] == 0x5D || bytes[2] == 0x5E ||
             bytes[2] == 0x5F) &&
            (bytes[3] & 0xC7U) == 0x05U) {
            displacement_offset = 4;
            instruction_size = 8;
        } else if (bytes[0] == 0x0F &&
                   (bytes[1] == 0x2E || bytes[1] == 0x2F) &&
                   (bytes[2] & 0xC7U) == 0x05U) {
            displacement_offset = 3;
            instruction_size = 7;
        } else {
            continue;
        }
        std::int32_t displacement{};
        std::memcpy(&displacement, bytes + displacement_offset,
                    sizeof(displacement));
        const auto target_offset =
            static_cast<std::ptrdiff_t>(offset + instruction_size) +
            displacement;
        if (target_offset < 0 ||
            static_cast<std::size_t>(target_offset) + sizeof(std::uint32_t) >
                image->size()) {
            continue;
        }
        std::uint32_t target_value{};
        std::memcpy(&target_value, image->data() + target_offset,
                    sizeof(target_value));
        if (target_value != widescreen_bits) {
            continue;
        }
        Logger::instance().write(
            L"  aspect_xref[{}]=MonsterHunterWorld.exe+0x{:X} -> +0x{:X}",
            xref_count, offset, target_offset);
        log_code_window(
            reinterpret_cast<std::uintptr_t>(image->data() + offset));
        ++xref_count;
    }
    Logger::instance().write(L"Aspect xref trace completed: xrefs={}",
                             xref_count);
}

void scan_active_render_rectangle(unsigned attempt) {
    const auto active_height =
        static_cast<std::int32_t>((g_target_width * 9U) / 16U);
    const auto top = static_cast<std::int32_t>(
        (g_target_height - active_height) / 2U);
    const std::array<std::int32_t, 6> needle{
        0, top, static_cast<std::int32_t>(g_target_width),
        top + active_height, static_cast<std::int32_t>(g_target_width),
        active_height};

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    auto* cursor = static_cast<std::uint8_t*>(
        system_info.lpMinimumApplicationAddress);
    const auto* maximum = static_cast<const std::uint8_t*>(
        system_info.lpMaximumApplicationAddress);
    std::array<const std::uint8_t*, 32> matches{};
    std::size_t match_count{};
    std::size_t writable_regions{};
    std::uintptr_t last_allocation{};

    // This engine allocation has placed the live rectangle at +0xFFE00
    // consistently across repeated Windows and Steam Deck captures. Check
    // that exact location first so the diagnostic can run before resource
    // initialization without scanning every writable byte.
    while (cursor < maximum && match_count < matches.size()) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(cursor, &region, sizeof(region)) != sizeof(region) ||
            region.RegionSize == 0) {
            break;
        }
        const auto allocation =
            reinterpret_cast<std::uintptr_t>(region.AllocationBase);
        if (allocation && allocation != last_allocation) {
            last_allocation = allocation;
            const auto* candidate = reinterpret_cast<const std::uint8_t*>(
                allocation + 0xFFE00);
            MEMORY_BASIC_INFORMATION candidate_region{};
            if (VirtualQuery(candidate, &candidate_region,
                             sizeof(candidate_region)) ==
                    sizeof(candidate_region) &&
                candidate_region.State == MEM_COMMIT &&
                (candidate_region.Protect &
                 (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
                std::memcmp(candidate, needle.data(), sizeof(needle)) == 0) {
                matches[match_count++] = candidate;
            }
        }
        const auto next =
            reinterpret_cast<std::uintptr_t>(region.BaseAddress) +
            region.RegionSize;
        if (next <= reinterpret_cast<std::uintptr_t>(cursor)) {
            break;
        }
        cursor = reinterpret_cast<std::uint8_t*>(next);
    }

    if (match_count == 0) {
        cursor = static_cast<std::uint8_t*>(
            system_info.lpMinimumApplicationAddress);
        while (cursor < maximum && match_count < matches.size()) {
            MEMORY_BASIC_INFORMATION region{};
            if (VirtualQuery(cursor, &region, sizeof(region)) !=
                    sizeof(region) ||
                region.RegionSize == 0) {
                break;
            }
            const auto protection = region.Protect & 0xFFU;
            const auto writable =
                protection == PAGE_READWRITE ||
                protection == PAGE_WRITECOPY ||
                protection == PAGE_EXECUTE_READWRITE ||
                protection == PAGE_EXECUTE_WRITECOPY;
            if (region.State == MEM_COMMIT && writable &&
                (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
                region.RegionSize >= sizeof(needle)) {
                ++writable_regions;
                const auto* begin =
                    static_cast<const std::uint8_t*>(region.BaseAddress);
                const auto limit = region.RegionSize - sizeof(needle);
                for (std::size_t offset = 0;
                     offset <= limit && match_count < matches.size();
                     offset += alignof(std::int32_t)) {
                    if (std::memcmp(begin + offset, needle.data(),
                                    sizeof(needle)) == 0) {
                        matches[match_count++] = begin + offset;
                    }
                }
            }
            const auto next =
                reinterpret_cast<std::uintptr_t>(region.BaseAddress) +
                region.RegionSize;
            if (next <= reinterpret_cast<std::uintptr_t>(cursor)) {
                break;
            }
            cursor = reinterpret_cast<std::uint8_t*>(next);
        }
    }

    Logger::instance().write(
        L"Active-rect scan {}: pattern=[0,{},1280,{},1280,{}], "
        L"writable_regions={}, matches={}",
        attempt, top, top + active_height, active_height, writable_regions,
        match_count);
    for (std::size_t index = 0; index < match_count; ++index) {
        const auto address =
            reinterpret_cast<std::uintptr_t>(matches[index]);
        MEMORY_BASIC_INFORMATION region{};
        VirtualQuery(matches[index], &region, sizeof(region));
        const auto allocation =
            reinterpret_cast<std::uintptr_t>(region.AllocationBase);
        Logger::instance().write(
            L"  active_rect[{}]=0x{:X}, allocation=0x{:X}+0x{:X}, "
            L"protect=0x{:X}",
            index, address, allocation, address - allocation, region.Protect);
        const auto region_start =
            reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const auto region_end = region_start + region.RegionSize;
        if (address >= region_start + 4 * sizeof(std::int32_t) &&
            address + 12 * sizeof(std::int32_t) <= region_end) {
            const auto* values =
                reinterpret_cast<const std::int32_t*>(matches[index]);
            std::wstring nearby;
            for (int value_index = -4; value_index < 12; ++value_index) {
                nearby += std::format(
                    L"{}{}", value_index == -4 ? L"" : L",",
                    values[value_index]);
            }
            Logger::instance().write(L"    nearby_i32[-4..11]={}", nearby);
        }
    }
    if (g_patch_active_rect && match_count == 1 &&
        !g_active_rect_patch_applied.load()) {
        const std::array<std::int32_t, 6> replacement{
            0, 0, static_cast<std::int32_t>(g_target_width),
            static_cast<std::int32_t>(g_target_height),
            static_cast<std::int32_t>(g_target_width),
            static_cast<std::int32_t>(g_target_height)};
        auto* destination =
            const_cast<std::uint8_t*>(matches.front());
        if (std::memcmp(destination, needle.data(), sizeof(needle)) == 0) {
            g_active_rect_patch_applied.store(true);
            std::memcpy(destination, replacement.data(), sizeof(replacement));
            Logger::instance().write(
                L"Active-rect-only patch applied: [0,{},1280,{},1280,{}] "
                L"-> [0,0,{}, {},{},{}]",
                top, top + active_height, active_height, g_target_width,
                g_target_height, g_target_width, g_target_height);
        } else {
            Logger::instance().write(
                L"Active-rect-only patch aborted: value changed before write");
        }
    }
}

DWORD WINAPI active_rect_trace_thread(void*) {
    constexpr std::array<DWORD, 3> delays{0, 10000, 15000};
    for (unsigned index = 0; index < delays.size(); ++index) {
        Sleep(delays[index]);
        scan_active_render_rectangle(index + 1);
    }
    return 0;
}

void start_active_rect_trace() {
    if (g_active_rect_trace_started.exchange(true)) {
        return;
    }
    const auto thread =
        CreateThread(nullptr, 0, active_rect_trace_thread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
        Logger::instance().write(
            L"Active-rect trace scheduled immediately, +10s, +25s");
    } else {
        Logger::instance().write(L"Active-rect trace thread creation failed");
    }
}

bool patch_render_aspect_constant(const Config& config, Logger& log) {
    if (!config.experimental_patch_aspect_constant) {
        log.write(L"Experimental render aspect constant patch: disabled");
        return true;
    }
    if (config.width != 1280 || config.height != 800) {
        log.write(
            L"Render aspect constant patch aborted: target is not 1280x800");
        return false;
    }
    constexpr std::uintptr_t constant_rva = 0x2F74AD4;
    constexpr std::uintptr_t fallback_16_9_rva = 0x229990E;
    // Keep every consumer except the final rectangle-fitting path
    // (+2304967) on untouched 16:9. The main scene resources and viewport
    // are expanded independently to 1280x800; only the final rectangle uses
    // patched 16:10 so the full-height source can reach the swap chain.
    constexpr std::array<std::uintptr_t, 7> ui_xrefs{
        0x1FDA863, 0x22993DA, 0x229943A, 0x229A903,
        0x229EF70, 0x229F0C4, 0x242368E};
    constexpr std::uint32_t expected = 0x3FE38E39U;
    const auto module =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* address =
        reinterpret_cast<std::uint32_t*>(module + constant_rva);
    if (!address || *address != expected) {
        log.write(
            L"Render aspect constant patch aborted: value mismatch at "
            L"MonsterHunterWorld.exe+0x{:X}",
            constant_rva);
        return false;
    }
    const auto* fallback =
        reinterpret_cast<const std::uint32_t*>(module + fallback_16_9_rva);
    if (*fallback != expected) {
        log.write(
            L"Selective aspect patch aborted: fallback 16:9 constant "
            L"mismatch at +0x{:X}",
            fallback_16_9_rva);
        return false;
    }
    for (const auto xref_rva : ui_xrefs) {
        const auto* instruction =
            reinterpret_cast<const std::uint8_t*>(module + xref_rva);
        if (instruction[0] != 0xF3 || instruction[1] != 0x0F ||
            instruction[2] != 0x10 ||
            (instruction[3] & 0xC7U) != 0x05U) {
            log.write(
                L"Selective aspect patch aborted: instruction mismatch "
                L"at +0x{:X}",
                xref_rva);
            return false;
        }
        std::int32_t old_displacement{};
        std::memcpy(&old_displacement, instruction + 4,
                    sizeof(old_displacement));
        const auto old_target =
            module + xref_rva + 8 + old_displacement;
        if (old_target != module + constant_rva) {
            log.write(
                L"Selective aspect patch aborted: xref +0x{:X} targets "
                L"+0x{:X}, expected +0x{:X}",
                xref_rva, old_target - module, constant_rva);
            return false;
        }
    }
    for (const auto xref_rva : ui_xrefs) {
        auto* displacement = reinterpret_cast<std::int32_t*>(
            module + xref_rva + 4);
        const auto replacement_displacement =
            static_cast<std::int32_t>(
                fallback_16_9_rva - (xref_rva + 8));
        DWORD code_protection{};
        if (!VirtualProtect(displacement, sizeof(*displacement),
                            PAGE_EXECUTE_READWRITE, &code_protection)) {
            log.write(
                L"Selective aspect patch failed: protection at +0x{:X}",
                xref_rva);
            return false;
        }
        *displacement = replacement_displacement;
        DWORD ignored{};
        VirtualProtect(displacement, sizeof(*displacement),
                       code_protection, &ignored);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

    const auto aspect = config.aspect();
    std::uint32_t replacement{};
    std::memcpy(&replacement, &aspect, sizeof(replacement));
    DWORD old_protection{};
    if (!VirtualProtect(address, sizeof(*address), PAGE_READWRITE,
                        &old_protection)) {
        log.write(L"Render aspect constant patch aborted: protection failed");
        return false;
    }
    *address = replacement;
    DWORD ignored{};
    const auto restored =
        VirtualProtect(address, sizeof(*address), old_protection, &ignored);
    if (!restored || *address != replacement) {
        log.write(
            L"Render aspect constant patch failed verification at +0x{:X}",
            constant_rva);
        return false;
    }
    log.write(
        L"Selective aspect patch applied: final shared constant={:.6f}; "
        L"{} xrefs redirected to 1.777778",
        aspect, ui_xrefs.size());
    return true;
}

void camera_parameter_build_hook(void* object, void* render_context) {
    if (object && render_context &&
        g_camera_parameter_log_count.load(std::memory_order_relaxed) < 256) {
        const auto base = reinterpret_cast<std::uintptr_t>(object);
        const auto context = reinterpret_cast<std::uintptr_t>(render_context);
        const auto value_2a8 =
            *reinterpret_cast<const std::uint32_t*>(base + 0x2A8);
        const auto value_2bc =
            *reinterpret_cast<const std::uint32_t*>(base + 0x2BC);
        std::uint32_t packed_dimensions{};
        const auto dimension_owner =
            *reinterpret_cast<const std::uintptr_t*>(context + 0x10);
        if (dimension_owner) {
            packed_dimensions =
                *reinterpret_cast<const std::uint32_t*>(
                    dimension_owner + 0x68);
        }

        bool changed{};
        {
            std::scoped_lock lock(g_camera_parameter_states_mutex);
            CameraParameterState* empty{};
            CameraParameterState* state{};
            for (auto& candidate : g_camera_parameter_states) {
                if (candidate.object == base) {
                    state = &candidate;
                    break;
                }
                if (!empty && candidate.object == 0) {
                    empty = &candidate;
                }
            }
            if (!state) {
                state = empty;
            }
            if (state &&
                (state->object != base ||
                 state->value_2a8 != value_2a8 ||
                 state->value_2bc != value_2bc ||
                 state->packed_dimensions != packed_dimensions)) {
                *state = {base, value_2a8, value_2bc,
                          packed_dimensions};
                changed = true;
            }
        }

        if (changed &&
            g_camera_parameter_log_count.fetch_add(
                1, std::memory_order_relaxed) < 256) {
            const auto read_float = [base](std::uintptr_t offset) {
                return *reinterpret_cast<const float*>(base + offset);
            };
            const auto width = packed_dimensions & 0xFFFFU;
            const auto height = (packed_dimensions >> 16U) & 0x7FFFU;
            std::array<void*, 8> frames{};
            const auto frame_count = RtlCaptureStackBackTrace(
                1, static_cast<DWORD>(frames.size()), frames.data(),
                nullptr);
            Logger::instance().write(
                L"Engine camera-parameter candidate: object={:p}, "
                L"dims={}x{}, +2A8={:.7f}, +2AC={:.7f}, "
                L"+2B0={:.7f}, +2B4={:.7f}, +2BC={:.7f}, "
                L"+39C={:.7f}, stack_frames={}",
                object, width, height, read_float(0x2A8),
                read_float(0x2AC), read_float(0x2B0),
                read_float(0x2B4), read_float(0x2BC),
                read_float(0x39C), frame_count);
            for (USHORT index = 0; index < frame_count; ++index) {
                const auto source = describe_callsite(
                    reinterpret_cast<std::uintptr_t>(frames[index]));
                Logger::instance().write(
                    L"  camera_parameter_stack[{}]={}+0x{:X}", index,
                    source.module, source.offset);
            }
        }
    }
    g_original_camera_parameter_build(object, render_context);
}

bool install_camera_parameter_hook(Logger& log) {
    constexpr std::uintptr_t function_rva = 0x1FDA390;
    constexpr std::size_t overwritten_size = 19;
    constexpr std::array<std::uint8_t, overwritten_size> expected{
        0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x41, 0x54, 0x41, 0x55,
        0x41, 0x56, 0x48, 0x8D, 0xA8, 0xF8, 0xFE, 0xFF, 0xFF};
    auto* target = reinterpret_cast<std::uint8_t*>(
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) +
        function_rva);
    if (!target ||
        std::memcmp(target, expected.data(), expected.size()) != 0) {
        log.write(
            L"Engine camera-parameter hook aborted: target bytes mismatch");
        return false;
    }

    constexpr std::size_t absolute_jump_size = 14;
    auto* trampoline = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, overwritten_size + absolute_jump_size,
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!trampoline) {
        log.write(
            L"Engine camera-parameter hook aborted: allocation failed");
        return false;
    }
    std::memcpy(trampoline, target, overwritten_size);
    const auto write_absolute_jump = [](std::uint8_t* destination,
                                        const void* jump_target) {
        destination[0] = 0xFF;
        destination[1] = 0x25;
        std::memset(destination + 2, 0, 4);
        std::memcpy(destination + 6, &jump_target, sizeof(jump_target));
    };
    write_absolute_jump(trampoline + overwritten_size,
                        target + overwritten_size);
    DWORD ignored{};
    if (!VirtualProtect(trampoline,
                        overwritten_size + absolute_jump_size,
                        PAGE_EXECUTE_READ, &ignored)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        log.write(
            L"Engine camera-parameter hook aborted: trampoline protection "
            L"failed");
        return false;
    }
    g_original_camera_parameter_build =
        reinterpret_cast<CameraParameterBuildFn>(trampoline);

    DWORD old_protection{};
    if (!VirtualProtect(target, overwritten_size,
                        PAGE_EXECUTE_READWRITE, &old_protection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        log.write(
            L"Engine camera-parameter hook aborted: target protection "
            L"failed");
        return false;
    }
    write_absolute_jump(
        target,
        reinterpret_cast<const void*>(&camera_parameter_build_hook));
    std::memset(target + absolute_jump_size, 0x90,
                overwritten_size - absolute_jump_size);
    FlushInstructionCache(GetCurrentProcess(), target, overwritten_size);
    const auto restored = VirtualProtect(
        target, overwritten_size, old_protection, &ignored);
    log.write(
        L"Engine camera-parameter hook installed at "
        L"MonsterHunterWorld.exe+0x{:X}, protection_restored={}",
        function_rva, restored != FALSE);
    return true;
}

void scan_camera_instances() {
    if (g_camera_instance_scan_complete.exchange(true)) {
        return;
    }
    const auto module =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    constexpr std::uintptr_t camera_vtable_rva = 0x2E4F5A0;
    constexpr std::uintptr_t mh_camera_vtable_rva = 0x2E09AB9;
    const std::array targets{
        std::pair{module + camera_vtable_rva, std::size_t{0x1A0}},
        std::pair{module + mh_camera_vtable_rva, std::size_t{0x17C0}}};
    unsigned found{};
    MEMORY_BASIC_INFORMATION region{};
    std::uintptr_t cursor{};
    while (VirtualQuery(reinterpret_cast<const void*>(cursor), &region,
                        sizeof(region)) == sizeof(region)) {
        const auto base =
            reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const auto next = base + region.RegionSize;
        const auto readable =
            region.State == MEM_COMMIT &&
            !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (region.Protect & (PAGE_READONLY | PAGE_READWRITE |
                               PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE |
                               PAGE_EXECUTE_WRITECOPY));
        if (readable && region.Type == MEM_PRIVATE &&
            region.RegionSize <= 256ULL * 1024ULL * 1024ULL) {
            std::vector<std::uint8_t> copy(region.RegionSize);
            SIZE_T copied{};
            if (ReadProcessMemory(GetCurrentProcess(), region.BaseAddress,
                                  copy.data(), copy.size(), &copied)) {
                for (const auto& [vtable, object_size] : targets) {
                    for (std::size_t offset{};
                         offset + sizeof(vtable) <= copied;
                         offset += alignof(void*)) {
                        std::uintptr_t value{};
                        std::memcpy(&value, copy.data() + offset,
                                    sizeof(value));
                        if (value != vtable) {
                            continue;
                        }
                        std::scoped_lock lock(
                            g_camera_instance_states_mutex);
                        auto state = std::find_if(
                            g_camera_instance_states.begin(),
                            g_camera_instance_states.end(),
                            [](const auto& item) {
                                return item.object == 0;
                            });
                        if (state == g_camera_instance_states.end()) {
                            continue;
                        }
                        state->object = base + offset;
                        state->vtable = vtable;
                        state->size = object_size;
                        SIZE_T object_bytes{};
                        ReadProcessMemory(
                            GetCurrentProcess(),
                            reinterpret_cast<const void*>(state->object),
                            state->values.data(), object_size,
                            &object_bytes);
                        ++found;
                        Logger::instance().write(
                            L"Engine camera instance: object=0x{:X}, "
                            L"type={}, size=0x{:X}",
                            state->object,
                            vtable == module + camera_vtable_rva
                                ? L"uCamera"
                                : L"uMhCamera",
                            object_size);
                    }
                }
            }
        }
        if (next <= cursor) {
            break;
        }
        cursor = next;
    }
    Logger::instance().write(
        L"Engine camera instance scan complete: found={}", found);
}

void trace_camera_instance_changes() {
    std::scoped_lock lock(g_camera_instance_states_mutex);
    for (auto& state : g_camera_instance_states) {
        if (!state.object ||
            g_camera_instance_log_count.load(std::memory_order_relaxed) >=
                512) {
            continue;
        }
        std::array<std::uint32_t, 0x17C0 / sizeof(std::uint32_t)> current{};
        SIZE_T copied{};
        if (!ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<const void*>(state.object), current.data(),
                state.size, &copied) ||
            copied != state.size) {
            continue;
        }
        for (std::size_t index = 2; index < state.size / 4; ++index) {
            if (current[index] == state.values[index]) {
                continue;
            }
            const auto old_value = std::bit_cast<float>(state.values[index]);
            const auto new_value = std::bit_cast<float>(current[index]);
            if (std::isfinite(old_value) && std::isfinite(new_value) &&
                std::abs(old_value) < 100000.0F &&
                std::abs(new_value) < 100000.0F &&
                g_camera_instance_log_count.fetch_add(
                    1, std::memory_order_relaxed) < 512) {
                Logger::instance().write(
                    L"Engine camera field changed: object=0x{:X}, "
                    L"offset=0x{:X}, old={:.7f}, new={:.7f}",
                    state.object, index * 4, old_value, new_value);
            }
            state.values[index] = current[index];
        }
    }
}

void probe_camera_singleton_references() {
    if (g_camera_singleton_probe_complete.exchange(true)) {
        return;
    }
    const auto module =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    constexpr std::uintptr_t descriptor_rva = 0x500E3F8;
    const auto descriptor = module + descriptor_rva;

    std::array<std::uintptr_t, 32> descriptor_words{};
    SIZE_T descriptor_bytes{};
    if (ReadProcessMemory(GetCurrentProcess(),
                          reinterpret_cast<const void*>(descriptor),
                          descriptor_words.data(),
                          sizeof(descriptor_words), &descriptor_bytes)) {
        for (std::size_t index{}; index < descriptor_bytes / 8; ++index) {
            Logger::instance().write(
                L"sMhCamera descriptor: +0x{:X}=0x{:X}", index * 8,
                descriptor_words[index]);
        }
    }

    unsigned found{};
    MEMORY_BASIC_INFORMATION region{};
    std::uintptr_t cursor{};
    while (VirtualQuery(reinterpret_cast<const void*>(cursor), &region,
                        sizeof(region)) == sizeof(region)) {
        const auto base =
            reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const auto next = base + region.RegionSize;
        const auto readable =
            region.State == MEM_COMMIT &&
            !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (region.Protect & (PAGE_READONLY | PAGE_READWRITE |
                               PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE |
                               PAGE_EXECUTE_WRITECOPY));
        if (readable && region.Type == MEM_PRIVATE &&
            region.RegionSize <= 256ULL * 1024ULL * 1024ULL) {
            std::vector<std::uint8_t> copy(region.RegionSize);
            SIZE_T copied{};
            if (ReadProcessMemory(GetCurrentProcess(), region.BaseAddress,
                                  copy.data(), copy.size(), &copied)) {
                for (std::size_t offset{};
                     offset + sizeof(descriptor) <= copied;
                     offset += alignof(void*)) {
                    std::uintptr_t value{};
                    std::memcpy(&value, copy.data() + offset,
                                sizeof(value));
                    if (value != descriptor || found >= 128) {
                        continue;
                    }
                    const auto reference = base + offset;
                    std::array<std::uintptr_t, 12> surrounding{};
                    SIZE_T surrounding_bytes{};
                    const auto start =
                        reference >= 4 * sizeof(std::uintptr_t)
                            ? reference - 4 * sizeof(std::uintptr_t)
                            : reference;
                    ReadProcessMemory(
                        GetCurrentProcess(),
                        reinterpret_cast<const void*>(start),
                        surrounding.data(), sizeof(surrounding),
                        &surrounding_bytes);
                    Logger::instance().write(
                        L"sMhCamera descriptor reference: address=0x{:X}, "
                        L"region=0x{:X}, offset=0x{:X}",
                        reference, base, reference - base);
                    for (std::size_t index{};
                         index < surrounding_bytes / 8; ++index) {
                        Logger::instance().write(
                            L"  sMhCamera reference word[{:+}]=0x{:X}",
                            static_cast<int>(index) - 4,
                            surrounding[index]);
                    }
                    ++found;
                }
            }
        }
        if (next <= cursor) {
            break;
        }
        cursor = next;
    }
    Logger::instance().write(
        L"sMhCamera singleton reference scan complete: found={}", found);
}

void apply_native_aspect_mode_zero() {
    if (g_native_aspect_mode_attempted.exchange(true)) {
        return;
    }
    const auto module =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    constexpr std::uintptr_t render_manager_global_rva = 0x51C4480;
    constexpr std::uintptr_t aspect_mode_setter_rva = 0x229C790;
    const auto manager =
        *reinterpret_cast<void**>(module + render_manager_global_rva);
    if (!manager) {
        Logger::instance().write(
            L"Native aspect mode 0 aborted: render manager unavailable");
        return;
    }
    const auto manager_address =
        reinterpret_cast<std::uintptr_t>(manager);
    const auto active_before =
        *reinterpret_cast<const std::int32_t*>(manager_address + 0x7B43C);
    const auto requested_before =
        *reinterpret_cast<const std::int32_t*>(manager_address + 0x7B440);
    const auto width_before =
        *reinterpret_cast<const std::uint32_t*>(manager_address + 0x198);
    const auto height_before =
        *reinterpret_cast<const std::uint32_t*>(manager_address + 0x19C);
    using SetAspectModeFn = void (*)(void*, std::int32_t);
    const auto setter =
        reinterpret_cast<SetAspectModeFn>(module + aspect_mode_setter_rva);
    setter(manager, 0);
    const auto requested_after =
        *reinterpret_cast<const std::int32_t*>(manager_address + 0x7B440);
    Logger::instance().write(
        L"Native aspect mode requested: manager={:p}, active {} -> "
        L"pending, requested {} -> {}, content={}x{}, setter=+0x{:X}",
        manager, active_before, requested_before, requested_after,
        width_before, height_before, aspect_mode_setter_rva);
}

void log_native_aspect_mode_result() {
    if (!g_native_aspect_mode_attempted.load() ||
        g_native_aspect_mode_result_logged.load()) {
        return;
    }
    const auto module =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto manager =
        *reinterpret_cast<void**>(module + 0x51C4480);
    if (!manager) {
        return;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(manager);
    const auto active =
        *reinterpret_cast<const std::int32_t*>(address + 0x7B43C);
    const auto requested =
        *reinterpret_cast<const std::int32_t*>(address + 0x7B440);
    if (active != requested) {
        return;
    }
    if (!g_native_aspect_mode_result_logged.exchange(true)) {
        const auto width =
            *reinterpret_cast<const std::uint32_t*>(address + 0x198);
        const auto height =
            *reinterpret_cast<const std::uint32_t*>(address + 0x19C);
        const auto output =
            *reinterpret_cast<const std::uint64_t*>(address + 0x1F448);
        Logger::instance().write(
            L"Native aspect mode applied: active={}, requested={}, "
            L"content={}x{}, output={}x{}",
            active, requested, width, height,
            static_cast<std::uint32_t>(output),
            static_cast<std::uint32_t>(output >> 32));
    }
}

void* resource_constructor_hook(
    void* self, std::uint32_t width, std::uint32_t height,
    std::uint32_t argument4, std::uintptr_t argument5,
    std::uintptr_t argument6, std::uintptr_t argument7,
    std::uintptr_t argument8, std::uintptr_t argument9,
    std::uintptr_t argument10, std::uintptr_t argument11,
    std::uintptr_t argument12) {
    if (g_expand_720p && width == 1280 && height == 720) {
        if (!g_logged_upstream_expansion.exchange(true)) {
            Logger::instance().write(
                L"DX11 resource resolution: {}x{} -> {}x{}", width,
                height, g_target_width, g_target_height);
        }
        width = g_target_width;
        height = g_target_height;
    }
    return g_original_resource_constructor(
        self, width, height, argument4, argument5, argument6, argument7,
        argument8, argument9, argument10, argument11, argument12);
}

bool install_resource_constructor_hook(Logger& log) {
    if (!g_expand_720p) {
        return true;
    }

    constexpr std::uintptr_t function_rva = 0x239B7B0;
    constexpr std::size_t overwritten_size = 20;
    constexpr std::array<std::uint8_t, overwritten_size> expected{
        0x48, 0x89, 0x6C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
        0x48, 0x89, 0x7C, 0x24, 0x18, 0x4C, 0x89, 0x74, 0x24, 0x20};
    auto* target = reinterpret_cast<std::uint8_t*>(
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) +
        function_rva);
    if (!target ||
        std::memcmp(target, expected.data(), expected.size()) != 0) {
        log.write(L"Upstream resolution hook aborted: target bytes mismatch");
        return false;
    }

    constexpr std::size_t absolute_jump_size = 14;
    auto* trampoline = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, overwritten_size + absolute_jump_size,
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!trampoline) {
        log.write(L"Upstream resolution hook aborted: trampoline allocation "
                  L"failed");
        return false;
    }
    std::memcpy(trampoline, target, overwritten_size);
    auto write_absolute_jump = [](std::uint8_t* destination,
                                  const void* jump_target) {
        destination[0] = 0xFF;
        destination[1] = 0x25;
        std::memset(destination + 2, 0, 4);
        std::memcpy(destination + 6, &jump_target, sizeof(jump_target));
    };
    write_absolute_jump(trampoline + overwritten_size,
                        target + overwritten_size);
    DWORD ignored{};
    if (!VirtualProtect(trampoline, overwritten_size + absolute_jump_size,
                        PAGE_EXECUTE_READ, &ignored)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        log.write(L"Upstream resolution hook aborted: trampoline protection "
                  L"failed");
        return false;
    }
    g_original_resource_constructor =
        reinterpret_cast<ResourceConstructorFn>(trampoline);

    DWORD old_protection{};
    if (!VirtualProtect(target, overwritten_size, PAGE_EXECUTE_READWRITE,
                        &old_protection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        log.write(L"Upstream resolution hook aborted: target protection "
                  L"failed");
        return false;
    }
    write_absolute_jump(
        target, reinterpret_cast<const void*>(&resource_constructor_hook));
    std::memset(target + absolute_jump_size, 0x90,
                overwritten_size - absolute_jump_size);
    FlushInstructionCache(GetCurrentProcess(), target, overwritten_size);
    const auto restored =
        VirtualProtect(target, overwritten_size, old_protection, &ignored);
    if (!restored) {
        log.write(L"Upstream resolution hook warning: protection restore "
                  L"failed");
    }
    log.write(L"Resource-constructor resolution hook installed at "
              L"MonsterHunterWorld.exe+0x{:X}",
              function_rva);
    return true;
}

bool is_target_viewport(const D3D11_VIEWPORT& viewport) {
    return std::fabs(viewport.Width - 1280.0F) < 0.1F &&
           (std::fabs(viewport.Height - 720.0F) < 0.1F ||
            std::fabs(viewport.Height - 800.0F) < 0.1F);
}

struct TextureDescription {
    bool available{};
    std::uintptr_t resource{};
    UINT width{};
    UINT height{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    UINT samples{};
};

TextureDescription describe_view(ID3D11View* view) {
    TextureDescription result;
    if (!view) {
        return result;
    }
    ID3D11Resource* resource{};
    view->GetResource(&resource);
    if (!resource) {
        return result;
    }
    ID3D11Texture2D* texture{};
    if (SUCCEEDED(resource->QueryInterface(
            __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&texture))) &&
        texture) {
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        result = {true, reinterpret_cast<std::uintptr_t>(texture),
                  description.Width, description.Height,
                  description.Format, description.SampleDesc.Count};
        texture->Release();
    }
    resource->Release();
    return result;
}

void log_target_viewport(ID3D11DeviceContext* context,
                         const D3D11_VIEWPORT& viewport) {
    std::array<void*, 8> frames{};
    const auto frame_count = RtlCaptureStackBackTrace(
        1, static_cast<DWORD>(frames.size()), frames.data(), nullptr);

    ID3D11RenderTargetView* render_target{};
    ID3D11DepthStencilView* depth_target{};
    context->OMGetRenderTargets(1, &render_target, &depth_target);
    const auto color = describe_view(render_target);
    const auto depth = describe_view(depth_target);
    if (render_target) {
        render_target->Release();
    }
    if (depth_target) {
        depth_target->Release();
    }

    auto key = viewport_key(
        frame_count > 0
            ? reinterpret_cast<std::uintptr_t>(frames[0])
            : 0,
        viewport);
    key = mix_key(key, color.width);
    key = mix_key(key, color.height);
    key = mix_key(key, color.resource);
    key = mix_key(key, depth.width);
    key = mix_key(key, depth.height);
    key = mix_key(key, depth.resource);
    for (USHORT index = 1; index < frame_count; ++index) {
        key = mix_key(key, reinterpret_cast<std::uintptr_t>(frames[index]));
    }
    if (!mark_key_once(g_logged_target_viewports, key)) {
        return;
    }

    Logger::instance().write(
        L"DX11 target viewport detail: w={:.1f}, h={:.1f}, "
        L"RTV={}x{} format={} samples={}, DSV={}x{} format={} samples={}, "
        L"stack_frames={}",
        viewport.Width, viewport.Height, color.width, color.height,
        static_cast<unsigned>(color.format), color.samples, depth.width,
        depth.height, static_cast<unsigned>(depth.format), depth.samples,
        frame_count);
    for (USHORT index = 0; index < frame_count; ++index) {
        const auto source = describe_callsite(
            reinterpret_cast<std::uintptr_t>(frames[index]));
        Logger::instance().write(L"  stack[{}]={}+0x{:X}", index,
                                 source.module, source.offset);
    }
    const auto log_origin = [](const wchar_t* label,
                               const TextureDescription& texture) {
        const auto origin = find_resource_origin(texture.resource);
        if (!origin) {
            Logger::instance().write(
                L"  {} resource={:p}, origin=<untracked>", label,
                reinterpret_cast<void*>(texture.resource));
            return;
        }
        Logger::instance().write(
            L"  {} resource={:p}, creator format={}, bind=0x{:X}, "
            L"creator_stack_frames={}",
            label, reinterpret_cast<void*>(texture.resource),
            static_cast<unsigned>(origin->format), origin->bind_flags,
            origin->frame_count);
        for (USHORT index = 0; index < origin->frame_count; ++index) {
            const auto source = describe_callsite(origin->frames[index]);
            Logger::instance().write(
                L"    {}_creator_stack[{}]={}+0x{:X}", label, index,
                source.module, source.offset);
        }
    };
    if (color.available) {
        log_origin(L"RTV", color);
    }
    if (depth.available) {
        log_origin(L"DSV", depth);
    }
}

void STDMETHODCALLTYPE rs_set_viewports_hook(
    ID3D11DeviceContext* context, UINT count,
    const D3D11_VIEWPORT* viewports);
void STDMETHODCALLTYPE rs_set_scissor_rects_hook(
    ID3D11DeviceContext* context, UINT count, const D3D11_RECT* rects);
HRESULT STDMETHODCALLTYPE map_hook(
    ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource,
    D3D11_MAP map_type, UINT map_flags,
    D3D11_MAPPED_SUBRESOURCE* mapped);
void STDMETHODCALLTYPE unmap_hook(ID3D11DeviceContext* context,
                                  ID3D11Resource* resource,
                                  UINT subresource);
void STDMETHODCALLTYPE update_subresource_hook(
    ID3D11DeviceContext* context, ID3D11Resource* resource,
    UINT destination_subresource, const D3D11_BOX* destination_box,
    const void* source_data, UINT source_row_pitch,
    UINT source_depth_pitch);
HRESULT STDMETHODCALLTYPE create_deferred_context_hook(
    ID3D11Device* device, UINT context_flags,
    ID3D11DeviceContext** deferred_context);
bool install_context_upload_hooks(ID3D11DeviceContext* context);
HRESULT STDMETHODCALLTYPE create_texture_2d_hook(
    ID3D11Device* device, const D3D11_TEXTURE2D_DESC* description,
    const D3D11_SUBRESOURCE_DATA* initial_data,
    ID3D11Texture2D** texture) {
    D3D11_TEXTURE2D_DESC expanded_description{};
    const D3D11_TEXTURE2D_DESC* effective_description = description;
    std::array<void*, 8> creation_frames{};
    USHORT creation_frame_count{};
    if (description && description->Width == 1280 &&
        description->Height == 720) {
        log_target_runtime_regions();
        creation_frame_count = RtlCaptureStackBackTrace(
            1, static_cast<DWORD>(creation_frames.size()),
            creation_frames.data(), nullptr);
        const auto& frames = creation_frames;
        const auto frame_count = creation_frame_count;
        const auto module =
            reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto stack_contains_rva =
            [&](const std::uintptr_t rva) {
                const auto target = module + rva;
                return std::any_of(
                    frames.begin(), frames.begin() + frame_count,
                    [&](const void* frame) {
                        return reinterpret_cast<std::uintptr_t>(frame) ==
                               target;
                    });
            };

        constexpr std::uintptr_t scene_color_create_rva = 0x24F1F23;
        constexpr std::uintptr_t scene_depth_create_rva = 0x1AED61A;
        const auto expand_scene_color =
            g_expand_scene_resources &&
            description->Format == DXGI_FORMAT_R11G11B10_FLOAT &&
            description->BindFlags == 0xA8 &&
            stack_contains_rva(scene_color_create_rva);
        const auto expand_scene_depth =
            g_expand_scene_resources &&
            description->Format == DXGI_FORMAT_R32_TYPELESS &&
            description->BindFlags == 0x48 &&
            stack_contains_rva(scene_depth_create_rva);
        if (expand_scene_color || expand_scene_depth) {
            expanded_description = *description;
            expanded_description.Height = g_target_height;
            effective_description = &expanded_description;
            auto& logged = expand_scene_color
                               ? g_logged_scene_color_expansion
                               : g_logged_scene_depth_expansion;
            if (!logged.exchange(true)) {
                Logger::instance().write(
                    L"Selective scene {} resource expanded: {}x{} -> "
                    L"{}x{}, format={}, creator=MonsterHunterWorld.exe"
                    L"+0x{:X}",
                    expand_scene_color ? L"color" : L"depth",
                    description->Width, description->Height,
                    expanded_description.Width,
                    expanded_description.Height,
                    static_cast<unsigned>(description->Format),
                    expand_scene_color ? scene_color_create_rva
                                       : scene_depth_create_rva);
            }
        }

        auto key = mix_key(1469598103934665603ULL, description->Format);
        key = mix_key(key, description->BindFlags);
        key = mix_key(key, description->Usage);
        key = mix_key(key, description->MipLevels);
        key = mix_key(key, description->ArraySize);
        key = mix_key(key, description->SampleDesc.Count);
        for (USHORT index = 0; index < frame_count; ++index) {
            key = mix_key(key,
                          reinterpret_cast<std::uintptr_t>(frames[index]));
        }

        if (mark_key_once(g_logged_target_textures, key)) {
            Logger::instance().write(
                L"DX11 CreateTexture2D 1280x720: format={}, bind=0x{:X}, "
                L"usage={}, cpu=0x{:X}, misc=0x{:X}, mips={}, array={}, "
                L"samples={} quality={}, initial_data={}, stack_frames={}",
                static_cast<unsigned>(description->Format),
                description->BindFlags,
                static_cast<unsigned>(description->Usage),
                description->CPUAccessFlags, description->MiscFlags,
                description->MipLevels, description->ArraySize,
                description->SampleDesc.Count,
                description->SampleDesc.Quality,
                initial_data ? L"yes" : L"no", frame_count);
            for (USHORT index = 0; index < frame_count; ++index) {
                const auto source = describe_callsite(
                    reinterpret_cast<std::uintptr_t>(frames[index]));
                Logger::instance().write(L"  texture_stack[{}]={}+0x{:X}",
                                         index, source.module, source.offset);
            }
            for (USHORT index = 0; index < frame_count; ++index) {
                log_code_window(
                    reinterpret_cast<std::uintptr_t>(frames[index]));
            }
        }
    }
    const auto result = g_original_create_texture_2d(
        device, effective_description, initial_data, texture);
    const auto candidate_format =
        description &&
        (description->Format == DXGI_FORMAT_R11G11B10_FLOAT ||
         description->Format == DXGI_FORMAT_R32_TYPELESS ||
         description->Format == DXGI_FORMAT_R8_UNORM);
    if (SUCCEEDED(result) && texture && *texture && description &&
        description->Width == 1280 && description->Height == 720 &&
        candidate_format) {
        register_resource_origin(*texture, *description, creation_frames,
                                 creation_frame_count);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* swapchain,
                                       UINT sync_interval, UINT flags) {
    if (!g_swapchain_logged.exchange(true)) {
        DXGI_SWAP_CHAIN_DESC description{};
        if (SUCCEEDED(swapchain->GetDesc(&description))) {
            Logger::instance().write(
                L"DX11 real swap chain: {}x{}, format {}, window {:p}",
                description.BufferDesc.Width, description.BufferDesc.Height,
                static_cast<unsigned>(description.BufferDesc.Format),
                static_cast<void*>(description.OutputWindow));
        }
    }
    if (!g_real_context_hook_attempted.exchange(true)) {
        log_runtime_aspect_xrefs();
        // The full writable-memory scan can race engine allocation teardown.
        // It is only needed by the explicitly enabled active-rectangle
        // experiment, not by the selective aspect patch.
        if (g_patch_active_rect) {
            start_active_rect_trace();
        }
        install_resource_constructor_hook(Logger::instance());
        apply_native_aspect_mode_zero();
        ID3D11Device* device{};
        if (SUCCEEDED(swapchain->GetDevice(
                __uuidof(ID3D11Device),
                reinterpret_cast<void**>(&device))) &&
            device) {
            ID3D11DeviceContext* context{};
            device->GetImmediateContext(&context);
            if (context) {
                auto** context_vtable = *reinterpret_cast<void***>(context);
                const auto viewport_ok = replace_vtable_entry(
                    &context_vtable[44],
                    reinterpret_cast<void*>(&rs_set_viewports_hook),
                    reinterpret_cast<void**>(&g_original_rs_set_viewports));
                const auto scissor_ok = replace_vtable_entry(
                    &context_vtable[45],
                    reinterpret_cast<void*>(&rs_set_scissor_rects_hook),
                    reinterpret_cast<void**>(
                        &g_original_rs_set_scissor_rects));
                const auto upload_hooks_ok =
                    install_context_upload_hooks(context);
                Logger::instance().write(
                    L"DX11 real-context hooks: RSSetViewports={}, "
                    L"RSSetScissorRects={}, UploadHooks={}",
                    viewport_ok ? L"installed" : L"failed",
                    scissor_ok ? L"installed" : L"failed",
                    upload_hooks_ok ? L"installed" : L"failed");
                context->Release();
            } else {
                Logger::instance().write(
                    L"DX11 real immediate context was unavailable");
            }
            device->Release();
        } else {
            Logger::instance().write(
                L"DX11 real device was unavailable from swap chain");
        }
    }
    log_native_aspect_mode_result();
    return g_original_present(swapchain, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE resize_buffers_hook(IDXGISwapChain* swapchain,
                                              UINT buffer_count, UINT width,
                                              UINT height, DXGI_FORMAT format,
                                              UINT flags) {
    Logger::instance().write(L"DX11 ResizeBuffers: {}x{}, buffers {}, format {}",
                             width, height, buffer_count,
                             static_cast<unsigned>(format));
    return g_original_resize_buffers(swapchain, buffer_count, width, height,
                                     format, flags);
}

void STDMETHODCALLTYPE rs_set_viewports_hook(
    ID3D11DeviceContext* context, UINT count,
    const D3D11_VIEWPORT* viewports) {
    std::array<D3D11_VIEWPORT,
               D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
        expanded_viewports{};
    const D3D11_VIEWPORT* effective_viewports = viewports;
    if (g_expand_scene_resources && viewports && count > 0 &&
        count <= expanded_viewports.size()) {
        ID3D11RenderTargetView* render_target{};
        ID3D11DepthStencilView* depth_target{};
        context->OMGetRenderTargets(1, &render_target, &depth_target);
        const auto color = describe_view(render_target);
        const auto depth = describe_view(depth_target);
        if (render_target) {
            render_target->Release();
        }
        if (depth_target) {
            depth_target->Release();
        }
        const auto selected_scene_target =
            (color.available && color.width == 1280 &&
             color.height == g_target_height &&
             color.format == DXGI_FORMAT_R11G11B10_FLOAT) ||
            (depth.available && depth.width == 1280 &&
             depth.height == g_target_height &&
             depth.format == DXGI_FORMAT_R32_TYPELESS);
        if (selected_scene_target) {
            std::copy_n(viewports, count, expanded_viewports.begin());
            bool changed{};
            for (UINT index = 0; index < count; ++index) {
                auto& viewport = expanded_viewports[index];
                if (std::fabs(viewport.TopLeftX) < 0.1F &&
                    std::fabs(viewport.TopLeftY) < 0.1F &&
                    std::fabs(viewport.Width - 1280.0F) < 0.1F &&
                    std::fabs(viewport.Height - 720.0F) < 0.1F) {
                    viewport.Height = static_cast<float>(g_target_height);
                    changed = true;
                }
            }
            if (changed) {
                effective_viewports = expanded_viewports.data();
                if (!g_logged_scene_viewport_expansion.exchange(true)) {
                    Logger::instance().write(
                        L"Selective scene viewport expanded: "
                        L"1280x720 -> {}x{}",
                        g_target_width, g_target_height);
                }
            }
        }
    }
    if (viewports && count > 0) {
        for (UINT index = 0; index < count; ++index) {
            const auto& viewport = viewports[index];
            if (is_target_viewport(viewport)) {
                log_target_viewport(context, viewport);
            }
            const auto callsite =
                reinterpret_cast<std::uintptr_t>(_ReturnAddress());
            if (mark_key_once(g_logged_viewports,
                              viewport_key(callsite, viewport))) {
                const auto source = describe_callsite(callsite);
                Logger::instance().write(
                    L"DX11 viewport: index={}/{}, x={:.1f}, y={:.1f}, "
                    L"w={:.1f}, h={:.1f}, depth={:.3f}..{:.3f}, "
                    L"source={}+0x{:X}",
                    index + 1, count, viewport.TopLeftX, viewport.TopLeftY,
                    viewport.Width, viewport.Height, viewport.MinDepth,
                    viewport.MaxDepth, source.module, source.offset);
            }
        }

    }
    g_original_rs_set_viewports(context, count, effective_viewports);
}

bool looks_like_projection_matrix(const float* matrix) {
    for (std::size_t index = 0; index < 16; ++index) {
        if (!std::isfinite(matrix[index])) {
            return false;
        }
    }
    const auto x_scale = std::fabs(matrix[0]);
    const auto y_scale = std::fabs(matrix[5]);
    if (x_scale < 0.1F || x_scale > 10.0F ||
        y_scale < 0.1F || y_scale > 10.0F ||
        std::fabs((y_scale / x_scale) - (16.0F / 9.0F)) > 0.003F ||
        std::fabs(matrix[15]) > 0.001F) {
        return false;
    }
    const auto row_major = std::fabs(std::fabs(matrix[11]) - 1.0F) < 0.02F;
    const auto column_major =
        std::fabs(std::fabs(matrix[14]) - 1.0F) < 0.02F;
    return row_major || column_major;
}

void log_projection_candidates(ID3D11Resource* resource, const void* data,
                               UINT byte_width, const wchar_t* upload_path) {
    if (!resource || !data || byte_width < sizeof(float) * 16) {
        return;
    }
    const auto* bytes = static_cast<const std::byte*>(data);
    for (UINT offset = 0; offset + sizeof(float) * 16 <= byte_width;
         offset += 16) {
        const auto* matrix = reinterpret_cast<const float*>(bytes + offset);
        if (!looks_like_projection_matrix(matrix)) {
            continue;
        }
        std::array<void*, 8> frames{};
        const auto frame_count = RtlCaptureStackBackTrace(
            1, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
        auto key = mix_key(1469598103934665603ULL, byte_width);
        key = mix_key(key, offset);
        if (frame_count > 1) {
            key = mix_key(
                key, reinterpret_cast<std::uintptr_t>(frames[1]));
        }
        if (!mark_key_once(g_logged_projection_candidates, key)) {
            continue;
        }
        Logger::instance().write(
            L"DX11 projection candidate: path={}, buffer={:p}, bytes={}, "
            L"offset=0x{:X}, m00={:.6f}, m11={:.6f}, ratio={:.6f}, "
            L"m22={:.6f}, m23={:.6f}, m32={:.6f}, stack_frames={}",
            upload_path, static_cast<void*>(resource), byte_width, offset,
            matrix[0], matrix[5], std::fabs(matrix[5] / matrix[0]),
            matrix[10], matrix[11], matrix[14], frame_count);
        for (USHORT index = 0; index < frame_count; ++index) {
            const auto source = describe_callsite(
                reinterpret_cast<std::uintptr_t>(frames[index]));
            Logger::instance().write(
                L"  projection_stack[{}]={}+0x{:X}", index,
                source.module, source.offset);
        }
    }
}

std::optional<ContextUploadHooks> find_context_upload_hooks(
    ID3D11DeviceContext* context) {
    if (!context) {
        return std::nullopt;
    }
    auto** vtable = *reinterpret_cast<void***>(context);
    std::scoped_lock lock(g_context_upload_hooks_mutex);
    for (std::size_t index = 0; index < g_context_upload_hook_count;
         ++index) {
        if (g_context_upload_hooks[index].vtable == vtable) {
            return g_context_upload_hooks[index];
        }
    }
    return std::nullopt;
}

bool install_context_upload_hooks(ID3D11DeviceContext* context) {
    if (!context) {
        return false;
    }
    auto** vtable = *reinterpret_cast<void***>(context);
    std::scoped_lock lock(g_context_upload_hooks_mutex);
    for (std::size_t index = 0; index < g_context_upload_hook_count;
         ++index) {
        if (g_context_upload_hooks[index].vtable == vtable) {
            return true;
        }
    }
    if (g_context_upload_hook_count >= g_context_upload_hooks.size()) {
        return false;
    }

    ContextUploadHooks hooks{};
    hooks.vtable = vtable;
    void* original_map{};
    void* original_unmap{};
    void* original_update{};
    const auto map_ok = replace_vtable_entry(
        &vtable[14], reinterpret_cast<void*>(&map_hook), &original_map);
    const auto unmap_ok = replace_vtable_entry(
        &vtable[15], reinterpret_cast<void*>(&unmap_hook),
        &original_unmap);
    const auto update_ok = replace_vtable_entry(
        &vtable[48], reinterpret_cast<void*>(&update_subresource_hook),
        &original_update);
    if (!map_ok || !unmap_ok || !update_ok) {
        return false;
    }
    hooks.map = reinterpret_cast<MapFn>(original_map);
    hooks.unmap = reinterpret_cast<UnmapFn>(original_unmap);
    hooks.update_subresource =
        reinterpret_cast<UpdateSubresourceFn>(original_update);
    g_context_upload_hooks[g_context_upload_hook_count++] = hooks;
    return true;
}

HRESULT STDMETHODCALLTYPE map_hook(
    ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource,
    D3D11_MAP map_type, UINT map_flags,
    D3D11_MAPPED_SUBRESOURCE* mapped) {
    const auto hooks = find_context_upload_hooks(context);
    if (!hooks || !hooks->map) {
        return E_FAIL;
    }
    const auto result = hooks->map(context, resource, subresource, map_type,
                                   map_flags, mapped);
    g_mapped_constant_buffer = {};
    if (FAILED(result) || !resource || !mapped || !mapped->pData ||
        subresource != 0 ||
        (map_type != D3D11_MAP_WRITE_DISCARD &&
         map_type != D3D11_MAP_WRITE_NO_OVERWRITE)) {
        return result;
    }
    ID3D11Buffer* buffer{};
    if (SUCCEEDED(resource->QueryInterface(
            __uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buffer))) &&
        buffer) {
        D3D11_BUFFER_DESC description{};
        buffer->GetDesc(&description);
        buffer->Release();
        if ((description.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0 &&
            description.ByteWidth >= 64 && description.ByteWidth <= 65536) {
            g_mapped_constant_buffer =
                {resource, mapped->pData, description.ByteWidth};
        }
    }
    return result;
}

void STDMETHODCALLTYPE unmap_hook(ID3D11DeviceContext* context,
                                  ID3D11Resource* resource,
                                  UINT subresource) {
    const auto mapped = g_mapped_constant_buffer;
    if (mapped.resource == resource && mapped.data && subresource == 0) {
        log_projection_candidates(resource, mapped.data, mapped.byte_width,
                                  L"Map/Unmap");
    }
    g_mapped_constant_buffer = {};
    const auto hooks = find_context_upload_hooks(context);
    if (hooks && hooks->unmap) {
        hooks->unmap(context, resource, subresource);
    }
}

void STDMETHODCALLTYPE update_subresource_hook(
    ID3D11DeviceContext* context, ID3D11Resource* resource,
    UINT destination_subresource, const D3D11_BOX* destination_box,
    const void* source_data, UINT source_row_pitch,
    UINT source_depth_pitch) {
    if (resource && source_data && destination_subresource == 0) {
        ID3D11Buffer* buffer{};
        if (SUCCEEDED(resource->QueryInterface(
                __uuidof(ID3D11Buffer),
                reinterpret_cast<void**>(&buffer))) &&
            buffer) {
            D3D11_BUFFER_DESC description{};
            buffer->GetDesc(&description);
            buffer->Release();
            if ((description.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0 &&
                description.ByteWidth >= 64 &&
                description.ByteWidth <= 65536) {
                UINT byte_width = description.ByteWidth;
                if (destination_box) {
                    byte_width = destination_box->right -
                                 destination_box->left;
                }
                log_projection_candidates(
                    resource, source_data, byte_width,
                    L"UpdateSubresource");
            }
        }
    }
    const auto hooks = find_context_upload_hooks(context);
    if (hooks && hooks->update_subresource) {
        hooks->update_subresource(
            context, resource, destination_subresource, destination_box,
            source_data, source_row_pitch, source_depth_pitch);
    }
}

HRESULT STDMETHODCALLTYPE create_deferred_context_hook(
    ID3D11Device* device, UINT context_flags,
    ID3D11DeviceContext** deferred_context) {
    const auto result = g_original_create_deferred_context(
        device, context_flags, deferred_context);
    if (SUCCEEDED(result) && deferred_context && *deferred_context) {
        const auto hook_ok =
            install_context_upload_hooks(*deferred_context);
        const auto count = g_deferred_context_count.fetch_add(1) + 1;
        Logger::instance().write(
            L"DX11 deferred context #{} created: upload hooks={}",
            count, hook_ok ? L"installed" : L"failed");
    }
    return result;
}

void STDMETHODCALLTYPE rs_set_scissor_rects_hook(
    ID3D11DeviceContext* context, UINT count, const D3D11_RECT* rects) {
    if (rects && count > 0) {
        for (UINT index = 0; index < count; ++index) {
            const auto& rect = rects[index];
            const auto callsite =
                reinterpret_cast<std::uintptr_t>(_ReturnAddress());
            if (mark_key_once(g_logged_scissors,
                              scissor_key(callsite, rect))) {
                const auto source = describe_callsite(callsite);
                Logger::instance().write(
                    L"DX11 scissor: index={}/{}, left={}, top={}, right={}, "
                    L"bottom={}, source={}+0x{:X}",
                    index + 1, count, rect.left, rect.top, rect.right,
                    rect.bottom, source.module, source.offset);
            }
        }
    }
    g_original_rs_set_scissor_rects(context, count, rects);
}

}  // namespace

bool install_d3d11_diagnostics(Logger& log, const Config& config) {
    g_expand_720p = config.experimental_expand_720p &&
                    config.width == 1280 && config.height > 720;
    g_expand_scene_resources =
        config.experimental_expand_scene_resources &&
        config.width == 1280 && config.height == 800;
    g_patch_active_rect = config.experimental_patch_active_rect &&
                          config.width == 1280 && config.height == 800;
    g_target_width = config.width;
    g_target_height = config.height;
    log.write(L"Experimental 720p expansion: {} (target {}x{})",
              g_expand_720p ? L"enabled" : L"disabled", g_target_width,
              g_target_height);
    log.write(L"Selective scene-resource expansion: {}",
              g_expand_scene_resources ? L"enabled" : L"disabled");
    log.write(L"Experimental active-rect-only patch: {}",
              g_patch_active_rect ? L"enabled" : L"disabled");
    if (!patch_render_aspect_constant(config, log)) {
        return false;
    }
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferDesc.Width = 2;
    description.BufferDesc.Height = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 1;
    description.OutputWindow = GetDesktopWindow();
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
    IDXGISwapChain* swapchain{};
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    const auto result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &requested, 1,
        D3D11_SDK_VERSION, &description, &swapchain, &device, nullptr,
        &context);
    if (FAILED(result)) {
        log.write(L"DX11 diagnostic hook: dummy device creation failed 0x{:08X}",
                  static_cast<unsigned>(result));
        return false;
    }

    auto** swapchain_vtable = *reinterpret_cast<void***>(swapchain);
    const auto present_ok = replace_vtable_entry(
        &swapchain_vtable[8], reinterpret_cast<void*>(&present_hook),
        reinterpret_cast<void**>(&g_original_present));
    const auto resize_ok = replace_vtable_entry(
        &swapchain_vtable[13], reinterpret_cast<void*>(&resize_buffers_hook),
        reinterpret_cast<void**>(&g_original_resize_buffers));
    auto** device_vtable = *reinterpret_cast<void***>(device);
    const auto texture_ok = replace_vtable_entry(
        &device_vtable[5],
        reinterpret_cast<void*>(&create_texture_2d_hook),
        reinterpret_cast<void**>(&g_original_create_texture_2d));
    const auto deferred_context_ok = replace_vtable_entry(
        &device_vtable[27],
        reinterpret_cast<void*>(&create_deferred_context_hook),
        reinterpret_cast<void**>(&g_original_create_deferred_context));
    context->Release();
    device->Release();
    swapchain->Release();

    if (!present_ok || !resize_ok || !texture_ok ||
        !deferred_context_ok) {
        log.write(
            L"DX11 diagnostic hook failed: Present={}, ResizeBuffers={}, "
            L"CreateTexture2D={}, CreateDeferredContext={}",
            present_ok, resize_ok, texture_ok, deferred_context_ok);
        return false;
    }
    log.write(
        L"DX11 diagnostics installed: Present, ResizeBuffers and early "
        L"CreateTexture2D/CreateDeferredContext; real-context hooks "
        L"deferred to first Present");
    return true;
}

}  // namespace mhw
