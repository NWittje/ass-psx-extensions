#pragma once
#include <psx/sync/normalization.hpp>
#include <string>
namespace psx::sync {
struct Settings {
 bool enabled{},automatic_start{true};std::string host;
 std::uint16_t receive_port{},destination_port{};
 std::string psx_host{"127.0.0.1"};std::uint16_t main_port{10747},boost_port{10749};
 std::wstring path;
};
Settings read_settings(Role,const std::wstring& explicit_path={});
void save_connection(const Settings&); // Only XPlaneConnection and XPlaneHost.
bool start_runtime(Role,const Settings&,const std::wstring& qualified_dll={});
void stop_runtime()noexcept;
Endpoint* runtime()noexcept;
bool enabled()noexcept;
bool local_io_selected()noexcept;
std::int64_t freshness_now(std::int64_t)noexcept; // Atomic bound; never use as a presentation time.
// A worker-owned logical service over the shared NORM endpoint. The normalizer
// retains raw packet provenance while supplying a separate same-clock view.
class Port {
 Family family_;Endpoint::Binding binding_;Normalizer normalizer_;
public:
 explicit Port(Family f):family_(f){}
 ~Port();
 bool connected()const;
 bool changed()const;
 void attach();
 void detach()noexcept;
 LinkStatus status()const;
 bool send(std::span<const std::uint8_t>);
 bool receive(Frame&);
};
}
