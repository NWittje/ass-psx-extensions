#include "observation_internal.hpp"
#include <algorithm>
namespace coordinator {
psx::scenery::Result ObservationController::check_scenery(const psx::scenery::Query& query,std::uint32_t timeout) noexcept {return impl_->check_coverage(query,timeout);}
psx::scenery::Result ObservationController::Impl::check_coverage(const psx::scenery::Query& query,std::uint32_t timeout) noexcept {
 namespace s=psx::scenery;
 s::Result result;result.query=query;
 std::uint64_t operation{};
 try {
  {
   std::lock_guard lock(mutex);const auto now=qpc_now();const auto psx=traffic.view().selected;
   if(!s::valid(query)){result.reason=s::Reason::invalid;return result;}
   if(WaitForSingleObject(stop,0)==WAIT_OBJECT_0||view.bridge!=State::connected||!view.plugin_available)return result;
   if(!coverage_supported){result.reason=s::Reason::unsupported;return result;}
   if(!psx.pid||psx.pid!=query.psx_pid||psx.creation_filetime!=query.psx_birth){result.reason=s::Reason::identity;return result;}
   if(!(view.plugin.flags&psx::bridge::enabled)||!view.plugin.enable_id||now<view.plugin_received_qpc||now-view.plugin_received_qpc>=qpc_frequency())return result;
   if(coverage_pending||coverage_operation==UINT64_MAX){result.reason=s::Reason::busy;return result;}
   operation=++coverage_operation;coverage_request={view.plugin.connection_id,operation,view.plugin.plugin_load_id,view.plugin.enable_id,now+qpc_frequency()*(std::min)(timeout,s::check_timeout_ms)/1000,query};
   coverage_pending=true;coverage_sent=coverage_ready=false;SetEvent(published.get());
  }
  for(;;){
   {
    std::lock_guard lock(mutex);const auto now=qpc_now();const auto psx=traffic.view().selected;const auto& q=coverage_request;
    if(WaitForSingleObject(stop,0)==WAIT_OBJECT_0||view.bridge!=State::connected||!coverage_supported||!view.plugin_available||q.connection_id!=view.plugin.connection_id||q.plugin_load_id!=view.plugin.plugin_load_id||q.enable_id!=view.plugin.enable_id||psx.pid!=query.psx_pid||psx.creation_filetime!=query.psx_birth){result.reason=s::Reason::disconnected;break;}
    if(now>=q.deadline_qpc){result.reason=s::Reason::timeout;break;}
    if(coverage_ready){result=coverage_reply.result;break;}
   }
   WaitForSingleObject(stop,2);
  }
 }catch(...){result.reason=s::Reason::io;}
 if(operation){std::lock_guard lock(mutex);if(coverage_request.operation_id==operation){coverage_pending=coverage_sent=coverage_ready=false;coverage_request={};coverage_reply={};}}
 return result;
}
}
