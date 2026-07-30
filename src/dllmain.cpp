#include "fix.hpp"

namespace {

DWORD WINAPI initialize_thread(void* parameter) {
    const auto module = static_cast<HMODULE>(parameter);
    mhw::initialize_fix(module);
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        const auto handle =
            CreateThread(nullptr, 0, initialize_thread, module, 0, nullptr);
        if (handle) {
            CloseHandle(handle);
        }
    }
    return TRUE;
}
