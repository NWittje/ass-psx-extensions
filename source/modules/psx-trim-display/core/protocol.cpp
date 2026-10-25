#include "trim/protocol.hpp"
namespace trim {
using namespace psx::hardware;
bool decode_snapshot(Bytes b,Role role,Snapshot& out) noexcept {
    if(b.size()!=12)return false;
    Snapshot s{read16(b,0),read16(b,2),read16(b,4),b[6],Role(b[7]),read32(b,8)};
    if(s.role!=role || (s.validity&~15u)!=0 || (s.indicators&~15u)!=0 || ((s.validity&trim_known)&&s.angle>15000))return false;
    out=s;return true;
}
std::array<std::uint8_t,12> encode_snapshot(const Snapshot& s) noexcept {
    std::array<std::uint8_t,12> b{};write16(b,0,s.angle);write16(b,2,s.indicators);write16(b,4,s.validity);
    b[6]=s.brightness;b[7]=std::uint8_t(s.role);write32(b,8,s.source_generation);return b;
}
}
