#include "config.hpp"

#include <algorithm>
#include <array>
#include <cwctype>
#include <cstdlib>

namespace mhw {
namespace {

bool read_bool(const std::filesystem::path& file, const wchar_t* section,
               const wchar_t* key, bool fallback) {
    wchar_t value[32]{};
    const auto length = GetPrivateProfileStringW(
        section, key, fallback ? L"true" : L"false", value,
        static_cast<DWORD>(std::size(value)), file.c_str());
    std::wstring normalized(value, value + length);
    normalized.erase(
        std::remove_if(normalized.begin(), normalized.end(),
                       [](wchar_t character) { return std::iswspace(character); }),
        normalized.end());
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](wchar_t character) {
                       return static_cast<wchar_t>(std::towlower(character));
                   });
    if (normalized == L"true" || normalized == L"on" ||
        normalized == L"yes" || normalized == L"1") {
        return true;
    }
    if (normalized == L"false" || normalized == L"off" ||
        normalized == L"no" || normalized == L"0") {
        return false;
    }
    return fallback;
}

std::uint32_t read_uint(const std::filesystem::path& file,
                        const wchar_t* section, const wchar_t* key,
                        std::uint32_t fallback) {
    const auto value = GetPrivateProfileIntW(
        section, key, static_cast<int>(fallback), file.c_str());
    return value > 0 ? static_cast<std::uint32_t>(value) : fallback;
}

std::filesystem::path read_path(const std::filesystem::path& file,
                                const wchar_t* section,
                                const wchar_t* key) {
    std::array<wchar_t, 1024> value{};
    const auto length = GetPrivateProfileStringW(
        section, key, L"", value.data(),
        static_cast<DWORD>(value.size()), file.c_str());
    return std::filesystem::path(
        std::wstring_view(value.data(), length));
}

}  // namespace

float Config::aspect() const noexcept {
    return height == 0 ? 16.0F / 9.0F
                       : static_cast<float>(width) / static_cast<float>(height);
}

Config Config::load(const std::filesystem::path& path) {
    Config result;
    result.enabled = read_bool(path, L"Fix", L"Enabled", result.enabled);
    result.auto_detect_aspect = read_bool(
        path, L"Fix", L"AutoDetectAspect", result.auto_detect_aspect);
    result.width = read_uint(path, L"Fix", L"Width", result.width);
    result.height = read_uint(path, L"Fix", L"Height", result.height);
    result.remove_letterbox =
        read_bool(path, L"Fix", L"RemoveLetterbox", result.remove_letterbox);
    result.enable_log =
        read_bool(path, L"Debug", L"EnableLog", result.enable_log);
    result.chain_load = read_path(path, L"Loader", L"ChainLoad");
    result.forward_chain_direct_input = read_bool(
        path, L"Loader", L"ForwardDirectInput8Create",
        result.forward_chain_direct_input);

    if (result.auto_detect_aspect) {
        const auto graphics_file = path.parent_path() / L"graphics_option.ini";
        wchar_t resolution[64]{};
        GetPrivateProfileStringW(L"GraphicsOption", L"Resolution", L"",
                                 resolution,
                                 static_cast<DWORD>(std::size(resolution)),
                                 graphics_file.c_str());
        wchar_t* separator{};
        const auto width = std::wcstoul(resolution, &separator, 10);
        wchar_t* end{};
        const auto height =
            separator && (*separator == L'x' || *separator == L'X')
                ? std::wcstoul(separator + 1, &end, 10)
                : 0;
        if (width > 0 && height > 0 && end && *end == L'\0') {
            result.width = static_cast<std::uint32_t>(width);
            result.height = static_cast<std::uint32_t>(height);
            result.resolution_detected = true;
        }
        result.game_dx12 = read_bool(
            graphics_file, L"GraphicsOption", L"DirectX12Enable", false);
    }
    return result;
}

}  // namespace mhw
