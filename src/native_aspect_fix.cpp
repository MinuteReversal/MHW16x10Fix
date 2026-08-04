#include "native_aspect_fix.hpp"

#include "config.hpp"
#include "log.hpp"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <filesystem>

namespace mhw {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using SetAspectModeFn = void (*)(void*, std::int32_t);

constexpr std::uintptr_t kRenderManagerGlobalRva = 0x51C4480;
constexpr std::uintptr_t kAspectModeSetterRva = 0x229C790;
constexpr std::uintptr_t kActiveModeOffset = 0x7B43C;
constexpr std::uintptr_t kRequestedModeOffset = 0x7B440;
constexpr std::uintptr_t kContentWidthOffset = 0x198;
constexpr std::uintptr_t kContentHeightOffset = 0x19C;
constexpr std::uintptr_t kOutputDimensionsOffset = 0x1F448;

PresentFn g_original_present{};
CreateSwapChainFn g_original_create_swapchain{};
CreateSwapChainForHwndFn g_original_create_swapchain_for_hwnd{};
std::atomic<bool> g_request_attempted{false};
std::atomic<bool> g_result_logged{false};
std::atomic<bool> g_present_hook_installed{false};
std::mutex g_present_hook_mutex;
bool g_remove_letterbox{};

void request_native_aspect_mode();
void log_native_aspect_result();

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
        }
        Sleep(100);
    }
    Logger::instance().write(L"Native aspect startup monitor completed");
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
    // Updating the game's aspect state can cause a nested Present when
    // another loader also hooks DX12 presentation. Avoid recursively applying
    // the game-side update; the nested call can continue to the original.
    thread_local bool applying_native_aspect = false;
    if (!applying_native_aspect) {
        applying_native_aspect = true;
        request_native_aspect_mode();
        log_native_aspect_result();
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

    if (config.game_dx12) {
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
