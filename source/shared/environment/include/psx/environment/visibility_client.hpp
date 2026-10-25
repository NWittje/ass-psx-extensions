#pragma once
#include "presentation.hpp"
#include <psx/non_normal/pipe.hpp>
namespace psx::environment::presentation {
// One IOS network-worker owner. Authenticated IPC; no SDK/native/HTTP-thread waits.
class VisibilityClient {
 friend struct VisibilityClientFixture;
 HANDLE pipe_{INVALID_HANDLE_VALUE};ProcessIdentity server_{};std::uint64_t session_{},request_counter_{};
 std::uint32_t outgoing_{},incoming_{};
public:
 VisibilityClient()=default;~VisibilityClient(){disconnect();}
 VisibilityClient(const VisibilityClient&)=delete;VisibilityClient& operator=(const VisibilityClient&)=delete;
 bool connect(const std::wstring& endpoint,ProcessIdentity expected={})noexcept;
 void disconnect()noexcept;
 bool is_connected()const noexcept{return pipe_!=INVALID_HANDLE_VALUE;}
 ProcessIdentity server()const noexcept{return server_;}std::uint64_t session()const noexcept{return session_;}
 std::uint64_t next_request()noexcept{return request_counter_==UINT64_MAX?0:++request_counter_;}
 bool transact(VisibilityRequest,VisibilityResult&,std::uint32_t timeout_ms=1000)noexcept;
};
}
