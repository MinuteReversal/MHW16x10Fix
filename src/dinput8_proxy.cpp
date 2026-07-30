#include "common.hpp"

#include <unknwn.h>

#include <array>
#include <mutex>

namespace {

using DirectInput8CreateFn =
    HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);

HMODULE g_system_dinput8{};
DirectInput8CreateFn g_direct_input8_create{};
std::once_flag g_load_once;

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
        g_direct_input8_create = reinterpret_cast<DirectInput8CreateFn>(
            GetProcAddress(g_system_dinput8, "DirectInput8Create"));
    }
}

}  // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE instance, DWORD version, REFIID iid, LPVOID* output,
    IUnknown* outer) {
    std::call_once(g_load_once, load_system_dinput8);
    if (!g_direct_input8_create) {
        return HRESULT_FROM_WIN32(ERROR_DLL_INIT_FAILED);
    }
    return g_direct_input8_create(instance, version, iid, output, outer);
}
