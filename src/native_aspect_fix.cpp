#include "native_aspect_fix.hpp"

#include "config.hpp"
#include "log.hpp"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace mhw {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using SetAspectModeFn = void (*)(void*, std::int32_t);
using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
using RSSetScissorRectsFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
using CreateCommittedResourceFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE*, REFIID, void**);
using CopyTextureRegionFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT,
    UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
using CopyResourceFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using ResolveSubresourceFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, ID3D12Resource*, UINT, ID3D12Resource*, UINT,
    DXGI_FORMAT);
using CreateRenderTargetViewFn = void(STDMETHODCALLTYPE*)(
    ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*,
    D3D12_CPU_DESCRIPTOR_HANDLE);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
    const D3D12_CPU_DESCRIPTOR_HANDLE*);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);

constexpr std::uintptr_t kRenderManagerGlobalRva = 0x51C4480;
constexpr std::uintptr_t kOptionsGlobalRva = 0x51C5320;
constexpr std::uintptr_t kAspectModeSetterRva = 0x229C790;
constexpr std::uintptr_t kActiveModeOffset = 0x7B43C;
constexpr std::uintptr_t kRequestedModeOffset = 0x7B440;
constexpr std::uintptr_t kContentWidthOffset = 0x198;
constexpr std::uintptr_t kContentHeightOffset = 0x19C;
constexpr std::uintptr_t kOutputDimensionsOffset = 0x1F448;

// 2026-08-06: after the native aspect fix succeeds, a 512 KB sweep of the
// render-manager struct at 1920x1200 found exactly two dword pairs still
// holding the 16:9 size (1920x1080) while every other layout field had been
// rebuilt to 1920x1200. 1200 - 1080 = 120 px, which matches the completion
// screen's background not covering the full frame.
//
// What these fields actually drive is still unconfirmed. Writing them as 1200
// does change the background, but there is no clean reading on HUD side
// effects yet: an earlier session believed centred HUD elements shifted
// sideways, then found the game natively draws two R/F prompt sets (one below
// the item bar on the right, which spawns another one in the middle when it is
// used). That observation is retracted, and so is the "layout is computed once
// and cached" conclusion that was derived from it.
//
// Overrides are therefore applied early and re-asserted during startup, which
// is harmless either way, and left at 0 by default until a controlled test
// shows which value belongs here.
constexpr std::uintptr_t kUiReferenceWidthOffset = 0x1A0;
constexpr std::uintptr_t kUiReferenceHeightOffset = 0x1A4;
constexpr std::uintptr_t kSecondaryLayoutWidthOffset = 0x210;
constexpr std::uintptr_t kSecondaryLayoutHeightOffset = 0x214;
// Graphics-options byte. While it is non-zero, dozens of menu layout sites
// read the native content size at +0x198. While it is zero they read the
// 16:9 pair at +0x210. Live disassembly, 2026-10-27, pid 19184.
constexpr std::uintptr_t kUiUsesNativeSizeFlag = 0x2CDA;

PresentFn g_original_present{};
CreateSwapChainFn g_original_create_swapchain{};
CreateSwapChainForHwndFn g_original_create_swapchain_for_hwnd{};
std::atomic<bool> g_request_attempted{false};
std::atomic<bool> g_result_logged{false};
std::atomic<bool> g_present_hook_installed{false};
std::mutex g_present_hook_mutex;
bool g_remove_letterbox{};
bool g_keep_ui_16x9{true};
std::uint32_t g_ui_reference_width{};
std::uint32_t g_ui_reference_height{};
std::uint32_t g_secondary_layout_width{};
std::uint32_t g_secondary_layout_height{};
std::atomic<bool> g_layout_override_logged{false};
std::atomic<bool> g_ui_16x9_logged{false};
RSSetViewportsFn g_original_rs_set_viewports{};
RSSetScissorRectsFn g_original_rs_set_scissor_rects{};
CreateCommittedResourceFn g_original_create_committed_resource{};
CopyTextureRegionFn g_original_copy_texture_region{};
CopyResourceFn g_original_copy_resource{};
ResolveSubresourceFn g_original_resolve_subresource{};
CreateRenderTargetViewFn g_original_create_render_target_view{};
OMSetRenderTargetsFn g_original_om_set_render_targets{};
DrawIndexedInstancedFn g_original_draw_indexed_instanced{};
std::uint32_t g_trace_output_width{};
std::uint32_t g_trace_output_height{};
bool g_promote_widescreen_textures{};
std::atomic<std::uint32_t> g_promoted_texture_count{};
std::atomic<std::uint32_t> g_traced_resource_count{};
std::atomic<std::uint32_t> g_traced_copy_count{};
UINT g_rtv_descriptor_increment{};
std::mutex g_layout_trace_mutex;
std::unordered_set<std::uint64_t> g_seen_viewports;
std::unordered_set<std::uint64_t> g_seen_scissors;
std::atomic<std::uint32_t> g_traced_draw_count{};
std::atomic<ULONGLONG> g_trace_started_ticks{};
std::atomic<ULONGLONG> g_ui_draw_trace_until_ticks{};
std::atomic<std::uint32_t> g_current_viewport_width{};
std::atomic<std::uint32_t> g_current_viewport_height{};
std::atomic<std::uint32_t> g_current_scissor_width{};
std::atomic<std::uint32_t> g_current_scissor_height{};

struct RenderTargetInfo {
    std::uintptr_t resource{};
    std::uint64_t width{};
    std::uint32_t height{};
    DXGI_FORMAT format{};
};

std::unordered_map<SIZE_T, RenderTargetInfo> g_render_targets;
std::unordered_map<SIZE_T, ULONGLONG> g_render_target_last_bind_ticks;

void log_layout_callstack(std::wstring_view label);

