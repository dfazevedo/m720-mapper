#include "protocol.hpp"

using namespace m720::hidpp;

constexpr bool button_sequence() {
    Buttons buttons(0x0B);
    using Presses = std::array<Cid, targets.size()>;
    if (buttons.update(Packet{0x0B, 0, 0, 0x56, 0, 0x53}) != Presses{forward, back, 0}) return false;
    if (buttons.update(Packet{0x0B, 0, 0, 0x53}) != Presses{}) return false;
    if (buttons.update(Packet{0x0A, 0}) != Presses{}) return false;
    if (buttons.update(Packet{0x0B, 1}) != Presses{}) return false;
    if (buttons.update(Packet{0x0B, 0, 0, 0x53, 0, 0x52}) != Presses{}) return false;
    if (buttons.update(Packet{0x0B, 0, 0, 0xD0, 0, 0xD0}) != Presses{thumb, 0, 0}) return false;
    if (buttons.update(Packet{0x0B, 0}) != Presses{}) return false;
    return buttons.update(Packet{0x0B, 0, 0, 0xD0}) == Presses{thumb, 0, 0};
}

int main() {
    constexpr auto ping = request(0, 1, 0x0D, std::array<Byte, 3>{0, 0, 0x6C});
    static_assert(ping == Packet{0, 0x1D, 0, 0, 0x6C});
    static_assert(request(0, 0, 1, std::array<Byte, 2>{0x1B, 4}) == Packet{0, 1, 0x1B, 4});
    static_assert(classify(Packet{0, 0x1D, 4, 5, 0x6C}, 0, 0x1D) == Reply::success);
    static_assert(classify(Packet{0, 0x1E}, 0, 0x1D) == Reply::unrelated);
    static_assert(classify(Packet{0x0B, 0}, 0x0B, 0x21) == Reply::unrelated);
    static_assert(classify(Packet{0xFF, 0x0B, 0x21, 7}, 0x0B, 0x21) == Reply::device_error);
    constexpr auto state = reporting(Packet{0x0B, 0x21, 0, 0x53, 0x11, 0, 0xD0});
    static_assert(state.cid == back && state.flags == 0x11 && state.remap == thumb);
    static_assert(state.diverted() && state.with_divert(false) == 0x10);
    static_assert(button_sequence());

    ReportingPlan plan(Snapshot{Reporting{back, 0, 0}, Reporting{forward, 1, 0}, Reporting{thumb, 0, 0}});
    if (plan.has_to_restore() || !plan.has_unarmed()) return 1;
    if (plan.next_to_arm().cid != back || plan.next_to_arm().cid != forward) return 2;
    const auto second = plan.next_to_restore();
    if (second.cid != forward || !second.diverted()) return 3;
    const auto first = plan.next_to_restore();
    if (first.cid != back || first.diverted() || plan.has_to_restore()) return 4;
    return 0;
}
