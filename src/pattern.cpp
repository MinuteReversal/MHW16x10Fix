#include "pattern.hpp"

#include <charconv>
#include <sstream>

namespace mhw {

std::optional<Pattern> Pattern::parse(std::string_view text) {
    std::vector<PatternByte> bytes;
    std::istringstream input{std::string(text)};
    std::string token;
    while (input >> token) {
        if (token == "?" || token == "??") {
            bytes.push_back({0, true});
            continue;
        }
        if (token.size() != 2) {
            return std::nullopt;
        }
        unsigned value{};
        const auto [end, error] =
            std::from_chars(token.data(), token.data() + token.size(), value, 16);
        if (error != std::errc{} || end != token.data() + token.size() ||
            value > 0xFF) {
            return std::nullopt;
        }
        bytes.push_back({static_cast<std::uint8_t>(value), false});
    }
    if (bytes.empty()) {
        return std::nullopt;
    }
    return Pattern(std::move(bytes));
}

std::vector<std::byte*> Pattern::find_all(std::span<std::byte> haystack) const {
    std::vector<std::byte*> matches;
    if (haystack.size() < bytes_.size()) {
        return matches;
    }
    for (std::size_t offset = 0; offset <= haystack.size() - bytes_.size();
         ++offset) {
        bool match = true;
        for (std::size_t index = 0; index < bytes_.size(); ++index) {
            if (!bytes_[index].wildcard &&
                std::to_integer<std::uint8_t>(haystack[offset + index]) !=
                    bytes_[index].value) {
                match = false;
                break;
            }
        }
        if (match) {
            matches.push_back(haystack.data() + offset);
        }
    }
    return matches;
}

std::optional<std::span<std::byte>> executable_image() {
    auto* base = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
    if (!base) {
        return std::nullopt;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return std::nullopt;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return std::nullopt;
    }
    return std::span(base,
                     static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage));
}

}  // namespace mhw