void STDMETHODCALLTYPE draw_indexed_instanced_hook(
    ID3D12GraphicsCommandList* command_list, UINT index_count, UINT instance_count,
    UINT start_index, INT base_vertex, UINT start_instance) {
    const auto now = GetTickCount64();
    const auto log_draw = now <= g_ui_draw_trace_until_ticks.load() &&
        index_count <= 512 && instance_count <= 8 &&
        g_traced_draw_count.fetch_add(1) < 2048;
    if (log_draw) {
        Logger::instance().write(
            L"DX12 DrawIndexedInstanced: indices={}, instances={}, startIndex={}, "
            L"baseVertex={}, startInstance={}, viewport={}x{}, scissor={}x{}",
            index_count, instance_count, start_index, base_vertex, start_instance,
            g_current_viewport_width.load(), g_current_viewport_height.load(),
            g_current_scissor_width.load(), g_current_scissor_height.load());
        log_layout_callstack(L"DrawIndexedInstanced");
    }
    if (g_original_draw_indexed_instanced) {
        g_original_draw_indexed_instanced(command_list, index_count, instance_count,
                                          start_index, base_vertex, start_instance);
    }
}

void request_native_aspect_mode();
void log_native_aspect_result();
void apply_layout_overrides(std::uintptr_t manager_address);
void apply_ui_16x9_split(std::uintptr_t manager_address);

void log_layout_callstack(std::wstring_view label) {
    void* frames[16]{};
    const auto count = CaptureStackBackTrace(1, static_cast<DWORD>(std::size(frames)),
                                             frames, nullptr);
    const auto module =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    for (USHORT index = 0; index < count; ++index) {
        const auto address = reinterpret_cast<std::uintptr_t>(frames[index]);
        if (module && address >= module && address < module + 0x6000000) {
            Logger::instance().write(L"{} caller[{}]=MonsterHunterWorld.exe+0x{:X}",
                                     label, index, address - module);
        }
    }
}

std::uintptr_t capture_game_caller_rva() {
    void* frames[16]{};
    const auto count = CaptureStackBackTrace(
        1, static_cast<DWORD>(std::size(frames)), frames, nullptr);
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    for (USHORT index = 0; index < count; ++index) {
        const auto address = reinterpret_cast<std::uintptr_t>(frames[index]);
        if (module && address >= module && address < module + 0x6000000) {
            return address - module;
        }
    }
    return 0;
}

std::uint64_t layout_key(std::int32_t x, std::int32_t y,
                         std::uint32_t width, std::uint32_t height) {
    auto mix = [](std::uint64_t value) {
        value ^= value >> 33;
        value *= 0xff51afd7ed558ccdULL;
        value ^= value >> 33;
        return value;
    };
    return mix(static_cast<std::uint32_t>(x)) ^
           (mix(static_cast<std::uint32_t>(y)) << 1) ^
           (mix(width) << 2) ^ (mix(height) << 3);
}

bool is_large_layout(std::uint32_t width, std::uint32_t height) {
    return g_trace_output_width != 0 && g_trace_output_height != 0 &&
           width >= g_trace_output_width / 2 &&
           height >= g_trace_output_height / 2;
}

bool is_trace_texture(const D3D12_RESOURCE_DESC& description) {
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        g_trace_output_width == 0) {
        return false;
    }
    const auto widescreen_height = static_cast<std::uint32_t>(
        std::lround(static_cast<double>(g_trace_output_width) * 9.0 / 16.0));
    return description.Width == g_trace_output_width &&
        (description.Height == g_trace_output_height ||
         (description.Height >= widescreen_height - 2 &&
          description.Height <= widescreen_height + 2));
}

// True when a texture is exactly the swap-chain width but only 16:9 tall, i.e.
// the engine derived its height as width * 9/16 instead of using the real
// output height.
//
// 2026-08-07: proven that this height is COMPUTED, not read from any struct
// field. A 512 KB sweep of the render manager showed every layout field sitting
// at the correct 1920x1200 (all four candidate dword pairs were forced to 1200
// and held), yet the game still allocated 44 textures at 1920x1080. The
// decisive evidence came from a 2240x1463 session, which produced 2240x1260
// textures: 2240/1.7778 = 1260, a height that matches no window and no display
// mode, so it can only come from arithmetic in code.
//
// Rather than hunting that instruction, this promotes the allocation at the
// D3D12 boundary, which every such texture must pass through regardless of
// where the size was computed.
bool is_widescreen_shaped(const D3D12_RESOURCE_DESC& description) {
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        g_trace_output_width == 0 || g_trace_output_height == 0) {
        return false;
    }
    const auto widescreen_height = static_cast<std::uint32_t>(
        std::lround(static_cast<double>(g_trace_output_width) * 9.0 / 16.0));
    // Nothing to do when the display really is 16:9.
    if (widescreen_height >= g_trace_output_height) {
        return false;
    }
    return description.Width == g_trace_output_width &&
        description.Height >= widescreen_height - 2 &&
        description.Height <= widescreen_height + 2;
}

void log_texture_copy(std::wstring_view operation, ID3D12Resource* destination,
                      ID3D12Resource* source, const D3D12_BOX* source_box) {
    if (!destination || !source) {
        return;
    }
    const auto destination_desc = destination->GetDesc();
    const auto source_desc = source->GetDesc();
    if (!is_trace_texture(destination_desc) && !is_trace_texture(source_desc)) {
        return;
    }
    const auto dimensions_differ =
        destination_desc.Width != source_desc.Width ||
        destination_desc.Height != source_desc.Height;
    const auto partial_source = source_box &&
        (source_box->left != 0 || source_box->top != 0 ||
         source_box->front != 0 ||
         source_box->right != source_desc.Width ||
         source_box->bottom != source_desc.Height ||
         source_box->back != source_desc.DepthOrArraySize);
    if (!dimensions_differ && !partial_source) {
        return;
    }
    if (g_traced_copy_count.fetch_add(1, std::memory_order_relaxed) >= 1024) {
        return;
    }
    if (source_box) {
        Logger::instance().write(
            L"DX12 {}: src=0x{:X} {}x{} fmt={}, dst=0x{:X} {}x{} fmt={}, "
            L"box=({},{})->({},{})",
            operation, reinterpret_cast<std::uintptr_t>(source),
            source_desc.Width, source_desc.Height,
            static_cast<unsigned>(source_desc.Format),
            reinterpret_cast<std::uintptr_t>(destination),
            destination_desc.Width, destination_desc.Height,
            static_cast<unsigned>(destination_desc.Format), source_box->left,
            source_box->top, source_box->right, source_box->bottom);
    } else {
        Logger::instance().write(
            L"DX12 {}: src=0x{:X} {}x{} fmt={}, dst=0x{:X} {}x{} fmt={}",
            operation, reinterpret_cast<std::uintptr_t>(source),
            source_desc.Width, source_desc.Height,
            static_cast<unsigned>(source_desc.Format),
            reinterpret_cast<std::uintptr_t>(destination),
            destination_desc.Width, destination_desc.Height,
            static_cast<unsigned>(destination_desc.Format));
    }
    log_layout_callstack(L"DX12 copy");
}

