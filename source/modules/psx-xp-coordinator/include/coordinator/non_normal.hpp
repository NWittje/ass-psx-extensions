#pragma once
#include <psx/non_normal/protocol.hpp>
#include <psx/environment/presentation.hpp>
#include <memory>
#include <string>

namespace coordinator {
namespace non_normal_wire=psx::non_normal;
struct VisibilityIpcEvent {
 enum class Kind {open,request,lost};Kind kind{Kind::request};
 std::uint64_t epoch{},session{};psx::environment::presentation::VisibilityRequest request{};
};
struct NonNormalView {
 bool ios_connected{},plugin_connected{},report_current{};
 std::uint64_t ios_epoch{},plugin_epoch{},accepted{},duplicates{},obsolete{},faults{};
 non_normal_wire::ProcessIdentity selected{},ios{},psx{};
 non_normal_wire::Result report{};
};
// One dedicated acquisition owner. IPC workers only exchange bounded values.
class NonNormalModel {
 struct Impl;std::unique_ptr<Impl> impl_;
public:
 NonNormalModel();~NonNormalModel();
 NonNormalModel(const NonNormalModel&)=delete;NonNormalModel& operator=(const NonNormalModel&)=delete;
 void configure(non_normal_wire::ProcessIdentity,std::uint64_t);
 void ios_open(std::uint64_t,non_normal_wire::ProcessIdentity,std::int64_t);
 void ios_close(std::uint64_t,std::int64_t);
 void selected(non_normal_wire::ProcessIdentity,std::int64_t);
 void associate_psx(non_normal_wire::ProcessIdentity);
 void plugin_open(std::uint64_t,non_normal_wire::ProcessIdentity,std::uint64_t,std::int64_t);
 void plugin_close(std::uint64_t,std::int64_t);
 bool request(std::uint64_t,const non_normal_wire::Request&,std::int64_t);
 bool sent(std::uint64_t,const non_normal_wire::Request&,std::int64_t);
 bool uncertain_write(std::uint64_t,const non_normal_wire::Request&);
 bool result(std::uint64_t,const non_normal_wire::Result&,std::int64_t);
 void tick(std::int64_t);
 bool next_command(std::uint64_t&,non_normal_wire::Request&);
 bool next_result(std::uint64_t&,non_normal_wire::Result&);
 bool take_retirement(non_normal_wire::Request&)noexcept;
 NonNormalView view()const;
};
class NonNormalService {
 struct Impl;std::unique_ptr<Impl> impl_;
public:
 NonNormalService();~NonNormalService();
 NonNormalService(const NonNormalService&)=delete;NonNormalService& operator=(const NonNormalService&)=delete;
 bool start();void select(non_normal_wire::ProcessIdentity);
 void associate_psx(non_normal_wire::ProcessIdentity);
 void request_stop()noexcept;void join();bool done()const noexcept;
 NonNormalView snapshot()const;
 bool pop_visibility(VisibilityIpcEvent&);
 bool visibility_result(std::uint64_t,const psx::environment::presentation::VisibilityResult&);
 std::uintptr_t visibility_ready_event()const noexcept;
 // Authored endpoints must be chosen before start; never a product preference.
 void fixture_endpoints(std::wstring ios,std::wstring plugin);
};
}
