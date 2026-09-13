#pragma once
#include "math.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pop {
struct LevelObject {
    std::uint16_t slot;
    std::uint8_t model, kind;
    std::int8_t owner;
    MapPosition position;
    // Preserve the original record, including fields whose semantics are not recovered.
    std::array<std::uint8_t, 55> raw;
};

struct Level {
    static constexpr std::size_t side = 128, cells = side * side;
    std::uint8_t version{};
    std::array<std::int16_t, cells> heights{};
    std::array<std::uint8_t, cells> no_access{};
    std::array<MapPosition, 4> player_positions{};
    std::uint8_t sunlight{};
    std::vector<LevelObject> objects;
    std::vector<std::uint8_t> original_bytes;
    std::int16_t height_at(int x, int z) const;
};

// File decoding portion of D3DPopTB.exe 0x00484a10 / 0x0049a8f0.
// Does not yet perform terrain preprocessing, entity allocation or script startup.
Level decode_level(std::span<const std::uint8_t> dat, std::span<const std::uint8_t> ver);
} // namespace pop