void STDMETHODCALLTYPE copy_texture_region_hook(
    ID3D12GraphicsCommandList* command_list,
    const D3D12_TEXTURE_COPY_LOCATION* destination, UINT destination_x,
    UINT destination_y, UINT destination_z,
    const D3D12_TEXTURE_COPY_LOCATION* source, const D3D12_BOX* source_box) {
    if (destination && source) {
        log_texture_copy(L"CopyTextureRegion", destination->pResource,
                         source->pResource, source_box);
    }
    g_original_copy_texture_region(command_list, destination, destination_x,
                                   destination_y, destination_z, source,
                                   source_box);
}

void STDMETHODCALLTYPE copy_resource_hook(ID3D12GraphicsCommandList* command_list,
                                           ID3D12Resource* destination,
                                           ID3D12Resource* source) {
    log_texture_copy(L"CopyResource", destination, source, nullptr);
    g_original_copy_resource(command_list, destination, source);
}

void STDMETHODCALLTYPE resolve_subresource_hook(
    ID3D12GraphicsCommandList* command_list, ID3D12Resource* destination,
    UINT destination_subresource, ID3D12Resource* source,
    UINT source_subresource, DXGI_FORMAT format) {
    log_texture_copy(L"ResolveSubresource", destination, source, nullptr);
    g_original_resolve_subresource(command_list, destination,
                                   destination_subresource, source,
                                   source_subresource, format);
}

void STDMETHODCALLTYPE create_render_target_view_hook(
    ID3D12Device* device, ID3D12Resource* resource,
    const D3D12_RENDER_TARGET_VIEW_DESC* description,
    D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    g_original_create_render_target_view(device, resource, description,
                                         destination);
    if (!resource) {
        return;
    }
    const auto resource_description = resource->GetDesc();
    if (!is_trace_texture(resource_description)) {
        return;
    }
    const std::scoped_lock lock(g_layout_trace_mutex);
    g_render_targets[destination.ptr] = {
        reinterpret_cast<std::uintptr_t>(resource), resource_description.Width,
        resource_description.Height,
        description ? description->Format : resource_description.Format};
}

void STDMETHODCALLTYPE om_set_render_targets_hook(
    ID3D12GraphicsCommandList* command_list, UINT count,
    const D3D12_CPU_DESCRIPTOR_HANDLE* descriptors, BOOL single_range,
    const D3D12_CPU_DESCRIPTOR_HANDLE* depth_stencil) {
    if (count != 0 && descriptors) {
        const auto caller_rva = capture_game_caller_rva();
        const auto now = GetTickCount64();
        for (UINT index = 0; index < count; ++index) {
            const auto handle = single_range
                ? descriptors[0].ptr +
                      static_cast<SIZE_T>(index) * g_rtv_descriptor_increment
                : descriptors[index].ptr;
            RenderTargetInfo info{};
            bool should_log{};
            ULONGLONG idle_milliseconds{};
            {
                const std::scoped_lock lock(g_layout_trace_mutex);
                const auto match = g_render_targets.find(handle);
                if (match != g_render_targets.end()) {
                    info = match->second;
                    const auto previous =
                        g_render_target_last_bind_ticks.find(handle);
                    if (previous == g_render_target_last_bind_ticks.end()) {
                        should_log = true;
                    } else {
                        idle_milliseconds = now - previous->second;
                        should_log = idle_milliseconds >= 1500;
                    }
                    g_render_target_last_bind_ticks[handle] = now;
                }
            }
            if (should_log) {
                Logger::instance().write(
                    L"DX12 render target bound: slot={}, handle=0x{:X}, "
                    L"resource=0x{:X}, size={}x{}, format={}, idleMs={}, caller="
                    L"MonsterHunterWorld.exe+0x{:X}",
                    index, handle, info.resource, info.width, info.height,
                    static_cast<unsigned>(info.format), idle_milliseconds,
                    caller_rva);
                log_layout_callstack(L"DX12 render target");
            }
        }
    }
    g_original_om_set_render_targets(command_list, count, descriptors,
                                     single_range, depth_stencil);
}

void STDMETHODCALLTYPE rs_set_viewports_hook(
    ID3D12GraphicsCommandList* command_list, UINT count,
    const D3D12_VIEWPORT* viewports) {
    if (count == 1 && viewports) {
        const auto& viewport = viewports[0];
        const auto x = static_cast<std::int32_t>(std::lround(viewport.TopLeftX));
        const auto y = static_cast<std::int32_t>(std::lround(viewport.TopLeftY));
        const auto width = static_cast<std::uint32_t>(
            std::max(0L, std::lround(viewport.Width)));
        const auto height = static_cast<std::uint32_t>(
            std::max(0L, std::lround(viewport.Height)));
        g_current_viewport_width.store(width);
        g_current_viewport_height.store(height);
        if (is_large_layout(width, height)) {
            const std::scoped_lock lock(g_layout_trace_mutex);
            if (g_seen_viewports.size() < 256 &&
                g_seen_viewports.insert(layout_key(x, y, width, height)).second) {
                Logger::instance().write(
                    L"DX12 layout viewport: x={}, y={}, width={}, height={}, "
                    L"depth={:.3f}..{:.3f}",
                    x, y, width, height, viewport.MinDepth, viewport.MaxDepth);
                log_layout_callstack(L"DX12 viewport");
            }
        }
    }
    g_original_rs_set_viewports(command_list, count, viewports);
}

