#pragma once

#include "config.hpp"

namespace mhw {

class Logger;

bool apply_runtime_resolution_patch(const Config& config, Logger& log);

}  // namespace mhw

