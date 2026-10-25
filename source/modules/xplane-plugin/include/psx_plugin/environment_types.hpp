#pragma once
#include <psx/environment/protocol.hpp>
#include <psx/environment/presentation.hpp>
namespace psx::xp {
namespace env=psx::environment;
namespace envp=psx::environment::presentation;
enum class EnvironmentStage:unsigned {system_time,date,clock,settings,regional,readback,restoration,lease};
struct EnvironmentTiming {
    struct Slot {std::int64_t begin{},end{};std::uint64_t callback{},source{},target{},count{};std::uint32_t groups{};};
    std::array<Slot,8> stages{};std::uint64_t generation{},connection{};std::int64_t observed{};
};
struct EnvironmentCommand {env::Control control{};std::uint64_t connection{},revision{};std::int64_t received_qpc{};bool source_loss{};};
struct EnvironmentSample {env::Sample sample{};std::uint64_t connection{};std::array<std::uint64_t,2> revisions{};
    envp::Context context{};envp::VisibilityPolicy visibility{};std::uint64_t presentation_revision{};bool presentation_available{};
    envp::WeatherTarget weather{};envp::LocalVisibility local_visibility{};};
}
