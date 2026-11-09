#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace m720::hidpp {

using Byte = std::uint8_t;
using Cid = std::uint16_t;
using Packet = std::array<Byte, 18>;
inline constexpr Cid back = 0x0053;
inline constexpr Cid forward = 0x0056;
inline constexpr Cid thumb = 0x00D0;
inline constexpr std::array targets{back, forward, thumb};

constexpr Byte address(Byte function, Byte software_id) {
    return static_cast<Byte>((function << 4) | software_id);
}

constexpr Packet request(Byte feature, Byte function, Byte software_id, std::span<const Byte> parameters) {
    Packet packet{feature, address(function, software_id)};
    for (std::size_t i = 0; i < parameters.size() && i < packet.size() - 2; ++i) packet[i + 2] = parameters[i];
    return packet;
}

enum class Reply { unrelated, success, device_error };

constexpr Reply classify(const Packet& packet, Byte feature, Byte function_address) {
    if (packet[0] == 0xFF && packet[1] == feature && packet[2] == function_address) return Reply::device_error;
    if (packet[0] == feature && packet[1] == function_address) return Reply::success;
    return Reply::unrelated;
}

constexpr Cid word(const Packet& packet, std::size_t offset) {
    return static_cast<Cid>((packet[offset] << 8) | packet[offset + 1]);
}

struct Reporting {
    Cid cid{};
    Byte flags{};
    Cid remap{};

    constexpr bool diverted() const { return (flags & 1) != 0; }
    constexpr Byte with_divert(bool on) const { return static_cast<Byte>((flags & 0xFE) | (on ? 1 : 0)); }
};

constexpr Reporting reporting(const Packet& packet) {
    return {word(packet, 2), packet[4], word(packet, 5)};
}

using Snapshot = std::array<Reporting, targets.size()>;

// Mark writes before issuing them. A timeout can still mean firmware applied the request.
class ReportingPlan {
public:
    explicit ReportingPlan(Snapshot baseline) : baseline_(baseline) {}

    bool has_unarmed() const { return attempted_ < baseline_.size(); }
    Reporting next_to_arm() { return baseline_[attempted_++]; }
    bool has_to_restore() const { return attempted_ != 0; }
    Reporting next_to_restore() { return baseline_[--attempted_]; }

private:
    Snapshot baseline_;
    std::size_t attempted_{};
};

class Buttons {
public:
    explicit constexpr Buttons(Byte feature) : feature_(feature) {}

    // Returns newly pressed target CIDs in the order reported by the device, then zero padding.
    constexpr std::array<Cid, targets.size()> update(const Packet& packet) {
        std::array<Cid, targets.size()> presses{};
        if (packet[0] != feature_ || packet[1] != 0) return presses;

        unsigned current = 0;
        std::size_t count = 0;
        for (std::size_t offset = 2; offset < 10; offset += 2) {
            const auto cid = word(packet, offset);
            for (std::size_t i = 0; i < targets.size(); ++i) {
                if (targets[i] != cid) continue;
                const auto bit = 1u << i;
                if (((held_ | current) & bit) == 0) presses[count++] = cid;
                current |= bit;
                break;
            }
        }
        held_ = current;
        return presses;
    }

private:
    Byte feature_;
    unsigned held_{};
};

} // namespace m720::hidpp
