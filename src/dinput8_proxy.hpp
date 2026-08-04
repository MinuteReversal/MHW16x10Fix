#pragma once

#include "common.hpp"

namespace mhw {

class Logger;

void initialize_dinput8_proxy(
    HMODULE self, const std::filesystem::path& directory,
    const std::filesystem::path& chain_load,
    bool forward_chain_direct_input, Logger& log);

}  // namespace mhw
