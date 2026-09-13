#pragma once
#include <cstdint>

namespace pop {
struct MapPosition {
    std::uint16_t x, z;
};

// D3DPopTB.exe 0x00401000: signed shortest displacement (a - b).
// At precisely half the 65536-unit world, the original reverses the sign.
std::int32_t wrapped_delta(std::uint32_t a, std::uint32_t b);
// 0x00450450 and 0x004503f0: squared / integer-root toroidal distances.
std::uint32_t squared_distance(MapPosition a, MapPosition b);
std::uint32_t distance(MapPosition a, MapPosition b);
// 0x00586000: integer square root, floor, no floating point rounding.
std::uint32_t integer_sqrt(std::uint32_t value);
// 0x004443f0: signed 16-bit comparison semantics, 512-unit sector.
bool angle_in_quadrant(std::int16_t angle, std::int16_t start);
// 0x00586074: recovered lookup-table heading; game coordinate delta domain.
// Throws for values outside [-65535, 65535]; wider callers remain to be audited.
std::uint16_t heading(std::int32_t dx, std::int32_t dz);
} // namespace pop
