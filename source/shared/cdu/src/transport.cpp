#include "psx/cdu/transport.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace psx::cdu
{
void track_record(const Model &model, Outcome result,
                  std::array<std::uint64_t, 3> &revisions) noexcept
{
    if (result.result == Result::lifecycle)
    {
        for (auto &r : revisions)
            ++r;
        return;
    }
    if (result.result != Result::accepted)
        return;
    for (unsigned u = 0; u < 3; ++u)
    {
        const auto &s = model.screens[u];
        bool changed = s.appearance.receipt.sequence == model.sequence ||
                       s.brightness.receipt.sequence == model.sequence ||
                       s.blanking.receipt.sequence == model.sequence;
        for (unsigned row = 0; row < rows; ++row)
            changed |= s.text[row].receipt.sequence == model.sequence ||
                       s.colour[row].receipt.sequence == model.sequence;
        if (changed)
            ++revisions[u];
    }
}
bool cache_initialized(const Model &model) noexcept
{
    if (model.loading)
        return false;
    for (const auto &s : model.screens)
    {
        if (!s.appearance.receipt.valid || !s.brightness.receipt.valid || !s.blanking.receipt.valid)
            return false;
        for (const auto &row : s.text)
            if (!row.receipt.valid)
                return false;
    }
    return true;
}
namespace
{
bool refresh_complete(const Model &model) noexcept
{
    if (!cache_initialized(model))
        return false;
    // Empty colour records are observations too; a selected LCD client needs
    // their validity even when the current source presentation is CRT.
    for (const auto &screen : model.screens)
        for (const auto &row : screen.colour)
            if (!row.receipt.valid)
                return false;
    return true;
}
}
std::string adapter_ipv4(const psx::network::Adapter &a)
{
    for (const auto &e : a.addresses)
        if (e.address.ss_family == AF_INET)
            return e.host();
    throw std::runtime_error("Adapter has no IPv4 address");
}
psx::network::Adapter select_adapter(std::string_view ip)
{
    if (ip == "127.0.0.1")
    {
        psx::network::Adapter a;
        a.up = true;
        a.addresses.push_back(psx::network::Endpoint::parse(std::string(ip), 0));
        return a;
    }
    auto all = psx::network::enumerate();
    auto id = psx::network::initial_adapter({all});
    for (const auto &a : all)
        if (a.up)
        {
            if (ip.empty() && a.guid == id)
                return a;
            for (const auto &e : a.addresses)
                if (e.host() == ip)
                    return a;
        }
    throw std::runtime_error(
        "No unambiguous active wired IPv4 adapter; supply --adapter <local IPv4>");
}
MainSource::MainSource(std::string host, std::uint16_t port, const psx::network::Adapter &a)
    : adapter_(a), endpoint_(psx::network::Endpoint::parse(host, port))
{
}
bool Binding::update(const psx::network::Inventory &i)
{
    auto next = adapter_;
    next.up = false;
    for (const auto &a : i.adapters)
        if (a.guid == adapter_.guid)
        {
            next = a;
            break;
        }
    bool ipv4 = false;
    for (const auto &e : next.addresses)
        ipv4 |= e.address.ss_family == AF_INET;
    next.up = next.up && ipv4;
    bool changed = !psx::network::same_binding(adapter_, next);
    adapter_ = next;
    return changed;
}
void MainSource::bind(const psx::network::Adapter &a)
{
    stream_.close();
    adapter_ = a;
    established_ = false;
    state(SourceState::disconnected);
    retry_ns_ = psx::norm::monotonic_ns() + 1000000000ull;
}
void MainSource::state(SourceState s)
{
    if (source_state != s)
    {
        source_state = s;
        for (auto &r : revisions)
            ++r;
    }
}
void MainSource::pump(std::uint64_t n)
{
    if (!adapter_.up)
        return;
    using psx::network::State;
    if (stream_.state() == State::connecting && n >= connect_deadline_)
        stream_.close();
    if (stream_.state() == State::failed || stream_.state() == State::closed)
    {
        if (established_)
        {
            established_ = false;
            state(SourceState::disconnected);
            retry_ns_ = n + 1000000000ull;
        }
        else if (source_state == SourceState::connecting)
        {
            state(SourceState::disconnected);
            retry_ns_ = n + 1000000000ull;
        }
        if (n >= retry_ns_)
        {
            framer.reset();
            state(SourceState::connecting);
            stream_.connect(adapter_, endpoint_);
            retry_ns_ = n + 1000000000ull;
            connect_deadline_ = n + 3000000000ull;
        }
    }
    stream_.pump();
    if (stream_.state() == State::connected && !established_)
    {
        established_ = true;
        ++connections;
        model.begin_generation();
        model.loading = false;
        model.source_version.clear();
        for (auto &r : revisions)
            ++r;
        state(SourceState::initializing);
        refresh_attempted_ = false;
        refresh_after_load_ = false;
        refresh_at_ = n + 1000000000ull;
        constexpr std::uint8_t bang[]{'b', 'a', 'n', 'g', '\n'};
        if (!stream_.send(bang))
            throw std::runtime_error("PSX bang failed");
    }
    bool exit = false;
    for (unsigned pass = 0; pass < 8; ++pass)
    {
        auto bytes = stream_.received();
        if (bytes.empty())
            break;
        framer.feed({reinterpret_cast<const char *>(bytes.data()), bytes.size()},
                    [&](std::string_view line) {
                        if (!Model::relevant(line))
                            return;
                        auto time = psx::norm::monotonic_ns();
                        auto result = model.receive(line, time);
                        if (line == "load1")
                        {
                            refresh_at_ = 0;
                            refresh_after_load_ = false;
                        }
                        else if (line == "load3" && !refresh_attempted_)
                        {
                            refresh_at_ = time + 250000000ull;
                            refresh_after_load_ = true;
                        }
                        ++records;
                        rejections += result.result == Result::rejected;
                        track_record(model, result, revisions);
                        if (line == "exit")
                            exit = true;
                        if (sink)
                            sink(sink_user, line, time, result);
                    });
        stream_.consume(bytes.size());
        stream_.pump();
    }
    if (exit)
        stream_.close();
    if (established_ && (stream_.state() == State::closed || stream_.state() == State::failed))
    {
        established_ = false;
        state(SourceState::disconnected);
        retry_ns_ = n + 1000000000ull;
    }
    else if (established_)
    {
        state(cache_initialized(model) ? SourceState::connected : SourceState::initializing);
        if (refresh_complete(model) && !refresh_after_load_)
        {
            refresh_at_ = 0;
            refresh_attempted_ = false;
        }
        else if (!model.loading && !refresh_attempted_ && refresh_at_ && n >= refresh_at_)
        {
            // Refresh every completed situation load, which may emit only
            // changed fields, and an incomplete initial baseline. Keep Main open.
            // A refresh can itself carry load markers: only a complete baseline
            // rearms recovery, so a broken/partial peer cannot cause a bang loop.
            constexpr std::uint8_t bang[]{'b', 'a', 'n', 'g', '\n'};
            refresh_attempted_ = true;
            refresh_after_load_ = false;
            refresh_at_ = 0;
            if (!stream_.send(bang))
                throw std::runtime_error("PSX initialization refresh failed");
            ++refreshes;
        }
    }
}
void MainSource::stop()
{
    if (stream_.state() == psx::network::State::connected)
    {
        constexpr std::uint8_t exit[]{'e', 'x', 'i', 't', '\n'};
        stream_.send(exit);
        auto end = psx::norm::monotonic_ns() + 250000000ull;
        while (stream_.sending() && psx::norm::monotonic_ns() < end)
        {
            stream_.pump();
            WaitForSingleObject(stream_.event(), 10);
        }
    }
    stream_.close();
    established_ = false;
    refresh_at_ = 0;
    state(SourceState::disconnected);
}
void MainSource::suspend()
{
    stop();framer.reset();adapter_={};model.begin_generation();
    model.loading=false;model.source_version.clear();
    for(auto&revision:revisions)++revision;
}
void MainSource::reconnect(std::uint64_t now_ns)
{
    stop();
    framer.reset();
    retry_ns_ = now_ns;
}
void MainSource::configure(std::string host, std::uint16_t port,
                           const psx::network::Adapter &adapter, std::uint64_t now_ns)
{
    auto endpoint = psx::network::Endpoint::parse(host, port);
    const bool changed = !endpoint.same(endpoint_);
    stop();
    framer.reset();
    endpoint_ = endpoint;
    adapter_ = adapter;
    if (changed)
    {
        model.begin_generation();
        model.loading = false;
        model.source_version.clear();
        for (auto &revision : revisions)
            ++revision;
    }
    retry_ns_ = now_ns;
}
void Publisher::start(const psx::norm::Options &o)
{
    norm_.start(o, true);
    psx::norm::random_bytes(session_);
    present_ = {};
    dirty_ = {};
    sequence_ = 0;
    next_change_ = repeat_ = 0;
    stats_ = {};
    blackout_ = {}; blackout_present_ = blackout_dirty_ = false;
    blackout_sequence_ = blackout_repeat_ = 0;
}
void Publisher::offer_blackout(std::uint32_t mask,std::uint64_t revision,
                              std::uint64_t generation,bool ready,std::uint64_t n)
{
    if(!generation){blackout_={};blackout_present_=blackout_dirty_=false;return;}
    if(!revision||(mask & ~blackout::all_units)||(!ready&&mask))
        throw std::runtime_error("Invalid CDU blackout offer");
    blackout::Control c{session_,generation,0,revision,mask,ready?blackout::ready_flag:0};
    if(blackout_present_&&c==blackout_)return;
    blackout_=c;blackout_present_=blackout_dirty_=true;
    if(!blackout_repeat_)blackout_repeat_=n;
}
void Publisher::offer(const CduSnapshot &s, std::uint64_t n)
{
    auto u = unsigned(s.unit);
    if (u > 2)
        throw std::runtime_error("Invalid CDU offer");
    if (dirty_[u])
        ++stats_.coalesced;
    latest_[u] = s;
    present_[u] = dirty_[u] = true;
    offered_ns_[u] = sampled_ns_[u] = n;
    ++stats_.offered;
}
std::uint64_t Publisher::deadline() const noexcept
{
    bool dirty = std::any_of(dirty_.begin(), dirty_.end(), [](bool b) { return b; });
    auto deadline = dirty ? std::min(next_change_, repeat_) : repeat_;
    if(blackout_present_)deadline=std::min(deadline,blackout_dirty_?next_change_:blackout_repeat_);
    return deadline;
}
void Publisher::pump(std::uint64_t n, unsigned event_budget)
{
    norm_.pump(nullptr,nullptr,event_budget);
    if (!started())
        return;
    const bool heartbeat = n >= repeat_;
    if (heartbeat)
        for (unsigned u = 0; u < 3; ++u)
            if (present_[u] && !dirty_[u])
            {
                dirty_[u] = true;
                offered_ns_[u] = n;
            }
    if (n < next_change_)
        return;
    if(blackout_present_ && n>=blackout_repeat_)blackout_dirty_=true;
    // One latest control offer gets a slot before page bursts; existing cache
    // bounds and per-pass event limits still govern ownership and repair.
    if(blackout_present_&&blackout_dirty_)
    {
        auto c=blackout_;c.sequence=blackout_sequence_+1;
        std::array<std::uint8_t,blackout::control_bytes> bytes{};
        if(!blackout::encode(c,bytes))throw std::runtime_error("Invalid CDU blackout encoding");
        if(norm_.send(bytes)){++blackout_sequence_;++stats_.blackout_published;blackout_dirty_=false;blackout_repeat_=n+250000000ull;}
    }
    for (unsigned u = 0; u < 3; ++u)
        if (dirty_[u] && present_[u])
        {
            auto snapshot = latest_[u];
            snapshot.session = session_;
            snapshot.publication_sequence = sequence_ + 1;
            snapshot.host_elapsed_ns += n - sampled_ns_[u];
            WireBuffer b;
            std::string e;
            if (!encode(snapshot, b, &e))
                throw std::runtime_error(e);
            if (norm_.send({b.bytes.data(), b.size}))
            {
                ++sequence_;
                ++stats_.published;
                stats_.max_delay_ns = std::max(stats_.max_delay_ns, n - offered_ns_[u]);
                dirty_[u] = false;
            }
        }
    next_change_ = n + 16667000ull;
    if (heartbeat)
        repeat_ = n + 250000000ull;
}
void ReceiverState::clear() noexcept
{
    pinned_ = false;
    retired_count_ = 0;
    generation_ = last_advance_ = 0;
    sequences_ = {};
    valid = {};
    screens = {};
}
Admission ReceiverState::accept(const CduSnapshot &s, std::uint64_t n)
{
    for (unsigned i = 0; i < retired_count_; ++i)
        if (s.session == retired_[i])
            return Admission::retired;
    if (!pinned_)
    {
        session_ = s.session;
        pinned_ = true;
        generation_ = 0;
    }
    else if (s.session != session_)
    {
        if (healthy(n))
            return Admission::conflict;
        if (retired_count_ == retired_.size())
            return Admission::reset_required;
        retired_[retired_count_++] = session_;
        session_ = s.session;
        generation_ = 0;
        sequences_ = {};
        valid = {};
    }
    if (s.generation < generation_)
        return Admission::old_generation;
    if (s.generation > generation_)
    {
        generation_ = s.generation;
        sequences_ = {};
        valid = {};
    }
    auto u = unsigned(s.unit);
    if (s.publication_sequence <= sequences_[u])
        return Admission::duplicate;
    sequences_[u] = s.publication_sequence;
    screens[u] = s;
    valid[u] = true;
    last_advance_ = n;
    return Admission::accepted;
}
void Mailbox::clear() noexcept
{
    AcquireSRWLockExclusive(&lock_);
    valid_ = {};
    ReleaseSRWLockExclusive(&lock_);
}
void Mailbox::store(const CduSnapshot &s) noexcept
{
    auto u = unsigned(s.unit);
    AcquireSRWLockExclusive(&lock_);
    values_[u] = s;
    valid_[u] = true;
    ReleaseSRWLockExclusive(&lock_);
}
bool Mailbox::copy(Unit unit, CduSnapshot &s) const noexcept
{
    auto u = unsigned(unit);
    if (u > 2)
        return false;
    AcquireSRWLockShared(&lock_);
    bool valid = valid_[u];
    if (valid)
        s = values_[u];
    ReleaseSRWLockShared(&lock_);
    return valid;
}
void Receiver::start(const psx::norm::Options &o)
{
    if (o.expected_host.empty())
        throw std::runtime_error("Receiver requires explicit host IPv4");
    auto ep = psx::network::Endpoint::parse(o.expected_host, 1);
    if (ep.address.ss_family != AF_INET)
        throw std::runtime_error("Receiver requires IPv4");
    norm_.start(o, false);
    options_ = o;
    state_.clear();
    mailbox.clear();
    stats_ = {};
    reset_ = false;
    blackout_.clear(); source_ready_ = false;
}
void Receiver::packet(void *user, const psx::norm::Packet &p)
{
    auto &r = *static_cast<Receiver *>(user);
    if(p.bytes.size()>=4 && std::memcmp(p.bytes.data(),"CBL1",4)==0)
    {
        auto source=r.state_.status();
        r.blackout_.source(source.pinned,source.session,source.generation,
                           r.state_.healthy(r.now_)&&r.source_ready_,r.now_);
        blackout::Control c;
        if(blackout::decode(p.bytes,c)&&r.blackout_.accept(c,r.now_))++r.stats_.blackout_accepted;
        else ++r.stats_.blackout_rejected;
        return;
    }
    CduSnapshot s;
    if (!decode(p.bytes, s))
    {
        ++r.stats_.malformed;
        return;
    }
    auto prior = r.state_.valid;
    auto admission = r.state_.accept(s, r.now_);
    switch (admission)
    {
    case Admission::accepted:
        r.source_ready_ = s.source_state == SourceState::connected;
        r.blackout_.source(true,s.session,s.generation,r.source_ready_,r.now_);
        for (unsigned u = 0; u < 3; ++u)
            if (prior[u] && !r.state_.valid[u])
            {
                r.mailbox.clear();
                break;
            }
        if (r.accepted_callback_) r.accepted_callback_(r.accepted_user_, s);
        r.mailbox.store(s);
        ++r.stats_.accepted;
        break;
    case Admission::duplicate:
        ++r.stats_.duplicate;
        break;
    case Admission::old_generation:
        ++r.stats_.old_generation;
        break;
    case Admission::conflict:
        ++r.stats_.conflict;
        break;
    case Admission::retired:
        ++r.stats_.retired;
        break;
    case Admission::reset_required:
        r.reset_ = true;
        break;
    }
}
void Receiver::pump(std::uint64_t n)
{
    now_ = n;
    norm_.pump(packet, this);
    auto source=state_.status();
    blackout_.source(source.pinned,source.session,source.generation,
                     state_.healthy(n)&&source_ready_,n);
    if (reset_)
    {
        norm_.stop();
        state_.clear();
        mailbox.clear();
        blackout_.clear(); source_ready_=false;
        norm_.start(options_, false);
        reset_ = false;
        ++stats_.resets;
    }
}
} // namespace psx::cdu
