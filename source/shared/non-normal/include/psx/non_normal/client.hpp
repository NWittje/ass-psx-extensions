#pragma once
#include "pipe.hpp"
namespace psx::non_normal {
// Single worker owner. Never called by HTTP, native dynamics or SDK callbacks.
class Client {
 HANDLE pipe_{INVALID_HANDLE_VALUE};ProcessIdentity server_{};std::uint64_t session_{};
 std::uint32_t outgoing_{},incoming_{};
 std::uint16_t wire_version_{};
 std::uint64_t request_counter_{};
public:
 Client()=default;~Client(){disconnect();}Client(const Client&)=delete;Client& operator=(const Client&)=delete;
 bool connect(const std::wstring&,ProcessIdentity expected={})noexcept;
 void disconnect()noexcept;
 bool is_connected()const noexcept{return pipe_!=INVALID_HANDLE_VALUE;}
 ProcessIdentity server()const noexcept{return server_;}std::uint64_t session()const noexcept{return session_;}
 std::uint64_t next_request()noexcept{return request_counter_==UINT64_MAX?0:++request_counter_;}
 bool send(Request,std::uint32_t timeout_ms=1000)noexcept;
 bool receive(Result&,std::uint32_t timeout_ms=0)noexcept;
 bool transact(Request,Result&,std::uint32_t timeout_ms=1000)noexcept;
};
}
