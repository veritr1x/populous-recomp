#pragma once
#include "core/entities.hpp"
#include <cstring>
namespace entity_test {
constexpr std::uint32_t base = 0x8e0428, reservations = 0x3100000;
std::uint16_t w(const unsigned char *p) {
    return p[0] | (p[1] << 8);
}
std::uint32_t d(const unsigned char *p) {
    return w(p) | (std::uint32_t(w(p + 2)) << 16);
}
void w(unsigned char *p, std::uint16_t v) {
    p[0] = v;
    p[1] = v >> 8;
}
void d(unsigned char *p, std::uint32_t v) {
    w(p, v);
    w(p + 2, v >> 16);
}
pop::EntityId handle(std::uint32_t p) {
    return p ? static_cast<pop::EntityId>((p - base) / 179) : 0;
}
std::uint32_t pointer(pop::EntityId id) {
    return id ? base + id * 179 : 0;
}
pop::EntityPosition position(const unsigned char *p) {
    return {{w(p), w(p + 2)}, w(p + 4)};
}
void position(unsigned char *p, pop::EntityPosition v) {
    w(p, v.horizontal.x);
    w(p + 2, v.horizontal.z);
    w(p + 4, v.altitude);
}
pop::Entity decode(const unsigned char *p) {
    pop::Entity e;
    e.previous = handle(d(p));
    e.next = handle(d(p + 4));
    e.secondary_next = handle(d(p + 8));
    e.flags = d(p + 12);
    e.render_flags = d(p + 16);
    e.motion_flags = d(p + 20);
    e.animation_tick = d(p + 24);
    e.animation_2 = w(p + 28);
    e.animation_3 = w(p + 30);
    e.cell_next = w(p + 32);
    e.cell_previous = w(p + 34);
    e.id = w(p + 36);
    e.angle = w(p + 38);
    e.field_28 = w(p + 40);
    e.kind = p[42];
    e.model = p[43];
    e.state = p[44];
    e.state_2 = p[45];
    e.class_counter = p[46];
    e.owner = p[47];
    e.index = p[48];
    e.counter = p[49];
    e.counter_2 = p[50];
    e.object = {w(p + 51), w(p + 53), w(p + 55), p[57], p[58], p[59], p[60]};
    e.position = position(p + 61);
    e.delta = position(p + 67);
    std::memcpy(e.class_data.data(), p + 73, 106);
    return e;
}
void encode(const pop::Entity &e, unsigned char *p) {
    d(p, pointer(e.previous));
    d(p + 4, pointer(e.next));
    d(p + 8, pointer(e.secondary_next));
    d(p + 12, e.flags);
    d(p + 16, e.render_flags);
    d(p + 20, e.motion_flags);
    d(p + 24, e.animation_tick);
    w(p + 28, e.animation_2);
    w(p + 30, e.animation_3);
    w(p + 32, e.cell_next);
    w(p + 34, e.cell_previous);
    w(p + 36, e.id);
    w(p + 38, e.angle);
    w(p + 40, e.field_28);
    p[42] = e.kind;
    p[43] = e.model;
    p[44] = e.state;
    p[45] = e.state_2;
    p[46] = e.class_counter;
    p[47] = e.owner;
    p[48] = e.index;
    p[49] = e.counter;
    p[50] = e.counter_2;
    w(p + 51, e.object.index);
    w(p + 53, e.object.flags);
    w(p + 55, e.object.phase);
    p[57] = e.object.frame;
    p[58] = e.object.definition;
    p[59] = e.object.morph;
    p[60] = e.object.palette;
    position(p + 61, e.position);
    position(p + 67, e.delta);
    std::memcpy(p + 73, e.class_data.data(), 106);
}
} // namespace entity_test
