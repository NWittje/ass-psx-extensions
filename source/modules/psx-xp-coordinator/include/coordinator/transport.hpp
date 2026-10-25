#pragma once
#include "coordinator/status.hpp"
#include <atomic>
#include <string>
#include <thread>

namespace coordinator {
// start/join are serialized by the owner; request_stop may be called while a worker runs.
// All OS I/O handles remain owned by that worker until cancellation has completed.
struct TcpEndpoint {
    std::string ipv4;
    std::uint16_t port = 0;
    std::uint32_t connect_timeout_ms = 3000;
};
class TcpClient {
public:
    TcpClient(StatusStore& status, EventLog& log);
    ~TcpClient();
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;
    bool start(TcpEndpoint endpoint);
    void request_stop() noexcept;
    void join();
    bool done() const noexcept { return done_.load(); }
private:
    void run(const TcpEndpoint& endpoint);
    StatusStore& status_; EventLog& log_;
    void* stop_ = nullptr;
    std::thread worker_;
    std::atomic<bool> done_{true};
};
class PipeServer {
public:
    PipeServer(StatusStore& status, EventLog& log);
    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;
    bool start(std::wstring name);
    void request_stop() noexcept;
    void join();
    bool done() const noexcept { return done_.load(); }
private:
    void run(const std::wstring& name);
    StatusStore& status_; EventLog& log_;
    void* stop_ = nullptr;
    std::thread worker_;
    std::atomic<bool> done_{true};
};
}
