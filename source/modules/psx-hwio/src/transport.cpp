#include "hwio/catalogue.hpp"
#include "hwio/transport.hpp"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace hwio {
Transport::~Transport() {
    stop();
    if (available_event)
        CloseHandle(available_event);
    CloseHandle(wake_event_);
}
void Transport::start(std::string host, std::uint16_t port, bool autoconnect,
                      psx::diagnostics::DiagnosticProducer log) {
    host_ = std::move(host);
    port_ = port;
    desired_ = autoconnect;
    log_ = log;
    stop_ = false;
    thread_ = std::thread(&Transport::run, this);
}
void Transport::connect(std::string host, std::uint16_t port) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        host_ = std::move(host);
        port_ = port;
    }
    reset_ = true;
    desired_ = true;
    wake_.notify_one();
    SetEvent(wake_event_);
}
void Transport::disconnect() {
    desired_ = false;
    reset_ = true;
    wake_.notify_one();
    SetEvent(wake_event_);
}
void Transport::resynchronize() {
    reset_ = true;
    wake_.notify_one();
    SetEvent(wake_event_);
}
void Transport::cancel_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    all_cancelled_ = ++cancellation_generation_;
    cancelled_bindings_.clear();
    SetEvent(wake_event_);
}
void Transport::cancel_binding(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const auto &c) { return c.binding == id; }),
                 queue_.end());
    cancelled_bindings_[id] = ++cancellation_generation_;
    SetEvent(wake_event_);
}
void Transport::supersede_axes(const std::vector<RecordKey> &records) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &record : records) {
        const auto key = record_name(record);
        superseded_records_[key] = ++cancellation_generation_;
        queue_.erase(
            std::remove_if(queue_.begin(), queue_.end(),
                           [&](const auto &c) { return !c.event && c.line.substr(0, c.line.find('=')) == key; }),
            queue_.end());
    }
    SetEvent(wake_event_);
}
void Transport::supersede_records(const std::vector<RecordKey>& records) {
    std::lock_guard<std::mutex> lock(mutex_);
    for(const auto key:records){const auto name=record_name(key);superseded_events_[name]=++cancellation_generation_;
        queue_.erase(std::remove_if(queue_.begin(),queue_.end(),[&](const auto& c){return c.line.substr(0,c.line.find('='))==name;}),queue_.end());}
    SetEvent(wake_event_);
}
bool Transport::drain(unsigned timeout_ms) {
    const auto until = GetTickCount64() + timeout_ms;
    do {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty() && !pending_)
                return true;
        }
        Sleep(1);
    } while (GetTickCount64() < until);
    return false;
}
void Transport::stop() {
    stop_ = true;
    wake_.notify_one();
    SetEvent(wake_event_);
    if (thread_.joinable())
        thread_.join();
}
TransportStatus Transport::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result=status_;
    const auto blocked=write_blocked_since_.load();
    result.write_stall_ms=blocked?GetTickCount64()-blocked:0;
    result.overflows=overflow.load();
    return result;
}
void Transport::interests(const std::vector<RecordKey> &records) {
    std::lock_guard<std::mutex> lock(mutex_);
    interests_.clear();
    for(const auto* key:{"Qs120","Qs483","Qi129","Qh398","Qh399","Qh396","Qs425","Qs436","Qh388","Qi119","Qi226","Qh386"})interests_.insert(key);
    // Oversized observed records must invalidate physical-output sources as
    // well as command destinations; retaining their previous value is unsafe.
    const auto& observed=active_catalogue().observed_controls;
    interests_.insert(observed.begin(),observed.end());
    for (const auto &key : records)
        interests_.insert(record_name(key));
}
void Transport::set_status(std::string message, unsigned error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.message != message || status_.error != error)
        log_.emit(error ? psx::diagnostics::Code::io_failure : psx::diagnostics::Code::message,
                  error ? psx::diagnostics::Severity::error : psx::diagnostics::Severity::info, {},
                  error ? psx::diagnostics::ErrorDomain::winsock : psx::diagnostics::ErrorDomain::none, error, message);
    status_.message = std::move(message);
    status_.error = error;
    status_.enabled = desired_;
    status_.epoch = epoch_;
}
bool Transport::submit(Command c) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!status_.connected || !status_.ready || c.epoch != active_epoch_)
        return false;
    if (c.line.empty() || c.line.size() > 4095 || c.line.back() != '\n' || c.line.find('\n') != c.line.size() - 1)
        return false;
    if (c.follower_mask && !follower_allowed_) {
        ++status_.dropped;
        return true;
    }
    c.cancellation_generation = cancellation_generation_;
    if (!c.event) {
        const auto eq = c.line.find('=');
        for (auto it = queue_.rbegin(); it != queue_.rend(); ++it) {
            if (it->event)
                break;
            if (it->line.substr(0, it->line.find('=')) == c.line.substr(0, eq)) {
                *it = std::move(c);
                SetEvent(wake_event_);
                return true;
            }
        }
    }
    if (queue_.size() >= 256) {
        ++status_.dropped;
        ++overflow;
        reset_ = true;
        SetEvent(wake_event_);
        return false;
    }
    queue_.push_back(std::move(c));
    wake_.notify_one();
    SetEvent(wake_event_);
    return true;
}
bool Transport::event(NetworkEventKind kind, std::string_view text, std::uint64_t binding, const Command *c) {
    NetworkEvent e;
    e.kind = kind;
    e.epoch = epoch_;
    e.binding = binding;
    e.qpc = psx::diagnostics::qpc_now();
    if (c) {
        e.input_qpc = c->input_qpc;
        e.mapped_qpc = c->mapped_qpc;
        e.value = c->value;
    }
    const auto n = std::min(text.size(), e.text.size() - 1);
    std::memcpy(e.text.data(), text.data(), n);
    if (!incoming.push(e)) {
        ++overflow;
        reset_ = true;
        return false;
    }
    SetEvent(available_event);
    return true;
}
void Transport::run() {
    using namespace std::chrono;
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        set_status("Winsock initialization failed", WSAGetLastError());
        return;
    }
    SOCKET socket = INVALID_SOCKET;
    PendingWrite pending;
    std::string receive;
    receive.reserve(131072);
    bool synchronized = false;
    unsigned backoff = 1;
    int load_stage = 0;
    bool identified = false, versioned = false;
    const WSAEVENT socket_event = WSACreateEvent();
    auto retry = steady_clock::now(), sync_started = retry;
    bool connecting = false, write_blocked = false;
    std::map<std::string, steady_clock::time_point> sent_records;
    const auto remaining = [&](const Command &command) {
        if (!command.minimum_interval_ms) return 0u;
        const auto previous=sent_records.find(command.line.substr(0,command.line.find('=')));
        if(previous==sent_records.end())return 0u;
        const auto us=duration_cast<microseconds>(previous->second+milliseconds(command.minimum_interval_ms)-steady_clock::now()).count();
        return us>0?static_cast<unsigned>((us+999)/1000):0u;
    };
    auto close = [&](std::string message, unsigned error) {
        write_blocked_since_=0;
        if (socket != INVALID_SOCKET) {
            if (pending.offset == 0 || pending.complete()) {
                const char exit[] = "exit\n";
                ::send(socket, exit, 5, 0);
            }
            closesocket(socket);
            socket = INVALID_SOCKET;
        }
        pending = {};
        sent_records.clear();
        pending_ = false;
        follower_allowed_ = false;
        write_blocked = false;
        receive.clear();
        connecting = false;
        synchronized = false;
        load_stage = 0;
        identified = false;
        versioned = false;
        ++epoch_;
        active_epoch_ = epoch_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.clear();
            status_.connected = false;
            status_.ready = false;
        }
        event(NetworkEventKind::boundary);
        if (!error && message != "Disconnected by user" && message != "Reconnecting" && message != "Stopped")
            log_.emit(psx::diagnostics::Code::disconnected, psx::diagnostics::Severity::warning, {},
                      psx::diagnostics::ErrorDomain::none, 0, message);
        set_status(std::move(message), error);
    };
    while (!stop_) {
        if (reset_.exchange(false)) {
            close(desired_ ? "Reconnecting" : "Disconnected by user", 0);
            retry = steady_clock::now();
            backoff = 1;
        }
        if (!desired_) {
            if (socket != INVALID_SOCKET)
                close("Disconnected by user", 0);
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, milliseconds(20));
            continue;
        }
        if (socket == INVALID_SOCKET) {
            if (steady_clock::now() < retry) {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait_for(lock, milliseconds(20));
                continue;
            }
            std::string host;
            std::uint16_t port;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                host = host_;
                port = port_;
                ++status_.retries;
            }
            // Numeric IPv4 and IPv6 endpoints avoid blocking DNS on shutdown.
            sockaddr_storage address{};
            int length = 0;
            auto a4 = reinterpret_cast<sockaddr_in *>(&address);
            auto a6 = reinterpret_cast<sockaddr_in6 *>(&address);
            if (InetPtonA(AF_INET, host.c_str(), &a4->sin_addr) == 1) {
                a4->sin_family = AF_INET;
                a4->sin_port = htons(port);
                length = sizeof(sockaddr_in);
            } else if (InetPtonA(AF_INET6, host.c_str(), &a6->sin6_addr) == 1) {
                a6->sin6_family = AF_INET6;
                a6->sin6_port = htons(port);
                length = sizeof(sockaddr_in6);
            } else {
                set_status("Endpoint must be a numeric IPv4/IPv6 address", WSAEINVAL);
                retry = steady_clock::now() + seconds(8);
                continue;
            }
            socket = ::socket(address.ss_family, SOCK_STREAM, IPPROTO_TCP);
            if (socket == INVALID_SOCKET) {
                set_status("Socket creation failed", WSAGetLastError());
                retry = steady_clock::now() + seconds(backoff);
                backoff = std::min(backoff * 2, 8u);
                continue;
            }
            u_long nonblocking = 1;
            ioctlsocket(socket, FIONBIO, &nonblocking);
            BOOL no_delay = TRUE;
            setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&no_delay), sizeof(no_delay));
            if (send_buffer_ > 0)
                setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&send_buffer_),
                           sizeof(send_buffer_));
            WSAEventSelect(socket, socket_event, FD_READ | FD_WRITE | FD_CONNECT | FD_CLOSE);
            ++epoch_;
            active_epoch_ = epoch_;
            event(NetworkEventKind::boundary);
            sync_started = steady_clock::now();
            const int result = ::connect(socket, reinterpret_cast<sockaddr *>(&address), length);
            if (result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
                const unsigned error = WSAGetLastError();
                close("Connection failed; retry scheduled", error);
                retry = steady_clock::now() + seconds(backoff);
                backoff = std::min(backoff * 2, 8u);
                continue;
            }
            connecting = true;
            set_status("Connecting");
        }
        bool runnable = false;
        unsigned paced_wait=1000;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::set<std::string> blocked_records;
            for(const auto &command:queue_) {
                const auto key=command.line.substr(0,command.line.find('='));
                if(!blocked_records.insert(key).second)continue;
                const auto delay=remaining(command);
                if(!delay)runnable=true;
                else paced_wait=std::min(paced_wait,delay);
            }
        }
        runnable = (runnable || !pending.command.line.empty()) && !write_blocked;
        const HANDLE waits[] = {wake_event_, socket_event};
        WaitForMultipleObjects(2, waits, FALSE, runnable ? 0 : connecting ? 100 : paced_wait);
        WSANETWORKEVENTS network_events{};
        WSAEnumNetworkEvents(socket, socket_event, &network_events);
        if (network_events.lNetworkEvents & FD_WRITE)
            write_blocked = false;
        if (stop_ || reset_)
            continue;
        fd_set reads, writes, errors;
        FD_ZERO(&reads);
        FD_ZERO(&writes);
        FD_ZERO(&errors);
        FD_SET(socket, &reads);
        FD_SET(socket, &errors);
        FD_SET(socket, &writes);
        timeval timeout{0, 0};
        const int selected = select(0, &reads, &writes, &errors, &timeout);
        if (selected == SOCKET_ERROR || FD_ISSET(socket, &errors)) {
            unsigned error = WSAGetLastError();
            int option{}, size = sizeof(option);
            getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&option), &size);
            if (option)
                error = static_cast<unsigned>(option);
            close("Socket error; retry scheduled", error);
            retry = steady_clock::now() + seconds(backoff);
            backoff = std::min(backoff * 2, 8u);
            continue;
        }
        if (connecting) {
            if (FD_ISSET(socket, &writes)) {
                connecting = false;
                backoff = 1;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.connected = true;
                }
                set_status("Connected; waiting for initial records");
                log_.emit(psx::diagnostics::Code::connected);
            } else {
                if (steady_clock::now() - sync_started > seconds(5)) {
                    close("Connection timed out", WSAETIMEDOUT);
                    retry = steady_clock::now() + seconds(backoff);
                    backoff = std::min(backoff * 2, 8u);
                }
                continue;
            }
            // Connect and startup bytes can arrive in the same event snapshot.
            // EnumNetworkEvents consumed FD_READ; recv must service/re-enable it
            // before another wait, even when the connection has just completed.
        }
        bool broken = false;
        if (FD_ISSET(socket, &reads)) {
            char buffer[16384];
            const int n = recv(socket, buffer, sizeof(buffer), 0);
            if (n == 0 || (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) {
                close("Peer disconnected; retry scheduled", n ? WSAGetLastError() : 0);
                retry = steady_clock::now() + seconds(1);
                continue;
            }
            if (n > 0) {
                receive.append(buffer, static_cast<std::size_t>(n));
                std::size_t pos = 0;
                for (;;) {
                    auto end = receive.find('\n', pos);
                    if (end == receive.npos)
                        break;
                    std::string line = receive.substr(pos, end - pos);
                    pos = end + 1;
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    if (line.size() >= 4096) {
                        const auto key = line.substr(0, line.find('='));
                        bool relevant = false;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            relevant = interests_.count(key) != 0;
                            if (relevant)
                                queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                                            [&](const auto &c) {
                                                                return c.line.substr(0, c.line.find('=')) == key;
                                                            }),
                                             queue_.end());
                        }
                        if (relevant) {
                            if (pending.command.line.substr(0, pending.command.line.find('=')) == key) {
                                if (pending.offset) {
                                    close("Oversize target interrupted partial record", WSAEMSGSIZE);
                                    broken = true;
                                    break;
                                }
                                pending = {};
                                pending_ = false;
                            }
                            event(NetworkEventKind::unavailable, key);
                        }
                        continue;
                    }
                    if (line == "exit") {
                        close("Server exit", 0);
                        broken = true;
                        break;
                    }
                    if (line == "load1") {
                        // Readiness below blocks writes during loading. Keep
                        // unchanged native A/T authority on the same socket;
                        // PSX need not resend an ECON value that did not change.
                        if (!identified || !versioned) {
                            close("Load before identification", WSAEPROTONOSUPPORT);
                            broken = true;
                            break;
                        }
                        if (!pending.command.line.empty() && pending.offset > 0) {
                            close("Load interrupted partial record; reconnecting", 0);
                            broken = true;
                            break;
                        }
                        pending = {};
                        ++epoch_;
                        active_epoch_ = epoch_;
                        synchronized = false;
                        // A new load owns a fresh synchronization allowance. A
                        // connected socket alone does not qualify cockpit state.
                        sync_started = steady_clock::now();
                        load_stage = 1;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            queue_.clear();
                            status_.ready = false;
                        }
                        event(NetworkEventKind::load_boundary);
                        set_status("Loading; waiting for ordered load2/load3");
                    } else if (line == "load2") {
                        if (load_stage != 1) {
                            close("Unexpected load2", WSAEPROTONOSUPPORT);
                            broken = true;
                            break;
                        }
                        load_stage = 2;
                    } else if (line == "load3") {
                        if (load_stage != 2) {
                            close("Unexpected load3", WSAEPROTONOSUPPORT);
                            broken = true;
                            break;
                        }
                        load_stage = 3;
                        synchronized = true;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.ready = true;
                        }
                        event(NetworkEventKind::ready);
                        set_status("Connected and synchronized");
                    } else if (line.rfind("id=", 0) == 0) {
                        int id{};
                        identified = integer(std::string_view(line).substr(3), id) && id >= 0;
                    } else if (line.rfind("version=", 0) == 0) {
                        versioned = line.size() > 8;
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.version = line.substr(8);
                    } else if (line.starts_with(active_catalogue().authority_prefix)) {
                        const bool physical=active_catalogue().authority(std::string_view(line).substr(line.find('=')+1))==AtAuthority::physical;
                        follower_allowed_ = physical;
                        if (!physical) {
                            bool partial = false;
                            {
                                std::lock_guard<std::mutex> lock(mutex_);
                                queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                                            [](const auto &c) { return c.follower_mask != 0; }),
                                             queue_.end());
                            }
                            if (pending.command.follower_mask) {
                                partial = pending.offset != 0;
                                pending = {};
                            }
                            if (partial) {
                                close("A/T transition interrupted partial follower record; reconnecting", 0);
                                broken = true;
                                break;
                            }
                        }
                        event(NetworkEventKind::line, line);
                    } else if (!line.empty() && line[0] == 'Q')
                        event(NetworkEventKind::line, line);
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        ++status_.received;
                    }
                }
                if (broken) {
                    retry = steady_clock::now() + seconds(1);
                    continue;
                }
                receive.erase(0, pos);
                if (receive.size() > 100128) {
                    close("Incoming line exceeded PSX maximum record bound", WSAEMSGSIZE);
                    retry = steady_clock::now() + seconds(1);
                    continue;
                }
            }
        }
        if (!synchronized && steady_clock::now() - sync_started > seconds(10)) {
            close("PSX load synchronization timed out", WSAETIMEDOUT);
            retry = steady_clock::now() + seconds(1);
            continue;
        }
        // A handover may replace an unstarted axis record, never splice an in-flight line.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto key = pending.command.line.substr(0, pending.command.line.find('='));
            const auto newer = superseded_records_.find(key);
            const auto any_newer=superseded_events_.find(key);
            if(!pending.offset && any_newer!=superseded_events_.end() && pending.command.cancellation_generation<any_newer->second){pending={};pending_=false;}
            if (!pending.offset && !pending.command.event && newer != superseded_records_.end() &&
                pending.command.cancellation_generation < newer->second) {
                pending = {};
                pending_ = false;
            }
        }
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto binding_cancel = cancelled_bindings_.find(pending.command.binding);
            cancelled =
                !pending.command.line.empty() && (pending.command.cancellation_generation < all_cancelled_ ||
                                                  (!pending.offset && binding_cancel != cancelled_bindings_.end() &&
                                                   pending.command.cancellation_generation < binding_cancel->second));
        }
        if (cancelled && !pending.offset) {
            pending = {};
            pending_ = false;
            write_blocked = false;
            write_blocked_since_=0;
        }
        if (pending.command.line.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            std::set<std::string> blocked_records;
            for(auto it=queue_.begin();it!=queue_.end();) {
                if(it->epoch!=epoch_){it=queue_.erase(it);continue;}
                const auto key=it->line.substr(0,it->line.find('='));
                if(!blocked_records.insert(key).second||remaining(*it)){++it;continue;}
                pending.command=std::move(*it);queue_.erase(it);pending.offset=0;break;
            }
            pending_ = !pending.command.line.empty();
        }
        if (!pending.command.line.empty() && !write_blocked) {
            const int n =
                ::send(socket, pending.command.line.data() + pending.offset,
                       static_cast<int>(std::min(pending.command.line.size() - pending.offset, write_limit_)), 0);
            if (n > 0) {
                write_blocked_since_=0;
                pending.advance(static_cast<std::size_t>(n));
                if (pending.complete()) {
                    sent_records[pending.command.line.substr(0,pending.command.line.find('='))]=steady_clock::now();
                    event(NetworkEventKind::sent, pending.command.line, pending.command.binding, &pending.command);
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        ++status_.sent;
                        pending_ = false;
                    }
                    pending = {};
                }
            } else if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
                close("Send failed; retry scheduled", WSAGetLastError());
                retry = steady_clock::now() + seconds(1);
            } else {
                write_blocked = true;
                if(!write_blocked_since_)write_blocked_since_=GetTickCount64();
            }
        }
    }
    close("Stopped", 0);
    WSACloseEvent(socket_event);
    WSACleanup();
}
} // namespace hwio
