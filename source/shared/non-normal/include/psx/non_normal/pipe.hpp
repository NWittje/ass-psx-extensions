#pragma once
#include "protocol.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <string>
namespace psx::non_normal {
ProcessIdentity current_process()noexcept;
std::int64_t qpc()noexcept;std::uint64_t qpc_frequency()noexcept;std::uint64_t nonce()noexcept;
std::wstring current_sid();
std::wstring reset_endpoint(ProcessIdentity);
std::wstring coordinator_endpoint();
std::wstring plugin_endpoint(ProcessIdentity);
// Worker-only; DACL grants current user, remote clients refused, first-instance enforced.
HANDLE create_server(const std::wstring&,std::uint32_t max_instances=1,bool first_instance=true)noexcept;
bool accept(HANDLE,std::uint32_t timeout_ms)noexcept;
bool authenticate(HANDLE,bool server_side,ProcessIdentity expected,ProcessIdentity& actual)noexcept;
bool connected(HANDLE)noexcept;
// Idle wait in milliseconds; zero polls only complete buffered frames. Once
// assembly starts, header and body share a one-second peer-response allowance.
bool read_frame(HANDLE,Frame&,std::uint32_t timeout_ms)noexcept;
bool write_frame(HANDLE,const Frame&,std::uint32_t timeout_ms=1000)noexcept;
}
