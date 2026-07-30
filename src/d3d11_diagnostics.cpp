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

PresentFn g_original_present{};
ResizeBuffersFn g_original_resize_buffers{};
CreateTexture2DFn g_original_create_texture_2d{};
ResourceConstructorFn g_original_resource_constructor{};
RSSetViewportsFn g_original_rs_set_viewports{};
RSSetScissorRectsFn g_original_rs_set_scissor_rects{};
std::atomic<bool> g_swapchain_logged{false};
std::atomic<bool> g_real_context_hook_attempted{false};
std::array<std::atomic<std::uint64_t>, 64> g_logged_viewports{};
std::array<std::atomic<std::uint64_t>, 64> g_logged_scissors{};
std::array<std::atomic<std::uint64_t>, 128> g_logged_target_viewports{};
std::array<std::atomic<std::uint64_t>, 256> g_logged_target_textures{};
std::array<std::atomic<std::uint64_t>, 256> g_logged_code_windows{};
std::atomic<bool> g_logged_runtime_regions{false};
std::atomic<bool> g_active_rect_trace_started{false};
std::atomic<bool> g_active_rect_patch_applied{false};
std::atomic<bool> g_aspect_xrefs_logged{false};
bool g_expand_720p{};
bool g_patch_active_rect{};
UINT g_target_width{1280};
UINT g_target_height{800};
std::atomic<bool> g_logged_upstream_expansion{false};

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
    constexpr std::uint32_t expected = 0x3FE38E39U;
    auto* address = reinterpret_cast<std::uint32_t*>(
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) +
        constant_rva);
    if (!address || *address != expected) {
        log.write(
            L"Render aspect constant patch aborted: value mismatch at "
            L"MonsterHunterWorld.exe+0x{:X}",
            constant_rva);
        return false;
    }
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
        L"Render aspect constant patched at "
        L"MonsterHunterWorld.exe+0x{:X}: 1.777778 -> {:.6f}",
        constant_rva, aspect);
    return true;
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
        result = {true, description.Width, description.Height,
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
    key = mix_key(key, depth.width);
    key = mix_key(key, depth.height);
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
}

void STDMETHODCALLTYPE rs_set_viewports_hook(
    ID3D11DeviceContext* context, UINT count,
    const D3D11_VIEWPORT* viewports);
void STDMETHODCALLTYPE rs_set_scissor_rects_hook(
    ID3D11DeviceContext* context, UINT count, const D3D11_RECT* rects);
HRESULT STDMETHODCALLTYPE create_texture_2d_hook(
    ID3D11Device* device, const D3D11_TEXTURE2D_DESC* description,
    const D3D11_SUBRESOURCE_DATA* initial_data,
    ID3D11Texture2D** texture) {
    if (description && description->Width == 1280 &&
        description->Height == 720) {
        log_target_runtime_regions();
        std::array<void*, 8> frames{};
        const auto frame_count = RtlCaptureStackBackTrace(
            1, static_cast<DWORD>(frames.size()), frames.data(), nullptr);

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
    return g_original_create_texture_2d(device, description, initial_data,
                                        texture);
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
        start_active_rect_trace();
        install_resource_constructor_hook(Logger::instance());
        ID3D11Device* device{};
        if (SUCCEEDED(swapchain->GetDevice(__uuidof(ID3D11Device),
            reinterpret_cast<void**>(&device))) &&
            device) {
            auto** device_vtable = *reinterpret_cast<void***>(device);
            const auto texture_ok = replace_vtable_entry(
                &device_vtable[5],
                reinterpret_cast<void*>(&create_texture_2d_hook),
                reinterpret_cast<void**>(&g_original_create_texture_2d));
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
                Logger::instance().write(
                    L"DX11 real-device/context hooks: CreateTexture2D={}, "
                    L"RSSetViewports={}, RSSetScissorRects={}",
                    texture_ok ? L"installed" : L"failed",
                    viewport_ok ? L"installed" : L"failed",
                    scissor_ok ? L"installed" : L"failed");
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
    g_original_rs_set_viewports(context, count, viewports);
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
    g_patch_active_rect = config.experimental_patch_active_rect &&
                          config.width == 1280 && config.height == 800;
    g_target_width = config.width;
    g_target_height = config.height;
    log.write(L"Experimental 720p expansion: {} (target {}x{})",
              g_expand_720p ? L"enabled" : L"disabled", g_target_width,
              g_target_height);
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

    context->Release();
    device->Release();
    swapchain->Release();

    if (!present_ok || !resize_ok) {
        log.write(
            L"DX11 diagnostic hook failed: Present={}, ResizeBuffers={}",
            present_ok, resize_ok);
        return false;
    }
    log.write(L"DX11 diagnostics installed: Present, ResizeBuffers; "
              L"CreateTexture2D and context hooks deferred to real device");
    return true;
}

}  // namespace mhw