void STDMETHODCALLTYPE rs_set_scissor_rects_hook(
    ID3D12GraphicsCommandList* command_list, UINT count,
    const D3D12_RECT* rects) {
    if (count == 1 && rects) {
        const auto& rect = rects[0];
        const auto width = static_cast<std::uint32_t>(
            std::max<LONG>(0, rect.right - rect.left));
        const auto height = static_cast<std::uint32_t>(
            std::max<LONG>(0, rect.bottom - rect.top));
        g_current_scissor_width.store(width);
        g_current_scissor_height.store(height);
        if (is_large_layout(width, height)) {
            const std::scoped_lock lock(g_layout_trace_mutex);
            if (g_seen_scissors.size() < 256 &&
                g_seen_scissors
                    .insert(layout_key(rect.left, rect.top, width, height))
                    .second) {
                Logger::instance().write(
                    L"DX12 layout scissor: left={}, top={}, right={}, "
                    L"bottom={}, width={}, height={}",
                    rect.left, rect.top, rect.right, rect.bottom, width, height);
                log_layout_callstack(L"DX12 scissor");
            }
        }
    }
    g_original_rs_set_scissor_rects(command_list, count, rects);
}

HRESULT STDMETHODCALLTYPE create_committed_resource_hook(
    ID3D12Device* device, const D3D12_HEAP_PROPERTIES* heap_properties,
    D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC* description,
    D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE* clear_value,
    REFIID resource_iid, void** resource) {
    // Promote 16:9-shaped allocations to the real output height before the
    // driver sees them. Done on a copy so the caller's desc is never mutated;
    // the game may reuse or inspect that struct afterwards.
    D3D12_RESOURCE_DESC promoted_desc{};
    const D3D12_RESOURCE_DESC* effective_desc = description;
    bool promoted = false;
    if (g_promote_widescreen_textures && description &&
        is_widescreen_shaped(*description)) {
        promoted_desc = *description;
        promoted_desc.Height = g_trace_output_height;
        effective_desc = &promoted_desc;
        promoted = true;
    }

    const auto result = g_original_create_committed_resource(
        device, heap_properties, heap_flags, effective_desc, initial_state,
        clear_value, resource_iid, resource);

    if (promoted) {
        const auto index =
            g_promoted_texture_count.fetch_add(1, std::memory_order_relaxed);
        // A promoted allocation that fails is worth seeing every time; the
        // successful ones are capped so the log stays readable.
        if (index < 64 || FAILED(result)) {
            Logger::instance().write(
                L"Promoted widescreen texture #{}: {}x{} -> {}x{}, "
                L"format={}, flags=0x{:X}, result=0x{:08X}{}",
                index + 1, static_cast<std::uint32_t>(description->Width),
                description->Height,
                static_cast<std::uint32_t>(promoted_desc.Width),
                promoted_desc.Height,
                static_cast<unsigned>(promoted_desc.Format),
                static_cast<unsigned>(promoted_desc.Flags),
                static_cast<unsigned>(result),
                FAILED(result) ? L"  <<<< FAILED" : L"");
        }
    }

    if (description && description->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        g_trace_output_width != 0) {
        const auto native_height = g_trace_output_height;
        const auto widescreen_height = static_cast<std::uint32_t>(
            std::lround(static_cast<double>(g_trace_output_width) * 9.0 / 16.0));
        const auto width = static_cast<std::uint32_t>(description->Width);
        const auto height = description->Height;
        const auto target_size = width == g_trace_output_width &&
            (height == native_height ||
             (height >= widescreen_height - 2 && height <= widescreen_height + 2));
        const auto now = GetTickCount64();
        if (target_size && height == native_height &&
            description->Format == DXGI_FORMAT_R11G11B10_FLOAT &&
            (description->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 &&
            now - g_trace_started_ticks.load() >= 60000) {
            g_ui_draw_trace_until_ticks.store(now + 5000);
            Logger::instance().write(
                L"DX12 completion UI draw window opened for 5000 ms after "
                L"native-size capture texture creation");
        }
        if (target_size &&
            g_traced_resource_count.fetch_add(1, std::memory_order_relaxed) < 512) {
            Logger::instance().write(
                L"DX12 texture created: {}x{}, format={}, flags=0x{:X}, "
                L"heapType={}, heapFlags=0x{:X}, initialState=0x{:X}, "
                L"mips={}, samples={}, result=0x{:08X}, resource=0x{:X}",
                width, height, static_cast<unsigned>(description->Format),
                static_cast<unsigned>(description->Flags),
                heap_properties ? static_cast<unsigned>(heap_properties->Type) : 0,
                static_cast<unsigned>(heap_flags),
                static_cast<unsigned>(initial_state), description->MipLevels,
                description->SampleDesc.Count, static_cast<unsigned>(result),
                resource && *resource
                    ? reinterpret_cast<std::uintptr_t>(*resource)
                    : std::uintptr_t{});
            log_layout_callstack(L"DX12 texture");
        }
    }
    return result;
}

bool is_sharp_plugin_loader_active() {
    wchar_t executable_path[32768]{};
    const auto executable_length = GetModuleFileNameW(
        nullptr, executable_path,
        static_cast<DWORD>(std::size(executable_path)));
    if (executable_length == 0 ||
        executable_length == std::size(executable_path)) {
        return false;
    }

    const auto game_directory =
        std::filesystem::path(executable_path).parent_path();
    const auto local_winmm = game_directory / L"winmm.dll";
    const auto local_msvcrt = game_directory / L"msvcrt.dll";
    const auto sharp_core = game_directory /
        L"nativePC/plugins/CSharp/Loader/SharpPluginLoader.Core.dll";
    std::error_code error;
    const auto core_exists = std::filesystem::exists(sharp_core, error);
    error.clear();
    const auto windows_proxy_exists =
        std::filesystem::exists(local_winmm, error);
    error.clear();
    const auto linux_proxy_exists =
        std::filesystem::exists(local_msvcrt, error);
    return core_exists && (windows_proxy_exists || linux_proxy_exists);
}

DWORD WINAPI native_aspect_worker(void*) {
    // SharpPluginLoader creates its DX12 renderer well after the render-manager
    // object first becomes visible. Wait for real output dimensions so the
    // game's later renderer initialization cannot overwrite our request.
    for (unsigned attempt = 0; attempt < 1200; ++attempt) {
        const auto module =
            reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        auto* manager = module
                            ? *reinterpret_cast<void**>(
                                  module + kRenderManagerGlobalRva)
                            : nullptr;
        if (!manager) {
            Sleep(100);
            continue;
        }
        const auto address = reinterpret_cast<std::uintptr_t>(manager);
        const auto output = *reinterpret_cast<const std::uint64_t*>(
            address + kOutputDimensionsOffset);
        if (static_cast<std::uint32_t>(output) == 0 ||
            static_cast<std::uint32_t>(output >> 32) == 0) {
            Sleep(100);
            continue;
        }

        const auto requested = *reinterpret_cast<const std::int32_t*>(
            address + kRequestedModeOffset);
        if (requested != 0) {
            // Renderer initialization and some loader startup stages can
            // restore the forced 16:9 mode. Re-arm the one-shot setter only
            // when that state actually changes back.
            g_request_attempted.store(false, std::memory_order_release);
            g_result_logged.store(false, std::memory_order_release);
            request_native_aspect_mode();
            log_native_aspect_result();
        } else {
            // Re-assert for the whole startup window: the game may rebuild
            // layout state after the first write, and having the value in
            // place before the completion screen is built costs nothing.
            apply_layout_overrides(address);
            apply_ui_16x9_split(address);
        }
        Sleep(100);
    }
    Logger::instance().write(L"Native aspect startup monitor completed");
    // SharpPluginLoader does not take the Present hook. Settlement and the
    // equipment shop are entered long after startup, and those screens copy
    // the layout fields again, so keep the 16:9 UI split in place.
    if (g_keep_ui_16x9) {
        for (;;) {
            const auto module =
                reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            auto* manager = module
                                ? *reinterpret_cast<void**>(
                                      module + kRenderManagerGlobalRva)
                                : nullptr;
            if (manager) {
                apply_ui_16x9_split(reinterpret_cast<std::uintptr_t>(manager));
            }
            Sleep(100);
        }
    }
    return 0;
}

bool replace_vtable_entry(void** entry, void* replacement, void** original) {
    DWORD old_protection{};
    if (!VirtualProtect(entry, sizeof(*entry), PAGE_EXECUTE_READWRITE,
                        &old_protection)) {
        return false;
    }
    *original = *entry;
    *entry = replacement;
    DWORD ignored{};
    const auto restored =
        VirtualProtect(entry, sizeof(*entry), old_protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), entry, sizeof(*entry));
    return restored != FALSE;
}

