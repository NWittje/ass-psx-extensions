#include <psx/display/tcp.hpp>
#include <psx/display/dpf_protocol.hpp>
#include <algorithm>
#include <bcrypt.h>
#include <cstring>
namespace psx::du::transport::tcp {
namespace {
std::atomic_size_t owned{}, peak{}, model{};
template <class T> T get(std::span<const uint8_t> b, size_t p) {
    if (p > b.size() || sizeof(T) > b.size() - p)
        throw std::runtime_error("DUT1 bounds");
    T v;
    std::memcpy(&v, b.data() + p, sizeof v);
    return v;
}
template <class T, class B> void put(B &b, size_t p, T v) {
    std::memcpy(b.data() + p, &v, sizeof v);
}
// Charge payload bytes before allocation. The shared Owned deleter keeps the
// charge until the last reference releases the buffer, including pending sends.
Owned buffer(size_t n) {
    if (n > frame_limit)
        throw std::runtime_error("TCP frame allocation bound");
    auto v = owned.load();
    do {
        if (n > 207 * 1024 * 1024 - v)
            throw std::runtime_error("TCP host payload budget");
    } while (!owned.compare_exchange_weak(v, v + n));
    auto hi = peak.load();
    while (hi < v + n && !peak.compare_exchange_weak(hi, v + n)) {
    }
    // Failed byte allocation returns its reservation before propagating the error.
    Bytes *b;
    try {
        b = new Bytes(n);
    } catch (...) {
        owned -= n;
        throw;
    }
    return Owned(b, [n](auto *p) {
        delete p;
        owned -= n;
    });
}
}
Identity identity() {
    Identity v{};
    if (BCryptGenRandom(nullptr, v.data(), ULONG(v.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("TCP identity generation");
    return v;
}
std::array<uint8_t, 32> encode_header(const Header &h) {
    if (uint16_t(h.type) < 1 || uint16_t(h.type) > (h.protocol == 2 ? 9 : 6) || (h.protocol != 1 && h.protocol != 2) ||
        h.bytes > (h.type == Type::data ? frame_limit + envelope_limit : h.type == Type::instrument ? psx::instrument::frame_limit + envelope_limit : control_limit))
        throw std::runtime_error("DUT1 header bounds");
    std::array<uint8_t, 32> b{};
    put(b, 0, magic);
    put(b, 4, h.protocol);
    put(b, 6, uint16_t(h.type));
    put(b, 8, h.bytes);
    std::copy(h.connection.begin(), h.connection.end(), b.begin() + 16);
    return b;
}
Header decode_header(std::span<const uint8_t> b) {
    if (b.size() != 32 || get<uint32_t>(b, 0) != magic || (get<uint16_t>(b, 4) != 1 && get<uint16_t>(b, 4) != 2) || get<uint32_t>(b, 12))
        throw std::runtime_error("DUT1 framing/version");
    Header h{Type(get<uint16_t>(b, 6)), get<uint32_t>(b, 8)};
    h.protocol = get<uint16_t>(b, 4);
    std::copy_n(b.begin() + 16, 16, h.connection.begin());
    (void)encode_header(h);
    return h;
}
std::array<uint8_t, 64> encode_ack(const Ack &a) {
    std::array<uint8_t, 64> b{};
    std::copy(a.token.session.begin(), a.token.session.end(), b.begin());
    put(b, 16, a.token.generation);
    put(b, 24, a.token.publication);
    put(b, 32, a.token.frame);
    put(b, 40, uint32_t(a.disposition));
    put(b, 44, a.unit);
    put(b, 48, a.received);
    put(b, 56, a.clock);
    return b;
}
Ack decode_ack(std::span<const uint8_t> b) {
    if (b.size() != 64)
        throw std::runtime_error("DUT1 ACK length");
    Ack a;
    std::copy_n(b.begin(), 16, a.token.session.begin());
    a.token.generation = get<uint64_t>(b, 16);
    a.token.publication = get<uint64_t>(b, 24);
    a.token.frame = get<uint64_t>(b, 32);
    a.disposition = Disposition(get<uint32_t>(b, 40));
    a.unit = get<uint32_t>(b, 44);
    a.received = get<uint64_t>(b, 48);
    a.clock = get<uint64_t>(b, 56);
    if (a.unit >= 6 || uint32_t(a.disposition) < 1 || uint32_t(a.disposition) > 3 || !a.clock || !a.token.generation ||
        !a.token.publication || !a.token.frame)
        throw std::runtime_error("DUT1 ACK fields");
    return a;
}
Frame packet(const Envelope &e, Owned payload, uint64_t revision) {
    if (e.state == State::frame) {
        if (!payload || e.frame.size() != payload->size())
            throw std::runtime_error("TCP owned frame mismatch");
        auto d = psx::recorder::decode(*payload);
        if (d.unit != e.unit || d.frequency != e.source.clock || !e.source.pid || !e.source.created ||
            !identity_json(e.source.identity))
            throw std::runtime_error("TCP source/frame mismatch");
    } else if (payload || !e.frame.empty())
        throw std::runtime_error("TCP status payload");
    auto encoded = encode({e.session,
                           e.source,
                           e.unit,
                           e.state == State::frame ? State::waiting : e.state,
                           e.generation,
                           e.sequence,
                           e.offered,
                           {}});
    auto header = own(*encoded);
    encoded.reset();
    auto mutable_header = std::const_pointer_cast<Bytes>(header);
    put(*mutable_header, 12, uint32_t(payload ? payload->size() : 0));
    put(*mutable_header, 80, uint32_t(e.state));
    auto result = std::make_shared<Packet>();
    result->envelope = std::move(header);
    result->payload = std::move(payload);
    result->revision = revision;
    result->token = {e.session, e.generation, e.sequence,
                     result->payload ? psx::display::word(result->payload->data(), 16) : 0};
    result->end = result->payload ? psx::display::word(result->payload->data(), 48) : 0;
    return result;
}
Envelope decode_parts(std::span<const uint8_t> h, std::span<const uint8_t> b) {
    if (h.size() < 128 || h.size() > envelope_limit || get<uint32_t>(h, 8) != h.size() ||
        get<uint32_t>(h, 12) != b.size())
        throw std::runtime_error("DUN1 split lengths");
    auto state = get<uint32_t>(h, 80);
    std::array<uint8_t, envelope_limit> copy{};
    std::copy(h.begin(), h.end(), copy.begin());
    put(copy, 12, uint32_t(0));
    if (state == 1)
        put(copy, 80, uint32_t(State::waiting));
    auto e = decode({copy.data(), h.size()});
    e.state = State(state);
    e.frame = b;
    if (e.state == State::frame) {
        auto d = psx::recorder::decode(b);
        if (d.unit != e.unit || d.frequency != e.source.clock || !e.source.pid || !e.source.created ||
            !identity_json(e.source.identity))
            throw std::runtime_error("DUN1 split source mismatch");
    } else if (!b.empty())
        throw std::runtime_error("DUN1 split status payload");
    return e;
}
// Compatibility path for borrowed LocalSource until the reviewed owned sink is bound.
Owned own(std::span<const uint8_t> b) {
    auto p = buffer(b.size());
    auto w = std::const_pointer_cast<Bytes>(p);
    std::copy(b.begin(), b.end(), w->begin());
    return p;
}
std::shared_ptr<Bytes> allocate(size_t n) {
    return std::const_pointer_cast<Bytes>(buffer(n));
}
size_t payload_bytes() noexcept {
    return owned.load();
}
size_t payload_peak() noexcept {
    return peak.load();
}
// All allowances below are bytes: raw slots 24 MiB + Direct-local assembly 8 MiB
// + instrument slots/assembly 4 MiB + models 26 MiB + diagnostics 1 MiB leave
// 1 MiB for bounded control/receive
// scratch inside the 64 MiB CPU allowance.
bool reserve_model(size_t n) noexcept {
    auto v = model.load();
    do {
        if (n > 26 * 1024 * 1024 - v)
            return false;
    } while (!model.compare_exchange_weak(v, v + n));
    return true;
}
void release_model(size_t n) noexcept {
    model -= n;
}
size_t model_bytes() noexcept {
    return model.load();
}
}
