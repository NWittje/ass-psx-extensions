#pragma once
#include "output_identification.hpp"
#include <deque>
#include <mutex>
#include <span>

namespace hwio {
// The UI submits commands; only the provider worker touches the lease or writes
// hardware. Completion means the provider's actual transaction completed, never
// merely that its command queue accepted an item.
class IdentificationPort {
public:
    struct Command {IdentificationRequest request;IdentificationCard card;};
private:
    std::mutex mutex_;
    std::optional<Command> command_;
    std::deque<IdentificationCompletion> replies_;
public:
    bool submit(IdentificationRequest,const IdentificationCard&);
    std::optional<Command> take();
    void complete(IdentificationCompletion);
    std::optional<IdentificationCompletion> poll(std::uint64_t session);
};

// One worker-owned, bounded lease. Every eligible endpoint is owned until its
// latest normal intent is acknowledged during release. Endpoints without an
// ordinary assignment have normal OFF; there is no assumed hardware readback.
class IdentificationLease {
public:
    struct Write {IdentificationEndpoint endpoint;bool on{};std::uint64_t ticket{};};
private:
    struct Normal {bool on{},owned{true};std::uint64_t revision{};};
    IdentificationCard card_;
    IdentificationPort* port_{};
    std::vector<Normal> normal_;
    std::optional<IdentificationRequest> operation_;
    std::optional<Write> writing_;
    std::uint64_t next_ticket_{},writing_revision_{},session_{},last_operation_{};
    std::size_t cursor_{},writing_cursor_{};
    bool writing_restore_{},cleared_{};
    std::optional<std::size_t> illuminated_;
    void reply(bool success,bool released,std::string reason={});
    void finish_release();
public:
    bool active()const noexcept{return port_!=nullptr;}
    const IdentificationCard& card()const noexcept{return card_;}
    bool owns(std::uint16_t resource,std::uint16_t channel,std::uint16_t bit=UINT16_MAX)const noexcept;
    bool normal(std::uint16_t resource,std::uint16_t channel,bool on,std::uint16_t bit=UINT16_MAX)noexcept;
    void acquire(IdentificationPort&,IdentificationPort::Command,std::span<const std::uint8_t> initial,std::string error={});
    void request(IdentificationRequest);
    std::optional<Write> next();
    void complete(std::uint64_t ticket,bool success,std::string reason={});
    // The transport has been retired. No old operation may reach its successor.
    // This releases ownership, explicitly without claiming output restoration.
    void lost(std::string reason);
    void release();
};
}