void apply_layout_overrides(std::uintptr_t manager_address) {
    if (g_ui_reference_width == 0 && g_ui_reference_height == 0 &&
        g_secondary_layout_width == 0 && g_secondary_layout_height == 0) {
        return;
    }

    struct Override {
        const wchar_t* name;
        std::uintptr_t offset;
        std::uint32_t value;
    };
    const Override overrides[]{
        {L"UiReferenceWidth", kUiReferenceWidthOffset, g_ui_reference_width},
        {L"UiReferenceHeight", kUiReferenceHeightOffset, g_ui_reference_height},
        {L"SecondaryLayoutWidth", kSecondaryLayoutWidthOffset,
         g_secondary_layout_width},
        {L"SecondaryLayoutHeight", kSecondaryLayoutHeightOffset,
         g_secondary_layout_height},
    };

    const auto announce = !g_layout_override_logged.exchange(true);
    for (const auto& entry : overrides) {
        if (entry.value == 0) {
            continue;
        }
        auto* field =
            reinterpret_cast<std::uint32_t*>(manager_address + entry.offset);
        const auto before = *field;
        if (before == entry.value) {
            continue;
        }
        // Sanity gate: only touch fields that currently hold a plausible
        // resolution component, so a wrong offset on an unknown build cannot
        // corrupt unrelated state.
        if (before < 240 || before > 16384) {
            if (announce) {
                Logger::instance().write(
                    L"Layout override skipped: {} at +0x{:X} holds {}, "
                    L"outside the plausible range",
                    entry.name, entry.offset, before);
            }
            continue;
        }
        *field = entry.value;
        if (announce) {
            Logger::instance().write(
                L"Layout override applied: {} at +0x{:X}: {} -> {}",
                entry.name, entry.offset, before, *field);
        }
    }
}

std::uint32_t fitted_16x9_height(std::uint32_t width) {
    return (width * 9u + 8u) / 16u;
}

// Mode 0 leaves the 3D content size and the present rectangle at the full
// output, which is the uncropped scene. Menu code still has a second size:
// render-manager +0x210 is the 16:9 pair, selected when options+0x2CDA is 0.
// +0x1A0 is a renderer parameter registered at +0x2294582 and stays at the
// constructor's 1920x1080, which crops the settlement background. Point that
// parameter at the full output, and point the menu pair at the 16:9 fit.
void apply_ui_16x9_split(std::uintptr_t manager_address) {
    if (!g_keep_ui_16x9 || !g_remove_letterbox) {
        return;
    }
    const auto output_width = *reinterpret_cast<const std::uint32_t*>(
        manager_address + kOutputDimensionsOffset);
    const auto output_height = *reinterpret_cast<const std::uint32_t*>(
        manager_address + kOutputDimensionsOffset + 4);
    if (output_width < 16 || output_height < 9) {
        return;
    }
    const auto ui_height = fitted_16x9_height(output_width);
    // Wider than 16:9 (21:9) is a horizontal fit. This split only restores
    // the vertical 16:9 menu canvas used by 16:10 and similar displays.
    if (ui_height + 1 >= output_height) {
        return;
    }

    auto* background_width = reinterpret_cast<std::uint32_t*>(
        manager_address + kUiReferenceWidthOffset);
    auto* background_height = reinterpret_cast<std::uint32_t*>(
        manager_address + kUiReferenceHeightOffset);
    auto* ui_width = reinterpret_cast<std::uint32_t*>(
        manager_address + kSecondaryLayoutWidthOffset);
    auto* ui_height_field = reinterpret_cast<std::uint32_t*>(
        manager_address + kSecondaryLayoutHeightOffset);
    const auto background_changed =
        *background_width != output_width || *background_height != output_height;
    const auto ui_changed =
        *ui_width != output_width || *ui_height_field != ui_height;
    if (background_changed) {
        *background_width = output_width;
        *background_height = output_height;
    }
    if (ui_changed) {
        *ui_width = output_width;
        *ui_height_field = ui_height;
    }

    const auto module =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* options = module
                        ? *reinterpret_cast<std::uint8_t**>(
                              module + kOptionsGlobalRva)
                        : nullptr;
    auto flag_changed = false;
    if (options) {
        auto* flag = options + kUiUsesNativeSizeFlag;
        if (*flag != 0) {
            *flag = 0;
            flag_changed = true;
        }
    }

    if ((background_changed || ui_changed || flag_changed) &&
        !g_ui_16x9_logged.exchange(true)) {
        Logger::instance().write(
            L"UI kept at 16:9: background={}x{}, ui={}x{}, output={}x{}",
            output_width, output_height, output_width, ui_height,
            output_width, output_height);
    }
}

