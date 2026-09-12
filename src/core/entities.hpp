#pragma once
#include "sunlight.hpp"
#include "object_state.hpp"
#include <array>
#include <functional>
#include <vector>

namespace pop {
using EntityId = std::uint16_t; // zero is null, slots 1..1999 are addressable
struct EntityPosition {
    MapPosition horizontal{};
    std::uint16_t altitude{};
};
struct Entity {
    EntityId previous{}, next{}, secondary_next{};
    std::uint32_t flags{}, render_flags{}, motion_flags{}, animation_tick{};
    std::uint16_t animation_2{}, animation_3{};
    EntityId cell_next{}, cell_previous{}, id{};
    std::uint16_t angle{}, field_28{};
    std::uint8_t kind{}, model{}, state{}, state_2{}, class_counter{}, owner{}, index{}, counter{},
        counter_2{};
    ObjectState object{};
    EntityPosition position{}, delta{};
    // Original 0049..00b2 payload: preserved pending class/renderer recovery.
    // This is data storage, never executable pointers or an emulated address space.
    std::array<std::uint8_t, 106> class_data{};
    LightEntity light_entity() const {
        return {id, position.horizontal, position.altitude, flags};
    }
};
class EntityPool {
  public:
    std::vector<Entity> entities = std::vector<Entity>(2000);
    EntityId free_high{}, free_low{}, allocated{}, retiring{}, reserve_free{}, reserve_allocated{};
    std::uint16_t primary_begin{1}, primary_end{1840}, reserve_begin{1840}, reserve_end{2000};
    std::uint32_t primary_count{}, reserve_count{}, low_count{}, reserve_counter{}, allocations{},
        animation_tick{};
    std::array<std::uint8_t, 12> class_counters{};
    std::uint8_t scenery_toggle{}, allocation_flag{}, skip_initialization{};
    std::uint32_t reservation_cursor{}; // native index into 20-byte auxiliary records
    // Must be supplied for recovered classes 1..11 before ordinary allocation.
    // Missing behavior throws; the original skip-initialization mode is separate.
    std::function<void(Entity &)> initialize_class;

    EntityPool();
    Entity &at(EntityId);
    const Entity &at(EntityId) const;
    void reset_ids();     // 004ed880
    void rebuild_lists(); // 004ee300
    EntityId allocate(std::uint8_t kind, std::uint8_t model, std::uint8_t owner,
                      EntityPosition); // 004ed8a0
    EntityId allocate_reserved(std::uint8_t kind, std::uint8_t model, std::uint8_t owner,
                               EntityPosition);                             // 004edbd0
    bool reserve_has_capacity(std::uint8_t kind, std::uint8_t model) const; // 004edae0, AL result
    void insert_cell(Terrain &, EntityId, MapPosition); // 004ee470, does not change entity position
    void remove_cell(Terrain &, EntityId);              // 004ee4f0
    bool move(Terrain &, EntityId, EntityPosition);     // 004ee580
    void retire(Terrain &, Sunlight &, EntityId);       // 004edcf0, deferred primary reuse
    void release_reserved(Terrain &, Sunlight &, EntityId); // 004ee1e0
    void recycle_reserved_link(EntityId);                   // 004ed530, list links only
    void defer_link(EntityId);                              // 00401ba0, list links only
    void recycle_primary_link(EntityId);                    // 00401b40, list links only
    void advance_retirement(); // loop 004ec94c..004ec98b, not the full simulation tick
    void copy_payload(EntityId destination, EntityId source); // 004ede10
  private:
    void unlink(EntityId, EntityId &head);
    void prepend(EntityId, EntityId &head);
    void initialize(Entity &);
    void allocation_failed();
};
} // namespace pop
