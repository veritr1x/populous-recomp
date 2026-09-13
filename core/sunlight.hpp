#pragma once
#include "terrain.hpp"
#include <array>
#include <functional>

namespace pop {
// Native values; original packed record is 61 bytes, not its native sizeof.
struct LocalLight {
    std::uint8_t enabled{}, field_1{}, strength{}, flicker{};
    MapPosition position{};
    std::uint16_t altitude{}, entity{};
    std::array<std::uint8_t, 49> contributions{};
};
struct LightEntity {
    std::uint16_t id{};
    MapPosition position{};
    std::uint16_t altitude{};
    std::uint32_t flags{};
};
struct SunlightUpdate {
    std::uint8_t features{}; // original 0x895da5: bit 3 cycle, bit 4 local lights
    std::uint32_t tick{}, random_seed{};
    MapPosition viewer{};
    std::function<LightEntity(std::uint16_t)> entity;
    // Original 004bdd40 whole-map brightness/texture refresh. Backend owns it.
    std::function<void()> refresh_terrain;
};
class Sunlight {
  public:
    TerrainLight direction{};
    std::uint8_t ambient{28}, intensity{15}, phase{32};
    std::array<std::uint8_t, 1024> directional{}, underside{};
    std::array<LocalLight, 50> lights{};
    std::int16_t active{};

    void initialize();      // 00401040
    void reset_direction(); // 00401090
    // Sunlight portion of 00484a10. Older files reset all light state; modern
    // files set phase/direction and retain ambient, intensity and local lights.
    void load_level_parameters(Terrain &, const Level &);
    void rebuild_tables();                    // 00401790
    void remove(Terrain &, std::size_t slot); // 00401140
    void clear_contributions(Terrain &);      // 00401230; lights stay enabled
    void apply(Terrain &, TerrainCoordinate, std::size_t slot, std::size_t sample,
               std::int16_t noise);                 // 004015f0
    void update(Terrain &, const SunlightUpdate &); // 00401350
    bool add(Terrain &, LightEntity &, std::uint8_t strength, std::uint8_t flicker,
             std::uint8_t field_1, const SunlightUpdate &); // 004010b0
    void remove_entity(Terrain &, const LightEntity &);     // 004ee190; entity flag retained
  private:
    void subtract(Terrain &, LocalLight &);
};
} // namespace pop