void request_native_aspect_mode() {
    if (!g_remove_letterbox || g_request_attempted.load()) {
        return;
    }

    const auto module =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!module) {
        Logger::instance().write(
            L"Native aspect mode aborted: executable module unavailable");
        return;
    }

    constexpr std::uint8_t expected_setter[]{
        0x89, 0x91, 0x40, 0xB4, 0x07, 0x00};
    const auto* setter_bytes =
        reinterpret_cast<const std::uint8_t*>(module + kAspectModeSetterRva);
    if (std::memcmp(setter_bytes, expected_setter,
                    sizeof(expected_setter)) != 0) {
        Logger::instance().write(
            L"Native aspect mode aborted: unsupported executable "
            L"(setter bytes mismatch at +0x{:X})",
            kAspectModeSetterRva);
        return;
    }

    auto* manager = *reinterpret_cast<void**>(
        module + kRenderManagerGlobalRva);
    if (!manager) {
        return;
    }
    if (g_request_attempted.exchange(true)) {
        return;
    }

    const auto address = reinterpret_cast<std::uintptr_t>(manager);
    const auto active_before =
        *reinterpret_cast<const std::int32_t*>(address + kActiveModeOffset);
    const auto requested_before =
        *reinterpret_cast<const std::int32_t*>(
            address + kRequestedModeOffset);
    const auto width_before =
        *reinterpret_cast<const std::uint32_t*>(
            address + kContentWidthOffset);
    const auto height_before =
        *reinterpret_cast<const std::uint32_t*>(
            address + kContentHeightOffset);

    const auto setter = reinterpret_cast<SetAspectModeFn>(
        module + kAspectModeSetterRva);
    setter(manager, 0);

    const auto requested_after =
        *reinterpret_cast<const std::int32_t*>(
            address + kRequestedModeOffset);
    Logger::instance().write(
        L"Native aspect mode requested: active={}, requested {} -> {}, "
        L"content={}x{}",
        active_before, requested_before, requested_after, width_before,
        height_before);

    apply_layout_overrides(address);
}

void log_native_aspect_result() {
    if (!g_request_attempted.load() || g_result_logged.load()) {
        return;
    }

    const auto module =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* manager = module
                        ? *reinterpret_cast<void**>(
                              module + kRenderManagerGlobalRva)
                        : nullptr;
    if (!manager) {
        return;
    }

    const auto address = reinterpret_cast<std::uintptr_t>(manager);
    const auto active =
        *reinterpret_cast<const std::int32_t*>(address + kActiveModeOffset);
    const auto requested =
        *reinterpret_cast<const std::int32_t*>(
            address + kRequestedModeOffset);
    if (active != 0 || requested != 0) {
        return;
    }

    if (g_result_logged.exchange(true)) {
        return;
    }
    const auto width =
        *reinterpret_cast<const std::uint32_t*>(
            address + kContentWidthOffset);
    const auto height =
        *reinterpret_cast<const std::uint32_t*>(
            address + kContentHeightOffset);
    const auto output =
        *reinterpret_cast<const std::uint64_t*>(
            address + kOutputDimensionsOffset);
    Logger::instance().write(
        L"Native aspect mode applied: mode=0, content={}x{}, output={}x{}",
        width, height, static_cast<std::uint32_t>(output),
        static_cast<std::uint32_t>(output >> 32));
}

HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* swapchain,
                                       UINT sync_interval, UINT flags) {
    // Updating the game's aspect state can cause a nested Present when
    // another loader also hooks DX12 presentation. Avoid recursively applying
    // the game-side update; the nested call can continue to the original.
    thread_local bool applying_native_aspect = false;
    if (!applying_native_aspect) {
        applying_native_aspect = true;
        request_native_aspect_mode();
        log_native_aspect_result();
        const auto module =
            reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        auto* manager = module
                            ? *reinterpret_cast<void**>(
                                  module + kRenderManagerGlobalRva)
                            : nullptr;
        if (manager) {
            apply_ui_16x9_split(reinterpret_cast<std::uintptr_t>(manager));
        }
        applying_native_aspect = false;
    }
    return g_original_present(swapchain, sync_interval, flags);
}

bool hook_swapchain_present(IDXGISwapChain* swapchain) {
    if (!swapchain) {
        return false;
    }

    const std::scoped_lock lock(g_present_hook_mutex);
    if (g_present_hook_installed.load(std::memory_order_acquire)) {
        return true;
    }

    auto** vtable = *reinterpret_cast<void***>(swapchain);
    if (vtable[8] == reinterpret_cast<void*>(&present_hook)) {
        return g_original_present != nullptr;
    }
    void* original{};
    if (!replace_vtable_entry(&vtable[8], reinterpret_cast<void*>(&present_hook),
                              &original)) {
        return false;
    }
    if (original == reinterpret_cast<void*>(&present_hook)) {
        return false;
    }
    g_original_present = reinterpret_cast<PresentFn>(original);
    g_present_hook_installed.store(true, std::memory_order_release);
    return true;
}

