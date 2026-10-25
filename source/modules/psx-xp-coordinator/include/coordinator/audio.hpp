#pragma once
#include <psx/bridge/protocol.hpp>
#include <psx/exporter/protocol.hpp>
#include <array>
#include <vector>
namespace coordinator {
enum class AudioInputKind:std::uint8_t {attach=1,detach,hello,snapshot,lost,follow,mute,status,ack,tick,boundary,overflow,health,pause};
struct AudioInput {
    AudioInputKind kind{AudioInputKind::tick};std::int64_t now{};
    std::uint64_t x{},y{},z{},w{};
    psx::audio::Snapshot sample{};psx::audio::Status status{};psx::audio::LeaseAck ack{};
};
struct AudioView {
    psx::audio::State state{1,1};psx::audio::Status plugin{};psx::audio::LeaseAck ownership{};
    bool peer{},following{},muted{},status_available{};
    bool pause_available{},psx_paused{}; // Retain the mute until fresh evidence on invalidation; never infer resume from loss.
    bool effective_muted() const noexcept {return muted||psx_paused;}
    std::int64_t status_received{};
    std::uint64_t received{},rejected{},transitions{},overflows{},resets{},late_acks{},revision{1};
    std::uint64_t native_enqueued{},native_sent{},native_dropped{};
    psx::audio::Reason reason{psx::audio::Reason::not_following};
};
// Pure independently timed audio reducer/ownership state. Caller serializes
// calls under the existing facade mutex. No I/O, heap work or motion mutation.
class AudioModel {
public:
    explicit AudioModel(std::uint64_t frequency=0,std::uint64_t owner=1):frequency_(frequency),owner_(owner){}
    void configure(std::uint64_t frequency,std::uint64_t owner) noexcept {frequency_=frequency;owner_=owner;}
    bool apply(const AudioInput&) noexcept;
    bool next_frame(psx::bridge::Frame&,std::int64_t now) noexcept;
    bool next_lease(psx::audio::Lease&) noexcept;
    const AudioView& view() const noexcept{return view_;}
    std::uint32_t fresh_groups(std::int64_t now) const noexcept;
    std::vector<std::uint8_t> checkpoint() const;
    bool restore(const std::uint8_t*,std::size_t);
private:
    std::uint8_t checkpoint_version_{3}; // v1/2 retain historical mute and predicate replay semantics.
    void reset(psx::audio::Reason,std::int64_t now) noexcept;
    void control(bool follow,std::int64_t now) noexcept;
    void tick(std::int64_t now) noexcept;
    std::uint64_t frequency_{},owner_{1},operation_{},transition_sequence_{},lifecycle_{},configuration_{},published_epoch_{};
    psx::audio::Family family_{psx::audio::Family::unknown};std::uint8_t variant_{};
    std::uint32_t advertised_{};
    std::int64_t last_lease_{},stop_qpc_{};
    bool state_pending_{true},control_pending_{},lease_pending_{},waiting_stop_{},stop_sent_{};
    AudioView view_{};
    psx::audio::Lease pending_lease_{};psx::audio::Control pending_control_{};
    std::array<psx::audio::Transition,psx::audio::transition_capacity> edges_{};
    std::array<psx::audio::Snapshot,psx::audio::group_count> watermarks_{};std::uint32_t watermark_groups_{};
    std::size_t edge_head_{},edge_count_{};
};
std::vector<std::uint8_t> encode_audio_input(const AudioInput&);
bool decode_audio_input(const std::uint8_t*,std::size_t,AudioInput&) noexcept;
const char* audio_group_name(psx::audio::Group) noexcept;
const char* audio_reason_name(psx::audio::Reason) noexcept;
}
