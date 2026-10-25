#include <winsock2.h>
#include <ws2tcpip.h>
#include "coordinator/transport.hpp"
#include "coordinator/protocol.hpp"
#include "coordinator/win_handle.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace coordinator {
namespace {
struct Winsock {
    int result; Winsock() { WSADATA data{}; result = WSAStartup(MAKEWORD(2, 2), &data); }
    ~Winsock() { if (!result) WSACleanup(); }
};
struct Socket {
    SOCKET value = INVALID_SOCKET;
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
};
}
TcpClient::TcpClient(StatusStore& s, EventLog& l) : status_(s), log_(l), stop_(make_stop_event()) {}
TcpClient::~TcpClient() { request_stop(); join(); CloseHandle(stop_); }
bool TcpClient::start(TcpEndpoint endpoint) {
    if (worker_.joinable()) return false;
    IN_ADDR address{};
    if (!endpoint.port || !endpoint.connect_timeout_ms || endpoint.connect_timeout_ms > 60000 ||
        InetPtonA(AF_INET, endpoint.ipv4.c_str(), &address) != 1) return false;
    ResetEvent(stop_); done_ = false;
    status_.endpoint(Channel::tcp, endpoint.ipv4 + ":" + std::to_string(endpoint.port));
    status_.new_epoch(Channel::tcp); status_.state(Channel::tcp, State::starting);
    try {
        worker_ = std::thread([this, endpoint] {
            try { run(endpoint); }
            catch (...) { status_.state(Channel::tcp, State::error, ERROR_UNHANDLED_EXCEPTION); }
            done_ = true;
        });
    } catch (...) { done_ = true; status_.state(Channel::tcp, State::error, ERROR_NOT_ENOUGH_MEMORY); return false; }
    return true;
}
void TcpClient::request_stop() noexcept { SetEvent(stop_); }
void TcpClient::join() { if (worker_.joinable()) worker_.join(); }
void TcpClient::run(const TcpEndpoint& endpoint) {
    const auto fail = [this](DWORD e, const char* message) { status_.state(Channel::tcp, State::error, e); log_.add(Level::error, message); };
    const auto stopped = [this] { status_.state(Channel::tcp, State::stopped); log_.add(Level::info, "TCP stopped; no queued writes or replay"); };
    Winsock wsa;
    if (wsa.result) { fail(static_cast<DWORD>(wsa.result), "WSAStartup failed"); return; }
    Socket socket; socket.value = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket.value == INVALID_SOCKET) { fail(WSAGetLastError(), "TCP socket failed"); return; }
    WinHandle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!ready.valid()) { fail(GetLastError(), "TCP event failed"); return; }
    if (WSAEventSelect(socket.value, ready.get(), FD_CONNECT | FD_READ | FD_CLOSE) == SOCKET_ERROR) { fail(WSAGetLastError(), "WSAEventSelect failed"); return; }
    sockaddr_in remote{}; remote.sin_family = AF_INET; remote.sin_port = htons(endpoint.port);
    InetPtonA(AF_INET, endpoint.ipv4.c_str(), &remote.sin_addr);
    if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) { stopped(); return; }
    status_.state(Channel::tcp, State::connecting); log_.add(Level::info, "TCP connecting to explicit numeric endpoint");
    bool connected = connect(socket.value, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0;
    if (!connected && WSAGetLastError() != WSAEWOULDBLOCK) { fail(WSAGetLastError(), "TCP connect failed"); return; }
    const auto deadline = GetTickCount64() + endpoint.connect_timeout_ms;
    HANDLE waits[] = {stop_, ready.get()};
    if (connected) { status_.state(Channel::tcp, State::connected); log_.add(Level::info, "TCP connected; PSX state readiness is not established"); }
    LineFramer framer;
    const LineFramer::Sink line = [this](std::string_view text) {
        status_.frame(Channel::tcp);
        // Bounded preview is diagnostic only, not a retained PSX state registry.
        std::array<char, 180> preview{};
        constexpr char prefix[] = "TCP RX: ";
        std::copy(prefix, prefix + sizeof(prefix) - 1, preview.begin());
        const auto n = (std::min)(text.size(), preview.size() - sizeof(prefix));
        std::copy_n(text.data(), n, preview.data() + sizeof(prefix) - 1);
        log_.add(Level::info, preview.data());
    };
    std::array<char, 8192> buffer{};
    for (;;) {
        DWORD timeout = INFINITE;
        if (!connected) {
            const auto now = GetTickCount64();
            if (now >= deadline) { fail(WSAETIMEDOUT, "TCP connect timeout"); return; }
            timeout = static_cast<DWORD>(deadline - now);
        }
        const auto wait = WaitForMultipleObjects(2, waits, FALSE, timeout);
        if (wait == WAIT_OBJECT_0) { if (!framer.complete()) status_.truncated(Channel::tcp); stopped(); return; }
        if (wait == WAIT_TIMEOUT) { fail(WSAETIMEDOUT, "TCP connect timeout"); return; }
        if (wait != WAIT_OBJECT_0 + 1) { fail(GetLastError(), "TCP wait failed"); return; }
        WSANETWORKEVENTS events{};
        if (WSAEnumNetworkEvents(socket.value, ready.get(), &events) == SOCKET_ERROR) { fail(WSAGetLastError(), "TCP event enumeration failed"); return; }
        if (events.lNetworkEvents & FD_CONNECT) {
            if (events.iErrorCode[FD_CONNECT_BIT]) { fail(events.iErrorCode[FD_CONNECT_BIT], "TCP refused or unavailable"); return; }
            connected = true; status_.state(Channel::tcp, State::connected);
            log_.add(Level::info, "TCP connected; PSX state readiness is not established");
        }
        if (!(events.lNetworkEvents & (FD_READ | FD_CLOSE))) continue;
        if (events.iErrorCode[FD_READ_BIT]) { fail(events.iErrorCode[FD_READ_BIT], "TCP read notification failed"); return; }
        for (;;) {
            if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) { if (!framer.complete()) status_.truncated(Channel::tcp); stopped(); return; }
            const auto n = recv(socket.value, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (n > 0) {
                status_.received(Channel::tcp, static_cast<std::size_t>(n));
                if (!framer.feed(buffer.data(), static_cast<std::size_t>(n), line)) { fail(ERROR_INVALID_DATA, "TCP malformed ASCII/CRLF or oversized line; disconnected"); return; }
            } else if (!n) {
                if (!framer.complete()) { status_.truncated(Channel::tcp); fail(ERROR_HANDLE_EOF, "TCP EOF discarded an incomplete line"); }
                else { status_.state(Channel::tcp, State::disconnected); log_.add(Level::info, "TCP EOF; reconnect requires an explicit start"); }
                return;
            } else {
                const auto error = WSAGetLastError();
                if (error != WSAEWOULDBLOCK) { if (!framer.complete()) status_.truncated(Channel::tcp); fail(error, "TCP receive failed"); return; }
                break;
            }
        }
        if (events.lNetworkEvents & FD_CLOSE) {
            if (!framer.complete()) status_.truncated(Channel::tcp);
            if (events.iErrorCode[FD_CLOSE_BIT]) fail(events.iErrorCode[FD_CLOSE_BIT], "TCP remote close error");
            else if (!framer.complete()) fail(ERROR_HANDLE_EOF, "TCP close discarded an incomplete line");
            else status_.state(Channel::tcp, State::disconnected);
            return;
        }
    }
}
}
