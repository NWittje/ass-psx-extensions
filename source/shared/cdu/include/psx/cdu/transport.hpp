#pragma once
#include "framer.hpp"
#include "norm.hpp"
#include "psx/network/io.hpp"
#include "snapshot.hpp"
#include "blackout.hpp"
namespace psx::cdu
{
using RecordSink = void (*)(void *, std::string_view, std::uint64_t, Outcome);
// Track observations already decoded by Model; no second record decoder.
void track_record(const Model &, Outcome, std::array<std::uint64_t, 3> &) noexcept;
bool cache_initialized(const Model &) noexcept;
class MainSource
{
    psx::network::Stream stream_;
    psx::network::Adapter adapter_;
    psx::network::Endpoint endpoint_;
    bool established_{};
    std::uint64_t retry_ns_{}, connect_deadline_{};
    std::uint64_t refresh_at_{};
    bool refresh_attempted_{}, refresh_after_load_{};
    void state(SourceState);

  public:
    Model model;
    Framer framer;
    SourceState source_state{SourceState::disconnected};
    std::array<std::uint64_t, 3> revisions{1, 1, 1};
    std::uint64_t connections{}, records{}, rejections{}, refreshes{};
    RecordSink sink{};
    void *sink_user{};
    MainSource(std::string host, std::uint16_t port, const psx::network::Adapter &);
    void pump(std::uint64_t now_ns);
    void stop();
    // DNS/endpoint changes invalidate immediately while the next IPv4 target is unresolved.
    void suspend();
    // Same worker owner; preserve cache until the next connected generation.
    void reconnect(std::uint64_t now_ns);
    // Endpoint changes invalidate old source fields immediately; generations never reset.
    void configure(std::string host, std::uint16_t port, const psx::network::Adapter &,
                   std::uint64_t now_ns);
    void bind(const psx::network::Adapter &);
    HANDLE event() const noexcept
    {
        return stream_.event();
    }
    std::uint64_t deadline() const noexcept
    {
        if (established_ && refresh_at_)
            return refresh_at_;
        return stream_.state() == psx::network::State::connecting ? connect_deadline_ : retry_ns_;
    }
    int socket_error() const noexcept
    {
        return stream_.error();
    }
};
struct PublisherStats
{
    std::uint64_t offered{}, published{}, coalesced{}, max_delay_ns{}, blackout_published{};
};
class Publisher
{
    psx::norm::Adapter norm_;
    SessionId session_{};
    std::array<CduSnapshot, 3> latest_{};
    std::array<bool, 3> present_{}, dirty_{};
    std::array<std::uint64_t, 3> offered_ns_{};
    std::array<std::uint64_t, 3> sampled_ns_{};
    std::uint64_t sequence_{}, next_change_{}, repeat_{};
    PublisherStats stats_;
    blackout::Control blackout_{};
    bool blackout_present_{}, blackout_dirty_{};
    std::uint64_t blackout_sequence_{}, blackout_repeat_{};

  public:
    void start(const psx::norm::Options &);
    void stop() noexcept
    {
        norm_.stop();
        blackout_ = {}; blackout_present_ = blackout_dirty_ = false;
        blackout_sequence_ = blackout_repeat_ = 0;
    }
    bool started() const noexcept
    {
        return norm_.started();
    }
    const SessionId &session() const noexcept
    {
        return session_;
    }
    void offer(const CduSnapshot &, std::uint64_t now_ns);
    void offer_blackout(std::uint32_t mask, std::uint64_t revision, std::uint64_t generation,
                        bool ready, std::uint64_t now_ns);
    void pump(std::uint64_t now_ns, unsigned event_budget = 64);
    std::uint64_t deadline() const noexcept;
    HANDLE event() const noexcept
    {
        return norm_.event();
    }
    const PublisherStats &stats() const noexcept
    {
        return stats_;
    }
    const psx::norm::Stats &norm_stats() const noexcept
    {
        return norm_.stats();
    }
    std::array<int, 3> version() const noexcept
    {
        return norm_.version();
    }
};
enum class Admission
{
    accepted,
    duplicate,
    old_generation,
    conflict,
    retired,
    reset_required
};
struct ReceiverStatus { bool pinned{};SessionId session{};std::uint64_t generation{},last_advance{};std::array<bool,3> valid{}; };
class ReceiverState
{
    bool pinned_{};
    SessionId session_{};
    std::array<SessionId, 32> retired_{};
    unsigned retired_count_{};
    std::uint64_t generation_{}, last_advance_{};
    std::array<std::uint64_t, 3> sequences_{};

  public:
    std::array<CduSnapshot, 3> screens{};
    std::array<bool, 3> valid{};
    ReceiverStatus status()const noexcept{return {pinned_,session_,generation_,last_advance_,valid};}
    Admission accept(const CduSnapshot &, std::uint64_t now_ns);
    void clear() noexcept;
    bool healthy(std::uint64_t n) const noexcept
    {
        return pinned_ && n >= last_advance_ && n - last_advance_ < 2000000000ull;
    }
};
class Mailbox
{
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::array<CduSnapshot, 3> values_{};
    std::array<bool, 3> valid_{};

  public:
    void clear() noexcept;
    void store(const CduSnapshot &) noexcept;
    bool copy(Unit, CduSnapshot &) const noexcept;
};
struct ReceiverStats
{
    std::uint64_t accepted{}, malformed{}, duplicate{}, old_generation{}, conflict{}, retired{},
        resets{}, blackout_accepted{}, blackout_rejected{};
};
class Receiver
{
    using AcceptedCallback = void (*)(void *, const CduSnapshot &) noexcept;
    AcceptedCallback accepted_callback_{};
    void *accepted_user_{};
    psx::norm::Adapter norm_;
    psx::norm::Options options_;
    ReceiverState state_;
    ReceiverStats stats_;
    std::uint64_t now_{};
    bool reset_{};
    blackout::Reception blackout_;
    bool source_ready_{};
    static void packet(void *, const psx::norm::Packet &);

  public:
    // Worker-owned synchronous observation; the borrowed snapshot must not escape.
    void observe_accepted(AcceptedCallback callback, void *user) noexcept
    {
        accepted_callback_ = callback;
        accepted_user_ = user;
    }
    ReceiverStatus status()const noexcept{return state_.status();}
    blackout::Received blackout_status() const noexcept { return blackout_.status(); }
    Mailbox mailbox;
    void start(const psx::norm::Options &);
    void stop() noexcept
    {
        norm_.stop();
        blackout_.clear(); source_ready_ = false;
    }
    bool started() const noexcept
    {
        return norm_.started();
    }
    void pump(std::uint64_t now_ns);
    bool healthy(std::uint64_t n) const noexcept
    {
        return state_.healthy(n);
    }
    HANDLE event() const noexcept
    {
        return norm_.event();
    }
    const ReceiverStats &stats() const noexcept
    {
        return stats_;
    }
    const psx::norm::Stats &norm_stats() const noexcept
    {
        return norm_.stats();
    }
    std::array<int, 3> version() const noexcept
    {
        return norm_.version();
    }
};
psx::network::Adapter select_adapter(std::string_view address = {});
std::string adapter_ipv4(const psx::network::Adapter &);
// Tracks a selected adapter identity, never switches to a different adapter.
class Binding
{
    psx::network::Adapter adapter_;

  public:
    explicit Binding(const psx::network::Adapter &a) : adapter_(a)
    {
    }
    bool update(const psx::network::Inventory &);
    bool available() const noexcept
    {
        return adapter_.up;
    }
    const psx::network::Adapter &adapter() const noexcept
    {
        return adapter_;
    }
};
} // namespace psx::cdu
