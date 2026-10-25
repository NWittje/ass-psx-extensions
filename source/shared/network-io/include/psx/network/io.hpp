#pragma once
#include <WinSock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace psx::network {
struct Endpoint {
    sockaddr_storage address{};
    int length{};
    static Endpoint parse(const std::string& host,std::uint16_t port);
    std::string host() const;
    std::uint16_t port() const noexcept;
    bool same(const Endpoint&) const noexcept;
};
struct Adapter {
    std::wstring guid, name, description;
    std::uint64_t luid{};
    unsigned index{}, ipv6_index{};
    bool up{}, wired{}, hardware{};
    std::vector<Endpoint> addresses;
    // Prefix entries follow addresses; display selection persists the network, not NIC identity.
    std::vector<unsigned> prefixes;
    bool multicast{true};
    unsigned metric{};
};
struct Inventory {std::vector<Adapter> adapters;std::uint64_t revision{};std::string error;};
bool same_binding(const Adapter&, const Adapter&) noexcept;
enum class InventoryPolicy { physical, display };
std::vector<Adapter> enumerate(InventoryPolicy = InventoryPolicy::physical);
std::string host_ipv4(std::string_view);
// Normalized CIDR keys; labels may use wildcard octets for byte-aligned prefixes.
std::string subnet_key(const Endpoint&,unsigned prefix);
std::string subnet_label(std::string_view);
std::vector<std::string> subnets(const Inventory&);
std::vector<Adapter> subnet_adapters(const Inventory&,std::string_view);
Adapter route_ipv4(const Inventory&,const Endpoint&,std::string_view subnet = {});
class Ipv4Resolver {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    Ipv4Resolver();~Ipv4Resolver();
    Ipv4Resolver(const Ipv4Resolver&)=delete;
    void start(std::string_view);
    bool ready() const noexcept;
    std::vector<Endpoint> take();
    HANDLE event() const noexcept;
    void cancel() noexcept;
};
std::vector<Endpoint> resolve_ipv4(std::string_view,HANDLE stop=nullptr);

std::wstring initial_adapter(const Inventory&);
std::wstring initial_ipv4_adapter(const Inventory&);
class AdapterMonitor {
    std::thread worker_;
    HANDLE changed_{CreateEventW(nullptr,FALSE,FALSE,nullptr)}, stop_{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    HANDLE interface_notification_{},address_notification_{},route_notification_{};
    InventoryPolicy policy_;
    mutable std::mutex mutex_;
    std::shared_ptr<const Inventory> inventory_;
    void run();
public:
    HANDLE available{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    std::atomic<std::uint64_t> revision{};
    explicit AdapterMonitor(InventoryPolicy = InventoryPolicy::physical);
    ~AdapterMonitor();
    AdapterMonitor(const AdapterMonitor&)=delete;
    AdapterMonitor& operator=(const AdapterMonitor&)=delete;
    std::shared_ptr<const Inventory> snapshot() const;
};
enum class State {closed,connecting,connected,failed};
// Single-owner, bounded nonblocking TCP stream. Framing belongs to the provider.
// Each instance contributes one event to the owner's bounded unified wait.
class Stream {
    SOCKET socket_{INVALID_SOCKET};
    WSAEVENT event_{};
    State state_{State::closed};
    int error_{};
    bool closing_{};
    std::array<std::uint8_t,8192> tx_{},rx_{};
    std::size_t tx_begin_{},tx_end_{},rx_end_{};
    void fail(int) noexcept;
    void flush() noexcept;
public:
    Stream();
    ~Stream();
    Stream(const Stream&)=delete;
    Stream& operator=(const Stream&)=delete;
    bool connect(const Adapter&,const Endpoint&) noexcept;
    void close() noexcept;
    void pump() noexcept;
    bool send(std::span<const std::uint8_t>) noexcept;
    std::span<const std::uint8_t> received() const noexcept {return {rx_.data(),rx_end_};}
    void consume(std::size_t) noexcept;
    bool sending() const noexcept {return tx_begin_!=tx_end_;}
    HANDLE event() const noexcept {return event_;}
    State state() const noexcept {return state_;}
    int error() const noexcept {return error_;}
};
struct Datagram {Endpoint source;std::array<std::uint8_t,2048> bytes{};std::size_t size{};};
class Udp {
    SOCKET socket_{INVALID_SOCKET};
    WSAEVENT event_{};
    unsigned index_{};
    int family_{AF_INET};
    void* receive_message_{};
public:
    Udp();
    ~Udp();
    Udp(const Udp&)=delete;
    Udp& operator=(const Udp&)=delete;
    bool open(const Adapter&,int family=AF_INET,std::uint16_t local_port=0) noexcept;
    void close() noexcept;
    bool send(const Endpoint&,std::span<const std::uint8_t>) noexcept;
    bool receive(Datagram&) noexcept;
    HANDLE event() const noexcept {return event_;}
};
} // namespace psx::network
