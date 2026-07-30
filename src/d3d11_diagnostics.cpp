#include "d3d11_diagnostics.hpp"

#include "log.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
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
using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using RSSetScissorRectsFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, const D3D11_RECT*);

PresentFn g_original_present{};
ResizeBuffersFn g_original_resize_buffers{};
CreateTexture2DFn g_original_create_texture_2d{};
RSSetViewportsFn g_original_rs_set_viewports{};
RSSetScissorRectsFn g_original_rs_set_scissor_rects{};
std::atomic<bool> g_swapchain_logged{false};
std::atomic<bool> g_real_context_hook_attempted{false};
std::array<std::atomic<std::uint64_t>, 64> g_logged_viewports{};
std::array<std::atomic<std::uint64_t>, 64> g_logged_scissors{};
std::array<std::atomic<std::uint64_t>, 128> g_logged_target_viewports{};
std::array<std::atomic<std::uint64_t>, 256> g_logged_target_textures{};

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

bool install_d3d11_diagnostics(Logger& log) {
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
