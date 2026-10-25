#include <psx/display/tcp.hpp>
#include <algorithm>

namespace psx::du::transport::tcp
{
namespace
{
constexpr uint32_t envelope_magic = 0x314e4944; // DIN1, never DUN1.
template<class T> T get(std::span<const uint8_t> b, size_t offset)
{
    if (offset > b.size() || sizeof(T) > b.size() - offset)
        throw std::runtime_error("DIN1 truncated field");
    T value{}; std::memcpy(&value, b.data() + offset, sizeof value); return value;
}
template<class T> void put(Bytes &b, size_t offset, T value)
{ std::memcpy(b.data() + offset, &value, sizeof value); }
void check(const InstrumentEnvelope &e)
{
    if (e.instrument != psx::instrument::captain_stab_trim || !e.generation || !e.sequence ||
        e.session == Identity{} || e.source.identity.size() > envelope_limit - 128 ||
        uint32_t(e.state) < 1 || uint32_t(e.state) > 6)
        throw std::runtime_error("DIN1 identity");
    if (e.state == State::frame)
    {
        auto d = psx::instrument::decode(e.frame);
        if (d.instrument_id != e.instrument || d.frequency != e.source.clock ||
            !e.source.pid || !e.source.created || !instrument_identity_json(e.source.identity))
            throw std::runtime_error("DIN1 source/frame mismatch");
    }
    else if (!e.frame.empty()) throw std::runtime_error("DIN1 status payload");
}
}
Frame instrument_packet(const InstrumentEnvelope &e, Owned payload, uint64_t revision)
{
    check(e);
    if ((e.state == State::frame && (!payload || e.frame.data() != payload->data() ||
                                    e.frame.size() != payload->size())) ||
        (e.state != State::frame && payload)) throw std::runtime_error("DIN1 ownership");
    auto header = allocate(128 + e.source.identity.size());
    put(*header, 0, envelope_magic); put(*header, 4, uint32_t(1));
    put(*header, 8, uint32_t(header->size())); put(*header, 12, uint32_t(e.frame.size()));
    std::copy(e.session.begin(), e.session.end(), header->begin() + 16);
    put(*header, 32, e.source.pid); put(*header, 36, e.instrument);
    put(*header, 40, e.source.created); put(*header, 48, e.source.clock);
    put(*header, 56, e.generation); put(*header, 64, e.sequence); put(*header, 72, e.offered);
    put(*header, 80, uint32_t(e.state)); put(*header, 84, uint32_t(e.source.identity.size()));
    std::copy(e.source.identity.begin(), e.source.identity.end(), header->begin() + 128);
    auto p = std::make_shared<Packet>(); p->envelope = std::move(header); p->payload = std::move(payload);
    p->token = {e.session, e.generation, e.sequence, p->payload ? psx::display::word(p->payload->data(), 16) : 0};
    p->end = p->payload ? psx::display::word(p->payload->data(), 48) : 0;
    p->revision = revision; p->instrument = true; return p;
}
InstrumentEnvelope decode_instrument_parts(std::span<const uint8_t> h, std::span<const uint8_t> b)
{
    if (h.size() < 128 || h.size() > envelope_limit || get<uint32_t>(h, 0) != envelope_magic ||
        get<uint32_t>(h, 4) != 1 || get<uint32_t>(h, 8) != h.size() ||
        get<uint32_t>(h, 12) != b.size() || b.size() > psx::instrument::frame_limit ||
        get<uint32_t>(h, 84) != h.size() - 128) throw std::runtime_error("DIN1 framing");
    for (size_t i = 88; i < 128; ++i) if (h[i]) throw std::runtime_error("DIN1 reserved");
    InstrumentEnvelope e; std::copy_n(h.begin() + 16, 16, e.session.begin());
    e.source.pid = get<uint32_t>(h, 32); e.instrument = get<uint32_t>(h, 36);
    e.source.created = get<uint64_t>(h, 40); e.source.clock = get<uint64_t>(h, 48);
    e.generation = get<uint64_t>(h, 56); e.sequence = get<uint64_t>(h, 64);
    e.offered = get<uint64_t>(h, 72); e.state = State(get<uint32_t>(h, 80));
    e.source.identity.assign(reinterpret_cast<const char *>(h.data() + 128), h.size() - 128);
    e.frame = b; check(e); return e;
}
} // namespace psx::du::transport::tcp