HRESULT STDMETHODCALLTYPE create_swapchain_hook(
    IDXGIFactory* factory, IUnknown* device,
    DXGI_SWAP_CHAIN_DESC* description, IDXGISwapChain** swapchain) {
    const auto result = g_original_create_swapchain(
        factory, device, description, swapchain);
    if (SUCCEEDED(result) && swapchain && *swapchain) {
        const auto installed = hook_swapchain_present(*swapchain);
        Logger::instance().write(
            L"DX12 swap chain captured through CreateSwapChain; Present hook: {}",
            installed ? L"installed" : L"failed");
    }
    return result;
}

HRESULT STDMETHODCALLTYPE create_swapchain_for_hwnd_hook(
    IDXGIFactory2* factory, IUnknown* device, HWND window,
    const DXGI_SWAP_CHAIN_DESC1* description,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
    IDXGIOutput* output, IDXGISwapChain1** swapchain) {
    const auto result = g_original_create_swapchain_for_hwnd(
        factory, device, window, description, fullscreen, output, swapchain);
    if (SUCCEEDED(result) && swapchain && *swapchain) {
        const auto installed = hook_swapchain_present(*swapchain);
        Logger::instance().write(
            L"DX12 swap chain captured through CreateSwapChainForHwnd; Present hook: {}",
            installed ? L"installed" : L"failed");
    }
    return result;
}

bool install_dx12_layout_trace(Logger& log, const Config& config) {
    g_trace_output_width = config.width;
    g_trace_output_height = config.height;
    g_promote_widescreen_textures = config.promote_widescreen_textures;
    g_trace_started_ticks.store(GetTickCount64());

    ID3D12Device* device{};
    ID3D12CommandAllocator* allocator{};
    ID3D12GraphicsCommandList* command_list{};
    auto result = D3D12CreateDevice(
        nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (SUCCEEDED(result) && device) {
        result = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    }
    if (SUCCEEDED(result) && allocator) {
        result = device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
            IID_PPV_ARGS(&command_list));
    }

    bool viewport_installed{};
    bool scissor_installed{};
    bool resource_installed{};
    bool rtv_create_installed{};
    bool copy_texture_installed{};
    bool copy_resource_installed{};
    bool resolve_installed{};
    bool render_targets_installed{};
    bool draws_installed{};
    if (SUCCEEDED(result) && device) {
        auto** device_vtable = *reinterpret_cast<void***>(device);
        void* original_create_resource{};
        void* original_create_rtv{};
        resource_installed = replace_vtable_entry(
            &device_vtable[27],
            reinterpret_cast<void*>(&create_committed_resource_hook),
            &original_create_resource);
        if (resource_installed) {
            g_original_create_committed_resource =
                reinterpret_cast<CreateCommittedResourceFn>(
                    original_create_resource);
        }
        rtv_create_installed = replace_vtable_entry(
            &device_vtable[20],
            reinterpret_cast<void*>(&create_render_target_view_hook),
            &original_create_rtv);
        if (rtv_create_installed) {
            g_original_create_render_target_view =
                reinterpret_cast<CreateRenderTargetViewFn>(original_create_rtv);
            g_rtv_descriptor_increment = device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        }
    }
    if (SUCCEEDED(result) && command_list) {
        auto** vtable = *reinterpret_cast<void***>(command_list);
        void* original_copy_texture{};
        void* original_copy_resource{};
        void* original_resolve{};
        void* original_render_targets{};
        void* original_draws{};
        void* original_viewports{};
        void* original_scissors{};
        copy_texture_installed = replace_vtable_entry(
            &vtable[16], reinterpret_cast<void*>(&copy_texture_region_hook),
            &original_copy_texture);
        copy_resource_installed = replace_vtable_entry(
            &vtable[17], reinterpret_cast<void*>(&copy_resource_hook),
            &original_copy_resource);
        resolve_installed = replace_vtable_entry(
            &vtable[19], reinterpret_cast<void*>(&resolve_subresource_hook),
            &original_resolve);
        render_targets_installed = replace_vtable_entry(
            &vtable[46], reinterpret_cast<void*>(&om_set_render_targets_hook),
            &original_render_targets);
        draws_installed = replace_vtable_entry(
            &vtable[13], reinterpret_cast<void*>(&draw_indexed_instanced_hook),
            &original_draws);
        viewport_installed = replace_vtable_entry(
            &vtable[21], reinterpret_cast<void*>(&rs_set_viewports_hook),
            &original_viewports);
        scissor_installed = replace_vtable_entry(
            &vtable[22], reinterpret_cast<void*>(&rs_set_scissor_rects_hook),
            &original_scissors);
        if (copy_texture_installed) {
            g_original_copy_texture_region =
                reinterpret_cast<CopyTextureRegionFn>(original_copy_texture);
        }
        if (copy_resource_installed) {
            g_original_copy_resource =
                reinterpret_cast<CopyResourceFn>(original_copy_resource);
        }
        if (resolve_installed) {
            g_original_resolve_subresource =
                reinterpret_cast<ResolveSubresourceFn>(original_resolve);
        }
        if (render_targets_installed) {
            g_original_om_set_render_targets =
                reinterpret_cast<OMSetRenderTargetsFn>(original_render_targets);
        }
        if (draws_installed) {
            g_original_draw_indexed_instanced =
                reinterpret_cast<DrawIndexedInstancedFn>(original_draws);
        }
        if (viewport_installed) {
            g_original_rs_set_viewports =
                reinterpret_cast<RSSetViewportsFn>(original_viewports);
        }
        if (scissor_installed) {
            g_original_rs_set_scissor_rects =
                reinterpret_cast<RSSetScissorRectsFn>(original_scissors);
        }
    }

    if (command_list) {
        command_list->Release();
    }
    if (allocator) {
        allocator->Release();
    }
    if (device) {
        device->Release();
    }

    log.write(L"DX12 completion layout trace: viewport={}, scissor={}, resource={}, "
              L"rtvCreate={}, renderTargets={}, copyTexture={}, copyResource={}, "
              L"resolve={}, draws={} "
              L"(probe result=0x{:08X})",
              viewport_installed ? L"hooked" : L"failed",
              scissor_installed ? L"hooked" : L"failed",
              resource_installed ? L"hooked" : L"failed",
              rtv_create_installed ? L"hooked" : L"failed",
              render_targets_installed ? L"hooked" : L"failed",
              copy_texture_installed ? L"hooked" : L"failed",
              copy_resource_installed ? L"hooked" : L"failed",
              resolve_installed ? L"hooked" : L"failed",
              draws_installed ? L"hooked" : L"failed",
              static_cast<unsigned>(result));
    return viewport_installed && scissor_installed && resource_installed &&
        rtv_create_installed && render_targets_installed &&
        copy_texture_installed && copy_resource_installed && resolve_installed &&
        draws_installed;
}

