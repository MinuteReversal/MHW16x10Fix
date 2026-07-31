#include "fix.hpp"

#include "dinput8_proxy.hpp"
#include "native_aspect_fix.hpp"
#include "log.hpp"

#include <array>

namespace mhw {
namespace {

bool is_expected_process() {
    std::array<wchar_t, MAX_PATH> path{};
    const auto length =
        GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length == path.size()) {
        return false;
    }
    return std::filesystem::path(path.data()).filename() ==
           L"MonsterHunterWorld.exe";
}

}  // namespace

std::filesystem::path module_directory(HMODULE module) {
    std::vector<wchar_t> path(32768);
    const auto length =
        GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length == path.size()) {
        return {};
    }
    return std::filesystem::path(path.data(), path.data() + length).parent_path();
}

void initialize_fix(HMODULE self) {
    const auto directory = module_directory(self);
    if (directory.empty()) {
        return;
    }

    try {
        const auto config = Config::load(directory / L"mhw_16x10.ini");
        auto& log = Logger::instance();
        log.open(directory / L"MHW16x10Fix.log", config.enable_log);
        log.write(L"[{}] Version {}", kName, kVersion);

        if (!is_expected_process()) {
            log.write(L"Unexpected host process; no patches were applied");
            return;
        }
        initialize_dinput8_proxy(
            self, directory, config.chain_load, log);
        if (!config.enabled) {
            log.write(L"Disabled in configuration");
            return;
        }
        log.write(L"{} resolution: {}x{}",
                  config.resolution_detected ? L"Detected" : L"Configured",
                  config.width, config.height);
        log.write(L"Aspect ratio: {:.6f}", config.aspect());

        log.write(L"Graphics API: {}",
                  config.game_dx12 ? L"DX12" : L"DX11");
        if (!install_native_aspect_fix(log, config)) {
            log.write(L"{} native-aspect hook was not installed",
                      config.game_dx12 ? L"DX12" : L"DX11");
            return;
        }
        log.write(L"Native-aspect initialization completed");
    } catch (...) {
        const auto fallback = directory / L"MHW16x10Fix.error.log";
        if (const auto file = CreateFileW(
                fallback.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            file != INVALID_HANDLE_VALUE) {
            constexpr char message[] =
                "MHW16x10Fix initialization failed with an exception.\r\n";
            DWORD written{};
            WriteFile(file, message, sizeof(message) - 1, &written, nullptr);
            CloseHandle(file);
        }
    }
}

}  // namespace mhw
