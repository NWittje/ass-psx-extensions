#pragma once
#include "fmc_protocol.hpp"

namespace psx::fmc {
Hash memory_hash(const Snapshot&);
class Client {
 HANDLE pipe_{INVALID_HANDLE_VALUE};psx::situation::ProcessIdentity target_{},self_{};
 std::uint64_t session_{},next_{},last_operation_{},frequency_{};
 std::uint32_t sent_{},received_{};bool legacy_{};
 void open();
 wire::Request request(wire::Command,std::uint64_t operation,std::uint64_t lifecycle=0,std::uint64_t generation=0);
 wire::Result exchange(const wire::Request&);
 wire::Result wait(std::uint64_t,std::uint32_t timeout_ms);
public:
 explicit Client(psx::situation::ProcessIdentity);~Client();Client(const Client&)=delete;Client& operator=(const Client&)=delete;
 void connect();void disconnect()noexcept;
 wire::Result status();wire::Result lookup(std::uint64_t operation);
 wire::Result capture(std::vector<std::uint8_t>& artifact);
 wire::Result recall(std::span<const std::uint8_t> artifact,std::uint64_t lifecycle,std::uint64_t generation);
 std::uint64_t last_operation()const noexcept{return last_operation_;}
 bool historical_peer()const noexcept{return legacy_;}
};
}
