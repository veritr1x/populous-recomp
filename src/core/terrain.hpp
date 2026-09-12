#pragma once
#include "level.hpp"
#include <array>
#include <cstdint>
#include <functional>

namespace pop {
// Runtime cell fields recovered from D3DPopTB.exe 0x008a03e4, stride 16.
// Entity references remain integer handles, never emulated/native pointers.
struct TerrainCell {
    std::uint32_t flags{};
    std::int16_t height{};
    std::uint16_t first_entity{}, second_entity{};
    std::uint8_t cliff{}, field_11{}, surface{}, brightness{}, shadow{}, field_15{};
};
struct TerrainLight {
    std::int16_t x{147}, z{147}, bias{147};
};
// Packed coordinate uses two wrapping bytes; each map cell occupies two units.
struct TerrainCoordinate {
    std::uint8_t x{}, z{};
    static constexpr TerrainCoordinate from_packed(std::uint16_t v) {
        return {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8)};
    }
    constexpr std::uint16_t packed() const {
        return x | (static_cast<std::uint16_t>(z) << 8);
    }
    constexpr TerrainCoordinate offset(int dx, int dz) const {
        return {static_cast<std::uint8_t>(x + dx), static_cast<std::uint8_t>(z + dz)};
    }
    constexpr std::size_t index() const {
        return (z >> 1) * 128u + (x >> 1);
    }
};

class Terrain {
  public:
    std::array<TerrainCell, Level::cells> cells{};
    TerrainLight light{};
    std::uint32_t flags{};
    std::int32_t shoreline_height_limit{384}; // original initial land_const_1
    std::int32_t secondary_slope_limit{210};  // original initial DAT_005aa454
    std::array<std::array<std::uint8_t, 8192>, 2> slope_masks{};
    std::uint32_t visited{}, duplicates{};
    // Renderer receives the original ordered texture update calls after the
    // simulation/light passes. Tile generation lives in terrain_texture.hpp;
    // cache invalidation and renderer integration remain separate work.
    using TextureUpdate = std::function<void(TerrainCoordinate)>;
    TextureUpdate texture_update;

    // 0x0044ddc0: resets queue/mark storage, not terrain or diagnostic counters.
    void reset_updates();
    // 0x0044ddf0: enqueue a square, flushing at capacity. Radius 64 also flushes
    // the tail and repeats the entire square once, as in the original recursion.
    void update_square(TerrainCoordinate center, std::uint16_t radius,
                       std::uint8_t refresh_visuals);
    // 0x0044df40: four ordered passes; renderer work is delegated above.
    void flush();
    // 0x004be230: signed integer sunlight calculation and saturated brightness.
    void update_brightness(TerrainCoordinate point);
    // 0x00422bd0: choose a triangle (or all four corners) from subcell parity
    // and the recovered diagonal flag, then compare its height range.
    bool slope_allowed(TerrainCoordinate point, std::int32_t limit) const;
    // 0x0044e940: triangle-interpolated world height. Original result is AX;
    // upper EAX bits are path-dependent and are not part of this native API.
    std::int16_t height_at(MapPosition position) const;
    // 0x00422a60: update both 256x256 subcell bitmaps, honoring flags 0x80004.
    void update_slope_masks(TerrainCoordinate center, std::uint16_t radius);
    std::size_t pending() const {
        return count_;
    }
    const auto &pending_coordinates() const {
        return coordinates_;
    }
    const auto &pending_visual_flags() const {
        return visual_flags_;
    }
    const auto &marked_cells() const {
        return marked_;
    }
    bool recursion_active() const {
        return recursion_;
    }

  private:
    std::array<TerrainCoordinate, 1024> coordinates_{};
    std::array<std::uint8_t, 1024> visual_flags_{};
    std::array<std::uint8_t, Level::cells> marked_{};
    std::size_t count_{};
    bool recursion_{};
    TerrainCell &at(TerrainCoordinate p) {
        return cells[p.index()];
    }
    const TerrainCell &at(TerrainCoordinate p) const {
        return cells[p.index()];
    }
};

// Terrain preprocessing portion of the level loader. Entity initialization,
// the later texture pass and scenario globals are still separate recovery work.
Terrain prepare_level_terrain(const Level &level);
} // namespace pop
