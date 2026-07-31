#include "native_aspect_fix.hpp"

#include "config.hpp"
#include "log.hpp"

#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace mhw {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using SetAspectModeFn = void (*)(void*, std::int32_t);

constexpr std::uintptr_t kRenderManagerGlobalRva = 0x51C4480;
constexpr std::uintptr_t kAspectModeSetterRva = 0x229C790;
constexpr std::uintptr_t kActiveModeOffset = 0x7B43C;
constexpr std::uintptr_t kRequestedModeOffset = 0x7B440;
constexpr std::uintptr_t kContentWidthOffset = 0x198;
constexpr std::uintptr_t kContentHeightOffset = 0x19C;
constexpr std::uintptr_t kOutputDimensionsOffset = 0x1F448;

PresentFn g_original_present{};
std::atomic<bool> g_request_attempted{false};
std::atomic<bool> g_result_logged{false};
bool g_remove_letterbox{};

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
    request_native_aspect_mode();
    log_native_aspect_result();
    return g_original_present(swapchain, sync_interval, flags);
}

}  // namespace

bool install_native_aspect_fix(Logger& log, const Config& config) {
    g_remove_letterbox = config.remove_letterbox;

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

    auto** vtable = *reinterpret_cast<void***>(swapchain);
    const auto installed = replace_vtable_entry(
        &vtable[8], reinterpret_cast<void*>(&present_hook),
        reinterpret_cast<void**>(&g_original_present));
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