bool install_dx12_swapchain_capture(Logger& log) {
    IDXGIFactory2* factory{};
    const auto result = CreateDXGIFactory1(
        __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
    if (FAILED(result) || !factory) {
        log.write(L"DX12 swap-chain capture failed: CreateDXGIFactory1 "
                  L"returned 0x{:08X}", static_cast<unsigned>(result));
        return false;
    }

    auto** vtable = *reinterpret_cast<void***>(factory);
    void* original_create{};
    void* original_create_for_hwnd{};
    const auto legacy_installed = replace_vtable_entry(
        &vtable[10], reinterpret_cast<void*>(&create_swapchain_hook),
        &original_create);
    if (legacy_installed) {
        g_original_create_swapchain =
            reinterpret_cast<CreateSwapChainFn>(original_create);
    }
    const auto hwnd_installed = replace_vtable_entry(
        &vtable[15], reinterpret_cast<void*>(&create_swapchain_for_hwnd_hook),
        &original_create_for_hwnd);
    if (hwnd_installed) {
        g_original_create_swapchain_for_hwnd =
            reinterpret_cast<CreateSwapChainForHwndFn>(
                original_create_for_hwnd);
    }
    log.write(L"DX12 swap-chain capture: CreateSwapChain={}, "
              L"CreateSwapChainForHwnd={}",
              legacy_installed ? L"hooked" : L"failed",
              hwnd_installed ? L"hooked" : L"failed");

    ID3D12Device* device{};
    ID3D12CommandQueue* queue{};
    IDXGISwapChain* swapchain{};
    const auto dummy_window = CreateWindowExW(
        0, L"STATIC", L"MHW16x10Fix DX12 probe", WS_POPUP,
        0, 0, 2, 2, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    auto dummy_result = D3D12CreateDevice(
        nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (!dummy_window) {
        dummy_result = HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(dummy_result) && device) {
        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        dummy_result = device->CreateCommandQueue(
            &queue_description, IID_PPV_ARGS(&queue));
    }
    if (SUCCEEDED(dummy_result) && queue && legacy_installed) {
        DXGI_SWAP_CHAIN_DESC description{};
        description.BufferDesc.Width = 2;
        description.BufferDesc.Height = 2;
        description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.OutputWindow = dummy_window;
        description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        dummy_result = factory->CreateSwapChain(
            queue, &description, &swapchain);
    }

    const auto dummy_hooked =
        SUCCEEDED(dummy_result) && swapchain &&
        g_present_hook_installed.load(std::memory_order_acquire);
    log.write(L"DX12 dummy swap chain Present hook: {} (result=0x{:08X})",
              dummy_hooked ? L"installed" : L"failed",
              static_cast<unsigned>(dummy_result));
    if (swapchain) {
        swapchain->Release();
    }
    if (queue) {
        queue->Release();
    }
    if (device) {
        device->Release();
    }
    if (dummy_window) {
        DestroyWindow(dummy_window);
    }
    factory->Release();
    return dummy_hooked;
}

}  // namespace

bool install_native_aspect_fix(Logger& log, const Config& config) {
    g_remove_letterbox = config.remove_letterbox;
    g_keep_ui_16x9 = config.keep_ui_16x9;
    if (g_keep_ui_16x9) {
        log.write(L"UI 16:9 split armed");
    }
    g_ui_reference_width = config.ui_reference_width;
    g_ui_reference_height = config.ui_reference_height;
    g_secondary_layout_width = config.secondary_layout_width;
    g_secondary_layout_height = config.secondary_layout_height;

    if (g_ui_reference_width != 0 || g_ui_reference_height != 0 ||
        g_secondary_layout_width != 0 || g_secondary_layout_height != 0) {
        log.write(L"Experimental layout overrides armed: uiRef={}x{}, "
                  L"secondary={}x{} (0 = leave the game's value alone)",
                  g_ui_reference_width, g_ui_reference_height,
                  g_secondary_layout_width, g_secondary_layout_height);
    }

    if (config.game_dx12) {
        // The promotion hook lives inside the layout-trace installer, so it has
        // to run when either feature is requested. Gating it on the trace flag
        // alone would silently do nothing when only the override is enabled.
        if (config.trace_completion_layout ||
            config.promote_widescreen_textures) {
            install_dx12_layout_trace(log, config);
        }
        if (is_sharp_plugin_loader_active()) {
            log.write(L"SharpPluginLoader detected; using the DX12 "
                      L"native-aspect worker instead of a Present hook");
            const auto thread = CreateThread(
                nullptr, 0, native_aspect_worker, nullptr, 0, nullptr);
            if (!thread) {
                log.write(L"Native aspect worker creation failed: {}",
                          GetLastError());
                return false;
            }
            CloseHandle(thread);
            return true;
        }
        return install_dx12_swapchain_capture(log);
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
    D3D_FEATURE_LEVEL obtained{};
    IDXGISwapChain* swapchain{};
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    const auto result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &requested, 1,
        D3D11_SDK_VERSION, &description, &swapchain, &device, &obtained,
        &context);
    if (FAILED(result) || !swapchain) {
        log.write(L"DX11 Present hook failed: dummy swap chain creation "
                  L"returned 0x{:08X}",
                  static_cast<unsigned>(result));
        if (context) {
            context->Release();
        }
        if (device) {
            device->Release();
        }
        if (swapchain) {
            swapchain->Release();
        }
        return false;
    }

    const auto installed = hook_swapchain_present(swapchain);
    if (context) {
        context->Release();
    }
    if (device) {
        device->Release();
    }
    swapchain->Release();

    log.write(L"DX11 Present hook: {}",
              installed ? L"installed" : L"failed");
    return installed;
}

}  // namespace mhw
