#pragma once
#include "protocol.hpp"
#include <memory>
#include <string>

namespace psx::scenery {
std::wstring coordinator_endpoint();
// Worker-only. One absolute deadline includes connection, authentication and reply.
Result check_destination(const Query&,std::uint32_t timeout_ms=check_timeout_ms) noexcept;
Result check_destination_at(const Query&,const std::wstring& endpoint,std::uint32_t timeout_ms) noexcept;
class Server final {
public:
    using Handler=Result (*)(void*,const Query&,std::uint32_t) noexcept;
    Server();~Server();
    Server(const Server&)=delete;Server& operator=(const Server&)=delete;
    bool start(Handler,void*,const std::wstring& endpoint={}) noexcept;
    void request_stop() noexcept;
    void stop() noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
