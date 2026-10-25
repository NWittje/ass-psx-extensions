#pragma once
#include "transport.hpp"
#include "instrument.hpp"
#include "diagnostics.hpp"
#include <psx/network/io.hpp>
#include <functional>
#include <optional>
namespace psx::du::transport {
namespace tcp {
inline constexpr uint32_t magic=0x31545544;
inline constexpr uint16_t version=1;
inline constexpr size_t header_bytes=32,control_limit=4096,subscriber_limit=8;
using Identity=std::array<uint8_t,16>;
enum class Type:uint16_t { subscribe=1,accept,data,ack,status,error,instrument,instrument_ack,instrument_status };
enum class Disposition:uint32_t { accepted=1,replaced=2,budget=3 };
struct Header {Type type{};uint32_t bytes{};Identity connection{};uint16_t protocol{1};};
struct Ack {psx::norm::Correlation token;uint32_t unit{};Disposition disposition{};uint64_t received{},clock{};};
std::array<uint8_t,32> encode_header(const Header&);
Header decode_header(std::span<const uint8_t>);
std::array<uint8_t,64> encode_ack(const Ack&);
Ack decode_ack(std::span<const uint8_t>);
Identity identity();
struct Packet {
 Owned envelope,payload;psx::norm::Correlation token;uint64_t end{},revision{};
 bool instrument{};
 size_t bytes()const noexcept{return envelope->size()+(payload?payload->size():0);}
};
using Frame=std::shared_ptr<const Packet>;
Frame packet(const Envelope&,Owned,uint64_t revision=0);
Frame instrument_packet(const InstrumentEnvelope&,Owned,uint64_t revision=0);
InstrumentEnvelope decode_instrument_parts(std::span<const uint8_t>,std::span<const uint8_t>);
Owned own(std::span<const uint8_t>);
std::shared_ptr<Bytes> allocate(size_t);
Envelope decode_parts(std::span<const uint8_t> envelope,std::span<const uint8_t> payload);
struct Snapshot {
 uint64_t instrument_submitted{},instrument_acknowledged{},instrument_rejected{},instrument_expired{};
 uint64_t submitted{},acknowledged{},replaced{},expired{},reconnects{},rejected{},bytes{},partial{},released{},active{},peak{},cancelled{},enqueued{},pending_replaced{},mailbox_replaced{},capacity_used{};
 int send_buffer{},receive_buffer{},socket_error{};
 psx::norm::NativeSnapshot native;
};
struct ServerOptions {std::string address;uint16_t port{47480};unsigned freshness_ms{2000};Identity session{};uint16_t maximum_protocol{2};bool allow_wildcard{};};
struct Selection {Frame frame,status;Origin origin;Frame instrument_frame,instrument_status;Origin instrument_origin;};
// All network operations except notify() belong to one binding owner thread.
class Server {
 struct Impl;std::unique_ptr<Impl> impl_;
public:
 Server(unsigned binding,std::atomic<unsigned>& aggregate);
 ~Server();Server(const Server&)=delete;
 void start(const ServerOptions&,std::function<Selection(unsigned)>);
 void start(const ServerOptions&,std::function<Selection(unsigned)>,std::function<void(int)> instrument_interest);
 void observe(Trace*,unsigned producer);
 void events(psx::norm::DiagnosticSink,void*);
 void notify()noexcept;
 void pump(unsigned maximum_wait_ms=1000);
 void stop();Snapshot snapshot()const;
 psx::norm::NativeSnapshot native_snapshot()const noexcept;
};
struct ClientOptions {std::string host{"127.0.0.1"},local{"127.0.0.1"};uint16_t port{47480};unsigned unit{};uint16_t protocol{1};bool instrument{};};
struct ClientCallbacks {
 // Borrowed storage remains alive through completion/cancellation. No decoder
 // can see it until complete() publishes it. abort() releases incomplete input.
 std::function<std::span<uint8_t>(size_t)> acquire;
 std::function<Disposition(const Envelope&,uint64_t first,uint64_t complete)> complete;
 std::function<void()> abort;
 std::function<void(const Envelope&)> status;
 std::function<bool()> stop;
 std::function<void(const Identity&,int send_buffer,int receive_buffer)> connected;
 std::function<void(std::string_view local,std::string_view remote)> endpoints;
 std::function<std::span<uint8_t>(size_t)> instrument_acquire;
 std::function<Disposition(const InstrumentEnvelope&,uint64_t first,uint64_t complete)> instrument_complete;
 std::function<void()> instrument_abort;
 std::function<void(const InstrumentEnvelope&)> instrument_status;
 std::function<void(bool)> instrument_capability;
};
// Called on the independent reader. The control HANDLE wakes pending I/O.
void receive(const ClientOptions&,HANDLE control,const ClientCallbacks&,Trace* = nullptr,
             psx::norm::NativeMonitor* = nullptr);
size_t payload_bytes()noexcept;size_t payload_peak()noexcept;
bool reserve_model(size_t)noexcept;void release_model(size_t)noexcept;
size_t model_bytes()noexcept;
}
struct TcpHostOptions {std::string ethernet;uint32_t pid{};uint64_t created{};unsigned mask{63};bool enabled{true};uint16_t port{47480};unsigned freshness_ms{2000};bool all_interfaces{};bool operator==(const TcpHostOptions&)const=default;};
struct TcpHostView {std::array<LocalView,6> units;LocalView instrument;uint64_t instrument_received{},instrument_replaced{},instrument_invalid{};uint64_t published{},replaced{},expired{},rejected{},owned_peak{},prepared{},prepare_max_qpc{},diagnostic_lost{};size_t payload_bytes{},payload_peak{};bool ethernet{},loopback{},diagnostics_failed{};std::array<tcp::Snapshot,2> bindings;std::string error;};
class TcpHostPublisher {
 struct Impl;std::unique_ptr<Impl> impl_;
public:
 TcpHostPublisher();~TcpHostPublisher();void configure(TcpHostOptions);void reconnect();void request_stop()noexcept;void stop();
 void observe(std::shared_ptr<Trace>,std::filesystem::path source_output={});TcpHostView view()const;
 void events(psx::norm::DiagnosticSink,void*);
};
}
