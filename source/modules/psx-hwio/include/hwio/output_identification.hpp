#pragma once
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hwio {
// Transient addresses retain both generations; only persistent physical address
// components may later be written into ordinary HardwareBinding configuration.
struct IdentificationEndpoint {
    std::uint16_t resource{},channel{},bit{UINT16_MAX};
    std::uint32_t physical_order{};
    std::string label;
};
struct IdentificationCard {
    std::string device;
    std::uint64_t connection_generation{},resource_generation{};
    std::vector<IdentificationEndpoint> endpoints;
};
enum class IdentificationPhase {idle,acquiring,clearing,lighting,selecting,channel_off,releasing,review,paused,closed};
enum class IdentificationRequestKind {acquire,all_off,on,off,release};
struct IdentificationRequest {
    static constexpr auto no_channel=std::numeric_limits<std::size_t>::max();
    IdentificationRequestKind kind{};
    std::uint64_t session{},operation{},connection_generation{},resource_generation{};
    std::size_t channel{no_channel};
};
struct IdentificationCompletion {
    IdentificationRequest request;
    bool success{},ownership_released{};
    std::string reason;
};
struct IdentificationGesture {std::uint64_t session{},channel{},view{};};
enum class IdentificationChoiceKind {unvisited,assigned,skipped};
struct IdentificationChoice {
    IdentificationChoiceKind kind{IdentificationChoiceKind::unvisited};
    std::string output;
    bool operator==(const IdentificationChoice&)const=default;
};
// One UI-owned model, outside input callbacks. The backend serializes operations,
// reserves the whole affected output bank/card and restores its LATEST normal
// intent on release. A successful completion is not optical lamp verification.
class OutputIdentification {
    enum class EditKind {assign,skip,undo};
    struct Edit {EditKind kind{};std::string output;};
    struct History {std::size_t channel{};IdentificationChoice previous;};
    IdentificationCard card_;
    std::vector<IdentificationChoice> choices_;
    std::vector<History> history_;
    std::optional<IdentificationRequest> pending_;
    std::optional<Edit> edit_;
    IdentificationPhase phase_{IdentificationPhase::idle},after_release_{IdentificationPhase::closed};
    std::uint64_t session_{},operation_{},channel_revision_{},view_revision_{};
    std::uint64_t announced_connection_{},announced_resources_{};
    std::size_t channel_{};
    bool delivered_{},owned_{},image_ready_{},retry_only_{},undo_after_clear_{},review_after_edit_{};
    std::string region_{"flightdeck"},reason_;
    bool gesture(IdentificationGesture)const noexcept;
    bool selectable(IdentificationGesture)const noexcept;
    void issue(IdentificationRequestKind,std::size_t=IdentificationRequest::no_channel);
    void root_view();
    void illuminate();
    void change_after_off();
    void apply_undo();
    void release(IdentificationPhase);
    void fail(std::string);
    bool start(IdentificationCard,bool reset);
public:
    OutputIdentification()=default;
    OutputIdentification(const OutputIdentification&)=delete;
    OutputIdentification& operator=(const OutputIdentification&)=delete;
    bool begin(IdentificationCard);
    bool resume(IdentificationCard);
    std::optional<IdentificationRequest> take_request();
    bool complete(const IdentificationCompletion&);
    bool image_ready(IdentificationGesture)noexcept;
    bool navigate(std::string_view,IdentificationGesture);
    bool choose(std::string_view output,bool qualified,IdentificationGesture);
    bool skip(IdentificationGesture);
    bool undo(IdentificationGesture);
    bool retry_skipped(std::size_t,IdentificationGesture);
    void inventory_changed(std::uint64_t connection,std::uint64_t resources);
    void pause(std::string reason);
    void review();
    void stop();
    bool finish(bool persisted,std::string_view error={});
    IdentificationGesture token()const noexcept{return {session_,channel_revision_,view_revision_};}
    IdentificationPhase phase()const noexcept{return phase_;}
    const IdentificationCard& card()const noexcept{return card_;}
    const auto& choices()const noexcept{return choices_;}
    std::size_t channel()const noexcept{return channel_;}
    std::string_view region()const noexcept{return region_;}
    std::string_view reason()const noexcept{return reason_;}
    bool ownership_pending()const noexcept{return owned_ || pending_.has_value();}
    bool can_select()const noexcept{return phase_==IdentificationPhase::selecting && image_ready_;}
};
}
