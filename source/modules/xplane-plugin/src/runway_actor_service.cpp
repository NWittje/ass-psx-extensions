#include "psx_plugin/runway_actor_service.hpp"
#include <psx/sync/worker_io.hpp>
#include <optional>
#include <algorithm>
#include <psx/non_normal/pipe.hpp>
namespace psx::xp {
bool RunwayActorService::start(const std::wstring& endpoint){stop();endpoint_=endpoint.empty()?nn::plugin_endpoint(nn::current_process()):endpoint;stop_.store(false);overflow_.store(false);ActorCommand c;while(commands_.pop(c)){}Reply r;while(replies_.pop(r)){}try{worker_=std::thread([this]{run();});return true;}catch(...){return false;}}
void RunwayActorService::stop()noexcept{stop_.store(true,std::memory_order_release);if(worker_.joinable())worker_.join();connected_.store(false,std::memory_order_release);}
void RunwayActorService::reply(const nn::Result& r,std::uint64_t connection)noexcept{if(!replies_.push({r,connection}))overflow_.store(true,std::memory_order_release);}
void RunwayActorService::run()noexcept{try{
 const auto server=nn::current_process();const auto frequency=nn::qpc_frequency();
 HANDLE pipe=INVALID_HANDLE_VALUE;
 struct Close {HANDLE h;~Close(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}} close{pipe};
 while(!stop_.load(std::memory_order_acquire)){
  const bool remote=psx::sync::enabled();std::optional<psx::sync::WorkerIo> network;
  if(remote){if(!psx::sync::runtime()->status().connected){Sleep(20);continue;}network.emplace(psx::sync::Family::actor);}
  else {if(pipe==INVALID_HANDLE_VALUE){pipe=nn::create_server(endpoint_);close.h=pipe;if(pipe==INVALID_HANDLE_VALUE){Sleep(25);continue;}}
   if(!nn::accept(pipe,25)){if(GetLastError()!=ERROR_TIMEOUT)break;continue;}}
  nn::ProcessIdentity peer{};if(remote){const auto p=network->status().peer;peer={p.pid,p.process_birth};}
  else if(!nn::authenticate(pipe,true,{},peer)){DisconnectNamedPipe(pipe);continue;}
  const auto connection=connection_.fetch_add(1,std::memory_order_acq_rel)+1;std::uint64_t session{},high_water{},order{};std::uint32_t incoming{},outgoing{};
  auto last=nn::qpc();overflow_.store(false,std::memory_order_release);
  struct Record {nn::Request request{};nn::Result result{};bool result_valid{},terminal{};std::uint64_t order{};};std::array<Record,256> records{};
  const auto send=[&](nn::Result r){if(r.wire_version!=nn::actor_version)return false;r.server=server;r.session=session;nn::Frame f;return outgoing<UINT32_MAX&&nn::encode(r,++outgoing,f)&&(remote?psx::sync::worker_send({f.bytes.data(),f.size}):nn::write_frame(pipe,f,100));};
  const auto receive=[&](nn::Frame& f){if(!remote)return nn::read_frame(pipe,f,0);psx::sync::Frame input;if(!psx::sync::worker_receive(input)){SetLastError(ERROR_NO_DATA);return false;}if(input.size>f.bytes.size()){SetLastError(ERROR_INVALID_DATA);return false;}f.size=input.size;std::copy_n(input.bytes.data(),input.size,f.bytes.data());return true;};
  while(!stop_.load(std::memory_order_acquire)&&!overflow_.load(std::memory_order_acquire)&&(remote?psx::sync::worker_poll(nullptr):psx::sync::local_io_selected()&&nn::connected(pipe))){
   const auto now=nn::qpc();if(now<last||((!remote||session)&&now-last>=static_cast<std::int64_t>(frequency)))break;
   nn::Frame frame;if(receive(frame)){
    nn::Request request;std::uint32_t sequence{};const auto received=nn::qpc();
    if(!nn::decode({frame.bytes.data(),frame.size},request,sequence)||request.wire_version!=nn::actor_version||sequence<=incoming||request.client!=peer||request.target!=server||request.frequency!=frequency||request.issued_qpc>received||(session&&request.session!=session))break;
    incoming=sequence;session=request.session;last=received;connected_.store(true,std::memory_order_release);
    std::size_t found=records.size();for(std::size_t i=0;i<records.size();++i)if(records[i].request.request==request.request){found=i;break;}
    if(found<records.size()){nn::Frame prior,current;if(!nn::encode(records[found].request,1,prior)||!nn::encode(request,1,current)||prior.bytes!=current.bytes)break;if(records[found].result_valid&&!send(records[found].result))break;}
    else {
     nn::Reason reject=nn::Reason::none;
     if(request.request<=high_water)reject=nn::Reason::identity;
     else {high_water=request.request;if(request.deadline_qpc<=received)reject=nn::Reason::expired;}
     if(reject==nn::Reason::none&&request.command!=nn::Command::heartbeat){
      std::size_t slot=records.size();std::uint64_t oldest=UINT64_MAX;
      for(std::size_t i=0;i<records.size();++i){if(!records[i].request.request){slot=i;break;}if(records[i].terminal&&records[i].order<oldest){slot=i;oldest=records[i].order;}}
      if(slot==records.size()||!commands_.push({request,connection}))reject=nn::Reason::busy;
      else {records[slot]={};records[slot].request=request;records[slot].order=++order;}
     }
     if(reject!=nn::Reason::none){nn::Result result;result.wire_version=nn::actor_version;result.request=request.request;result.command=request.command;result.phase=nn::Phase::rejected;result.reason=reject;result.observed_qpc=now;if(!send(result))break;}
    }
   }else if(GetLastError()!=ERROR_NO_DATA)break;
   Reply reply;bool failed=false;for(unsigned i=0;i<32&&replies_.pop(reply);++i){if(reply.connection!=connection)continue;for(auto& record:records)if(record.request.request==reply.result.request){record.result=reply.result;record.result_valid=true;record.terminal=reply.result.phase!=nn::Phase::loading&&reply.result.phase!=nn::Phase::accepted;break;}if(!send(reply.result)){failed=true;break;}}
   if(failed)break;Sleep(2);
  }
  connected_.store(false,std::memory_order_release);if(!remote)DisconnectNamedPipe(pipe);
 }
}catch(...){connected_.store(false,std::memory_order_release);}}
}
