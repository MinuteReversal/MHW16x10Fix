#include "dinput8_proxy.hpp"

#include "log.hpp"

#include <unknwn.h>

#include <array>
#include <atomic>
#include <mutex>

namespace {

using DirectInput8CreateFn =
    HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);

HMODULE g_system_dinput8{};
HMODULE g_chain_module{};
DirectInput8CreateFn g_system_direct_input8_create{};
std::atomic<DirectInput8CreateFn> g_chain_direct_input8_create{};
std::once_flag g_system_load_once;
std::once_flag g_chain_load_once;
std::filesystem::path g_chain_path;
DWORD g_chain_error{};

enum class ChainStatus {
    disabled,
    loaded_with_export,
    loaded_without_export,
    self_reference,
    file_not_found,
    load_failed,
};

ChainStatus g_chain_status{ChainStatus::disabled};

void load_system_dinput8() {
    std::array<wchar_t, MAX_PATH> system_directory{};
    const auto length = GetSystemDirectoryW(
        system_directory.data(), static_cast<UINT>(system_directory.size()));
    if (length == 0 || length >= system_directory.size()) {
        return;
    }
    const auto path =
        std::filesystem::path(system_directory.data()) / L"dinput8.dll";
    g_system_dinput8 = LoadLibraryW(path.c_str());
    if (g_system_dinput8) {
        g_system_direct_input8_create =
            reinterpret_cast<DirectInput8CreateFn>(
                GetProcAddress(g_system_dinput8, "DirectInput8Create"));
    }
}

bool same_file(const std::filesystem::path& left,
               const std::filesystem::path& right) {
    std::error_code error;
    const auto left_path = std::filesystem::weakly_canonical(left, error);
    if (error) {
        return false;
    }
    const auto right_path = std::filesystem::weakly_canonical(right, error);
    return !error && left_path == right_path;
}

std::filesystem::path module_path(HMODULE module) {
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(
        module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return {};
    }
    return std::filesystem::path(
        std::wstring_view(path.data(), length));
}

void load_chain(HMODULE self, const std::filesystem::path& directory,
                const std::filesystem::path& configured_path) {
    if (configured_path.empty()) {
        g_chain_status = ChainStatus::disabled;
        return;
    }

    g_chain_path = configured_path.is_absolute()
                       ? configured_path
                       : directory / configured_path;
    const auto self_path = module_path(self);
    if (self_path.empty() || same_file(g_chain_path, self_path)) {
        g_chain_status = ChainStatus::self_reference;
        return;
    }

    std::error_code error;
    if (!std::filesystem::is_regular_file(g_chain_path, error)) {
        g_chain_status = ChainStatus::file_not_found;
        return;
    }

    g_chain_module = LoadLibraryW(g_chain_path.c_str());
    if (!g_chain_module) {
        g_chain_error = GetLastError();
        g_chain_status = ChainStatus::load_failed;
        return;
    }

    const auto chained = reinterpret_cast<DirectInput8CreateFn>(
        GetProcAddress(g_chain_module, "DirectInput8Create"));
    g_chain_direct_input8_create.store(
        chained, std::memory_order_release);
    g_chain_status = chained ? ChainStatus::loaded_with_export
                             : ChainStatus::loaded_without_export;
}

void ensure_chain_loaded(HMODULE self,
                         const std::filesystem::path& directory,
                         const std::filesystem::path& configured_path) {
    std::call_once(g_chain_load_once, load_chain, self, directory,
                   configured_path);
}

void ensure_chain_loaded_from_ini() {
    HMODULE self{};
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ensure_chain_loaded_from_ini),
            &self)) {
        return;
    }

    const auto directory = mhw::module_directory(self);
    std::array<wchar_t, 1024> value{};
    const auto ini = directory / L"mhw_16x10.ini";
    const auto length = GetPrivateProfileStringW(
        L"Loader", L"ChainLoad", L"", value.data(),
        static_cast<DWORD>(value.size()), ini.c_str());
    ensure_chain_loaded(
        self, directory,
        std::filesystem::path(std::wstring_view(value.data(), length)));
}

}  // namespace

namespace mhw {

void initialize_dinput8_proxy(
    HMODULE self, const std::filesystem::path& directory,
    const std::filesystem::path& chain_load, Logger& log) {
    ensure_chain_loaded(self, directory, chain_load);
    switch (g_chain_status) {
        case ChainStatus::disabled:
            log.write(L"Chain loader: disabled");
            break;
        case ChainStatus::loaded_with_export:
            log.write(
                L"Chain loader loaded: {}; DirectInput8Create forwarding=enabled",
                g_chain_path.wstring());
            break;
        case ChainStatus::loaded_without_export:
            log.write(
                L"Chain loader loaded: {}; DirectInput8Create not exported; system forwarding retained",
                g_chain_path.wstring());
            break;
        case ChainStatus::self_reference:
            log.write(L"Chain loader rejected self-reference: {}",
                      g_chain_path.wstring());
            break;
        case ChainStatus::file_not_found:
            log.write(L"Chain loader file not found: {}",
                      g_chain_path.wstring());
            break;
        case ChainStatus::load_failed:
            log.write(L"Chain loader failed: {} (Win32 error {})",
                      g_chain_path.wstring(), g_chain_error);
            break;
    }
}

}  // namespace mhw

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE instance, DWORD version, REFIID iid, LPVOID* output,
    IUnknown* outer) {
    ensure_chain_loaded_from_ini();
    const auto chained =
        g_chain_direct_input8_create.load(std::memory_order_acquire);
    if (chained) {
        return chained(instance, version, iid, output, outer);
    }

    std::call_once(g_system_load_once, load_system_dinput8);
    if (!g_system_direct_input8_create) {
        return HRESULT_FROM_WIN32(ERROR_DLL_INIT_FAILED);
    }
    return g_system_direct_input8_create(
        instance, version, iid, output, outer);
}
