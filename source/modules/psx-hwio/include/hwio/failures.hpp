#pragma once
#include "core.hpp"
#include "button_setup.hpp"
#include <psx/hwio/failures.hpp>

namespace hwio {
class FailureControl {
    struct Arm {psx::hwio::failures::ArmInfo info;double previous{};std::uint64_t sequence{};};
    std::array<Arm,psx::hwio::failures::max_arms> arms_{};
    std::deque<Command> outputs_;
    bool bound_{};
    std::uint64_t session_{},request_{},revision_{},alt_sequence_{},alt_generation_{},alt_start_{};
    std::uint32_t axes_{},alt_pid_{};
    int trim_{};
    std::int64_t owner_qpc_{},alt_sampled_unix_ms_{};
    double altitude_{};
    bool alt_valid_{};
    std::string reason_;
    void state(std::uint32_t,int);
    void activate(const psx::hwio::failures::ArmInfo&,int);
  public:
    static constexpr std::uint64_t binding_id=UINT64_MAX-1;
    psx::hwio::failures::Packet dispatch(const psx::hwio::failures::Packet&,Mapper&,std::string_view,std::uint16_t,std::int64_t,std::int64_t,int);
    void tick(Mapper&,std::int64_t,std::int64_t,int (*)());
    void clear(std::string_view reason);
    void detach(std::string_view reason) {clear(reason);request_=0;}
    void boundary(std::string_view reason);
    void reconcile(Mapper&,const ButtonRouter&,std::int64_t);
    std::optional<Command> next();
    bool filter(Command&,Mapper&) const;
    psx::hwio::failures::Packet status(const Mapper&,std::string_view,std::uint16_t,std::int64_t,std::int64_t) const;
    std::uint32_t axes() const noexcept {return axes_;}
    int trim() const noexcept {return trim_;}
    bool bound() const noexcept {return bound_;}
};
// Called only by Runtime's worker. One overlapped operation/one owner; no pipe thread.
class FailurePipe {
    HANDLE pipe_{INVALID_HANDLE_VALUE};
    OVERLAPPED io_{};
    enum class Phase {closed,connect,read,reply,write} phase_{Phase::closed};
    psx::hwio::failures::Packet packet_{};
    bool pending_{},lost_{};
    DWORD error_{};
    bool complete(DWORD&);
    void read();
    void reset();
  public:
    FailurePipe();
    ~FailurePipe();
    bool open(std::wstring_view suffix={});
    void close();
    const psx::hwio::failures::Packet* poll();
    void reply(const psx::hwio::failures::Packet&);
    bool take_loss() noexcept {const bool r=lost_;lost_=false;return r;}
    HANDLE event() const noexcept {return io_.hEvent;}
    DWORD error() const noexcept {return error_;}
};
} // namespace hwio
