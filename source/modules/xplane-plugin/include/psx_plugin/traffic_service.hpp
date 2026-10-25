#pragma once
#include <psx/traffic_visual/presentation.hpp>
#include <memory>
#include <string>
namespace psx::xp {
namespace tv=traffic_visual;
struct TrafficCommand {tv::Claim claim{};std::uint64_t transport{};std::int64_t received{};};
struct TrafficWindow {tv::Window window{};std::uint64_t transport{},lease{};};
std::wstring traffic_endpoint();
class TrafficService final {
public:
 TrafficService();~TrafficService();
 bool start(motion::InstanceId,const std::wstring& endpoint={});void stop() noexcept;
 bool connected() const noexcept;std::uint64_t connection() const noexcept;
 bool command(TrafficCommand&) noexcept;bool window(TrafficWindow&) noexcept;
 void ack(const tv::Ack&,std::uint64_t) noexcept;void publish(const tv::Report&,std::uint32_t capabilities) noexcept;
 bool capture(tv::Capture&) noexcept;void detail(tv::Detail) noexcept;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
