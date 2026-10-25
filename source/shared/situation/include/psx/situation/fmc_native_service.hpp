#pragma once
#include "fmc_native.hpp"
#include "fmc_protocol.hpp"

namespace psx::fmc::native_records {
struct Profile {Compatibility compatibility;Manifest records;};
Profile qualified_profile();

// Worker-only, single-owner client for the selected local PSX process. A
// transport failure retains the operation; reconnect/lookup never resends it.
class Client {
 HANDLE pipe_{INVALID_HANDLE_VALUE};
 psx::situation::ProcessIdentity target_{},self_{};
 Profile profile_;
 std::uint64_t session_{},next_{},last_{},frequency_{};
 std::uint32_t sent_{},received_{};
 std::uint16_t version_{wire::native_revision};
 struct Pending {
  wire::Command command{wire::Command::status};std::uint64_t operation{},lifecycle{},generation{};
  std::int64_t issued{};Hash artifact{};
 } pending_;
 void open();
 wire::Request request(wire::Command,std::uint64_t,std::uint64_t lifecycle=0,std::uint64_t generation=0);
 wire::Result exchange(const wire::Request&);
 bool available(const wire::Result&,wire::Capability)const;
 void complete(const wire::Result&);
public:
 explicit Client(psx::situation::ProcessIdentity,Profile=qualified_profile());
 ~Client();Client(const Client&)=delete;Client& operator=(const Client&)=delete;
 void connect();void disconnect()noexcept;
 wire::Result status();wire::Result lookup(std::uint64_t operation);
 wire::Result wait(std::uint64_t operation,std::uint32_t timeout_ms=30000);
 wire::Result capture(std::vector<std::uint8_t>& artifact,std::uint32_t wait_timeout_ms=30000);
 wire::Result fetch_capture(std::uint64_t operation,std::vector<std::uint8_t>& artifact);
 wire::Result recall(std::span<const std::uint8_t> artifact,std::uint64_t lifecycle,std::uint64_t generation,std::uint32_t wait_timeout_ms=30000);
 std::uint64_t last_operation()const noexcept{return last_;}
 bool native_peer()const noexcept{return version_==wire::native_revision;}
};
}
