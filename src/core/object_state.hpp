#pragma once
#include <cstdint>

namespace pop {
struct ObjectState {
    std::uint16_t index{}, flags{}, phase{};
    std::uint8_t frame{}, definition{}, morph{}, palette{};
};
// 004ee700. Definition indices 0..39 are backed by the original static table.
void set_object(ObjectState &, std::uint8_t definition, std::uint16_t index);
void finish_morph(ObjectState &);         // 0040cbd0
bool morph_finished(const ObjectState &); // 0040cc10
} // namespace pop
