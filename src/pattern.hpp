#pragma once

#include "common.hpp"

namespace mhw {

struct PatternByte {
    std::uint8_t value{};
    bool wildcard{};
};

class Pattern {
public:
    static std::optional<Pattern> parse(std::string_view text);
    [[nodiscard]] std::vector<std::byte*> find_all(
        std::span<std::byte> haystack) const;

private:
    explicit Pattern(std::vector<PatternByte> bytes)
        : bytes_(std::move(bytes)) {}
    std::vector<PatternByte> bytes_;
};

std::optional<std::span<std::byte>> executable_image();

}  // namespace mhw

