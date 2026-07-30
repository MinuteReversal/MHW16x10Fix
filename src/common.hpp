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
inline constexpr std::wstring_view kVersion = L"0.4.4-dx11-texture-trace";

std::filesystem::path module_directory(HMODULE module);

}  // namespace mhw
