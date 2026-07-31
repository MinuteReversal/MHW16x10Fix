#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mhw {

inline constexpr std::wstring_view kName = L"MHW16x10Fix";
inline constexpr std::wstring_view kVersion =
    L"0.7.6-native-aspect-mode-zero";

std::filesystem::path module_directory(HMODULE module);

}  // namespace mhw
