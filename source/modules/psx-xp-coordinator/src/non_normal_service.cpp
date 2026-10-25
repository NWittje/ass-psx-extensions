#include "coordinator/non_normal.hpp"
#include <psx/sync/runtime.hpp>
#include "coordinator/win_handle.hpp"
#include <psx/non_normal/client.hpp>
#include <atomic>
#include <array>
#include <mutex>
#include <thread>
#include <algorithm>

namespace coordinator {
namespace n=non_normal_wire;
namespace v2=psx::environment::presentation;
struct NonNormalService::Impl {
 enum class Kind {ios_open,ios_request,ios_lost,plugin_open,sent,uncertain,result,plugin_lost,selection,psx,overflow};
 struct Event {Kind kind{};std::uint64_t epoch{},session{};n::ProcessIdentity peer{};n::Request request{};n::Result result{};};
 struct Command {std::uint64_t epoch{};n::Request request{};};
 struct Response {std::uint64_t epoch{};n::Result result{};};
 struct VisibilityResponse {std::uint64_t epoch{};v2::VisibilityResult result{};};
 std::array<VisibilityIpcEvent,64> visibility_events{};std::size_t visibility_head{},visibility_size{};
 std::array<VisibilityResponse,64> visibility_responses{};std::size_t visibility_response_head{},visibility_response_size{};
 WinHandle visibility_ready{make_stop_event()};std::uint64_t visibility_abort{};
 mutable std::mutex mutex;
 std::array<Event,256> events{};std::size_t event_head{},event_size{};
 std::array<Command,64> commands{};std::size_t command_head{},command_size{};
 std::array<Response,64> responses{};std::size_t response_head{},response_size{};
 n::ProcessIdentity desired{},requested{},psx{};std::uint64_t binding_version{},ios_epoch{},plugin_epoch{},ios_abort{},ios_connection_counter{};
 n::Request retirement{};NonNormalView view{};NonNormalModel model;
 std::wstring fixture_ios,fixture_plugin;
 WinHandle stop{make_stop_event()},wake{make_stop_event()},server_ready{make_stop_event()};
 std::thread acquisition,ios,plugin;
 std::atomic<bool> acquisition_done{true},ios_done{true},plugin_done{true},plugin_stop{false};
 std::atomic<DWORD> server_error{};
 bool stopping()const{return WaitForSingleObject(stop.get(),0)==WAIT_OBJECT_0;}
 void push_locked(const Event& event){
  if(event_size==events.size()){
   event_head=event_size=0;commands={};command_head=command_size=0;++binding_version;++ios_abort;retirement={};Event failure;failure.kind=Kind::overflow;events[event_size++]=failure;
  }else{events[(event_head+event_size)%events.size()]=event;++event_size;}
  SetEvent(wake.get());
 }
 void push(const Event& event){std::lock_guard<std::mutex> lock(mutex);push_locked(event);}
 bool pop(Event& event){std::lock_guard<std::mutex> lock(mutex);if(!event_size){ResetEvent(wake.get());return false;}event=events[event_head];event_head=(event_head+1)%events.size();--event_size;return true;}
 void kick(const n::Request& remove){std::lock_guard<std::mutex> lock(mutex);retirement=remove;++binding_version;command_head=command_size=0;}
 void abort_ios(){std::lock_guard<std::mutex> lock(mutex);++ios_abort;response_head=response_size=0;}
 bool dispatch(std::uint64_t epoch,const n::Request& request){
  std::lock_guard<std::mutex> lock(mutex);if(command_size==commands.size())return false;commands[(command_head+command_size)%commands.size()]={epoch,request};++command_size;return true;
 }
 bool respond(std::uint64_t epoch,const n::Result& result){
  std::lock_guard<std::mutex> lock(mutex);if(response_size==responses.size())return false;responses[(response_head+response_size)%responses.size()]={epoch,result};++response_size;return true;
 }
 void push_visibility(VisibilityIpcEvent event){
  std::lock_guard<std::mutex> lock(mutex);
  if(visibility_size==visibility_events.size()){++visibility_abort;visibility_head=visibility_size=0;event.kind=VisibilityIpcEvent::Kind::lost;}
  visibility_events[(visibility_head+visibility_size)%visibility_events.size()]=event;++visibility_size;SetEvent(visibility_ready.get());
 }
 void acquire(){
  model.configure(n::current_process(),n::qpc_frequency());
  while(!stopping()){
   Event event;for(unsigned budget=0;budget<128&&pop(event);++budget){
    const auto now=n::qpc();bool good=true;
    switch(event.kind){
     case Kind::ios_open:model.ios_open(event.epoch,event.peer,now);break;
     case Kind::ios_request:good=model.request(event.epoch,event.request,now);break;
     case Kind::ios_lost:model.ios_close(event.epoch,now);break;
     case Kind::selection:{model.selected(event.peer,now);n::Request remove;model.take_retirement(remove);std::lock_guard<std::mutex> lock(mutex);desired=event.peer;retirement=remove;++binding_version;command_head=command_size=0;break;}
     case Kind::psx:model.associate_psx(event.peer);break;
     case Kind::plugin_open:model.plugin_open(event.epoch,event.peer,event.session,now);break;
     case Kind::sent:good=model.sent(event.epoch,event.request,now);break;
     case Kind::uncertain:good=model.uncertain_write(event.epoch,event.request);break;
     case Kind::result:good=model.result(event.epoch,event.result,now);break;
     case Kind::plugin_lost:model.plugin_close(event.epoch,now);break;
     case Kind::overflow:{const auto state=model.view();model.ios_close(state.ios_epoch,now);model.plugin_close(state.plugin_epoch,now);break;}
    }
    if(!good){const auto state=model.view();model.ios_close(state.ios_epoch,now);abort_ios();}
   }
   model.tick(n::qpc());n::Request remove;
   if(model.take_retirement(remove))kick(remove);
   std::uint64_t epoch{};n::Request request;
   for(unsigned budget=0;budget<64&&model.next_command(epoch,request);++budget)if(!dispatch(epoch,request)){model.plugin_close(epoch,n::qpc());kick({});break;}
   n::Result result;for(unsigned budget=0;budget<64&&model.next_result(epoch,result);++budget)if(!respond(epoch,result)){model.ios_close(epoch,n::qpc());abort_ios();break;}
   {std::lock_guard<std::mutex> lock(mutex);view=model.view();}
   WaitForSingleObject(wake.get(),10);
  }
  const auto state=model.view();model.ios_close(state.ios_epoch,n::qpc());n::Request remove;if(model.take_retirement(remove))kick(remove);
  {std::lock_guard<std::mutex> lock(mutex);view=model.view();}plugin_stop=true;acquisition_done=true;
 }
 void ios_worker(){
  std::wstring endpoint;{std::lock_guard<std::mutex> lock(mutex);endpoint=fixture_ios;}
  if(endpoint.empty())endpoint=n::coordinator_endpoint();
  enum class Role {unknown,actor,visibility};
  struct Record {bool used{},ready{};v2::VisibilityRequest request{};v2::VisibilityResult result{};};
  struct Peer {WinHandle pipe;bool connected{};Role role{Role::unknown};std::uint64_t epoch{},session{},last{},abort{},request_floor{};n::ProcessIdentity identity{};std::uint32_t incoming{},outgoing{};std::array<Record,64> records{};std::size_t cursor{};};
  std::array<Peer,2> peers;
  peers[0].pipe.reset(n::create_server(endpoint,2,true));
  if(peers[0].pipe.valid())peers[1].pipe.reset(n::create_server(endpoint,2,false));
  server_error=peers[0].pipe.valid()&&peers[1].pipe.valid()?ERROR_SUCCESS:GetLastError();SetEvent(server_ready.get());
  if(server_error!=ERROR_SUCCESS){ios_done=true;return;}
  auto& counter=ios_connection_counter;
  const auto close=[&](Peer& peer){
   if(peer.role==Role::actor){Event lost;lost.kind=Kind::ios_lost;lost.epoch=peer.epoch;push(lost);}
   else if(peer.role==Role::visibility)push_visibility({VisibilityIpcEvent::Kind::lost,peer.epoch,peer.session,{}});
   DisconnectNamedPipe(peer.pipe.get());peer.connected=false;peer.role=Role::unknown;peer.session=0;peer.records={};
  };
  const auto send_visibility=[&](Peer& peer,const v2::VisibilityResult& result){
   psx::environment::Frame wide;n::Frame frame;if(peer.outgoing==UINT32_MAX||!v2::encode(result,++peer.outgoing,wide)||wide.size>frame.bytes.size())return false;
   frame.size=wide.size;std::copy(wide.bytes.begin(),wide.bytes.begin()+wide.size,frame.bytes.begin());return n::write_frame(peer.pipe.get(),frame,100);
  };
  const auto unavailable=[&](const v2::VisibilityRequest& request,v2::VisibilityReason reason){v2::VisibilityResult result;const auto self=n::current_process();result.server={self.pid,self.creation_filetime};result.session=request.session;result.request=request.request;result.target=request.target;result.phase=reason==v2::VisibilityReason::disconnected?v2::VisibilityPhase::unavailable:v2::VisibilityPhase::rejected;result.reason=reason;result.observed_qpc=n::qpc();return result;};
  while(!stopping()){
   for(auto& peer:peers){
    if(!peer.connected){
     if(!n::accept(peer.pipe.get(),1)){if(GetLastError()!=ERROR_TIMEOUT){server_error=GetLastError();SetEvent(stop.get());}continue;}
     if(!n::authenticate(peer.pipe.get(),true,{},peer.identity)||counter==UINT64_MAX){DisconnectNamedPipe(peer.pipe.get());continue;}
     peer.connected=true;peer.epoch=++counter;peer.role=Role::unknown;peer.last=GetTickCount64();peer.session=peer.request_floor=0;peer.incoming=peer.outgoing=0;peer.records={};peer.cursor=0;
    }
    bool healthy=true;
    {std::lock_guard<std::mutex> lock(mutex);if((peer.role==Role::actor&&peer.abort!=ios_abort)||(peer.role==Role::visibility&&peer.abort!=visibility_abort))healthy=false;}
    n::Frame frame;
    for(unsigned budget=0;healthy&&budget<16;++budget){
     if(!n::read_frame(peer.pipe.get(),frame,0)){if(GetLastError()!=ERROR_NO_DATA)healthy=false;break;}
     const bool extended=frame.size>=4&&frame.bytes[0]=='P'&&frame.bytes[1]=='X'&&frame.bytes[2]=='W'&&(frame.bytes[3]=='2'||frame.bytes[3]=='3'||frame.bytes[3]=='4');
     const auto wanted=extended?Role::visibility:Role::actor;
     if(peer.role==Role::unknown){
      bool occupied=false;for(const auto& other:peers)if(&other!=&peer&&other.connected&&other.role==wanted)occupied=true;
      if(occupied){healthy=false;break;}peer.role=wanted;
      if(wanted==Role::actor){std::lock_guard<std::mutex> lock(mutex);ios_epoch=peer.epoch;peer.abort=ios_abort;response_head=response_size=0;}
      else{std::lock_guard<std::mutex> lock(mutex);peer.abort=visibility_abort;visibility_response_head=visibility_response_size=0;}
     }
     if(peer.role!=wanted){healthy=false;break;}
     if(extended){
      v2::Header header;v2::VisibilityRequest request;
      if(!v2::read_header(frame.bytes.data(),16,header)||header.kind!=v2::Kind::visibility_request||header.sequence<=peer.incoming||!v2::decode({frame.bytes.data()+16,frame.size-16},request)||request.client!=v2::ProcessIdentity{peer.identity.pid,peer.identity.creation_filetime}||request.server!=v2::ProcessIdentity{n::current_process().pid,n::current_process().creation_filetime}||(peer.session&&request.session!=peer.session)||request.qpc_frequency!=n::qpc_frequency()||request.issued_qpc>n::qpc()){healthy=false;break;}
      peer.incoming=header.sequence;peer.last=GetTickCount64();
      if(!peer.session){peer.session=request.session;push_visibility({VisibilityIpcEvent::Kind::open,peer.epoch,peer.session,{}});}
      Record* previous=nullptr;for(auto& record:peer.records)if(record.used&&record.request.request==request.request){previous=&record;break;}
      if(previous){if(previous->request!=request){healthy=send_visibility(peer,unavailable(request,v2::VisibilityReason::identity));}else if(previous->ready)healthy=send_visibility(peer,previous->result);continue;}
      if(request.request<=peer.request_floor){healthy=send_visibility(peer,unavailable(request,v2::VisibilityReason::identity));continue;}peer.request_floor=request.request;
      Record* record=nullptr;for(std::size_t index=0;index<peer.records.size();++index){auto& candidate=peer.records[(peer.cursor+index)%peer.records.size()];if(!candidate.used||candidate.ready){record=&candidate;peer.cursor=(peer.cursor+index+1)%peer.records.size();break;}}
      if(!record){healthy=send_visibility(peer,unavailable(request,v2::VisibilityReason::busy));continue;}*record={};record->used=true;record->request=request;
      n::ProcessIdentity main;{std::lock_guard<std::mutex> lock(mutex);main=psx;}
      if(!main.pid||request.deadline_qpc<=n::qpc()){
       record->ready=true;record->result=unavailable(request,!main.pid?v2::VisibilityReason::disconnected:v2::VisibilityReason::expired);healthy=send_visibility(peer,record->result);
      }else push_visibility({VisibilityIpcEvent::Kind::request,peer.epoch,peer.session,request});
     }else{
      n::Request request;std::uint32_t sequence{};
      if(!n::decode({frame.bytes.data(),frame.size},request,sequence)||request.wire_version!=n::actor_version||sequence<=peer.incoming||request.client!=peer.identity||request.target!=n::current_process()||(peer.session&&request.session!=peer.session)){healthy=false;break;}
      if(!peer.session){Event opened;opened.kind=Kind::ios_open;opened.epoch=peer.epoch;opened.peer=peer.identity;push(opened);}
      peer.session=request.session;peer.incoming=sequence;peer.last=GetTickCount64();Event event;event.kind=Kind::ios_request;event.epoch=peer.epoch;event.request=request;push(event);
     }
    }
    for(unsigned budget=0;healthy&&budget<16;++budget){
     if(peer.role==Role::actor){
      Response response;bool available=false;{std::lock_guard<std::mutex> lock(mutex);if(response_size){response=responses[response_head];response_head=(response_head+1)%responses.size();--response_size;available=true;}}
      if(!available)break;if(response.epoch!=peer.epoch)continue;
      if(peer.outgoing==UINT32_MAX||!n::encode(response.result,++peer.outgoing,frame)||!n::write_frame(peer.pipe.get(),frame,100))healthy=false;
     }else if(peer.role==Role::visibility){
      VisibilityResponse response;bool available=false;{std::lock_guard<std::mutex> lock(mutex);if(visibility_response_size){response=visibility_responses[visibility_response_head];visibility_response_head=(visibility_response_head+1)%visibility_responses.size();--visibility_response_size;available=true;}}
      if(!available)break;if(response.epoch!=peer.epoch||response.result.session!=peer.session)continue;
      for(auto& record:peer.records)if(record.used&&!record.ready&&record.request.request==response.result.request){record.ready=true;record.result=response.result;healthy=send_visibility(peer,record.result);break;}
     }else break;
    }
    if(!healthy||!n::connected(peer.pipe.get())||(peer.role!=Role::visibility&&GetTickCount64()-peer.last>=1000))close(peer);
   }
   WaitForSingleObject(stop.get(),2);
  }
  for(auto& peer:peers)if(peer.connected)close(peer);ios_done=true;
 }
 void remove_before_close(n::Client& client,const n::Request& remove){
  if(client.is_connected()&&remove.command==n::Command::remove_actor&&remove.request&&remove.target==client.server()&&remove.session==client.session()&&n::qpc()<remove.deadline_qpc)client.send(remove,100);
  client.disconnect();
 }
 void plugin_worker_network(){
  psx::sync::Port port(psx::sync::Family::actor);std::uint64_t counter{},epoch{},binding{},session{};std::uint32_t sequence{};
  n::ProcessIdentity expected{};
  const auto retire_actor=[&]{n::Request remove;{std::lock_guard<std::mutex> lock(mutex);remove=retirement;retirement={};}
   if(epoch&&!port.changed()&&remove.command==n::Command::remove_actor&&remove.request&&remove.target==expected&&remove.session==session&&n::qpc()<remove.deadline_qpc){n::Frame f;if(sequence<UINT32_MAX&&n::encode(remove,++sequence,f))port.send({f.bytes.data(),f.size});}
  };
  const auto close=[&]{if(epoch){Event e;e.kind=Kind::plugin_lost;e.epoch=epoch;push(e);port.detach();}epoch=0;};
  while(!plugin_stop){
   n::ProcessIdentity wanted;std::uint64_t version{};
   {std::lock_guard<std::mutex> lock(mutex);wanted=desired;version=binding_version;}
   if(version!=binding){retire_actor();close();binding=version;expected=wanted;}
   const auto state=port.status();
   if(!expected.pid||!state.connected||state.peer.pid!=expected.pid||state.peer.process_birth!=expected.creation_filetime){close();Sleep(20);continue;}
   if(epoch&&port.changed())close();
   if(!epoch){if(counter==UINT64_MAX)break;if(port.changed())port.attach();if(!port.connected()){Sleep(2);continue;}epoch=++counter;session=n::nonce();sequence=0;
    {std::lock_guard<std::mutex> lock(mutex);plugin_epoch=epoch;}
    Event e;e.kind=Kind::plugin_open;e.epoch=epoch;e.peer=expected;e.session=session;push(e);
   }
   Command command;bool available=false;
   {std::lock_guard<std::mutex> lock(mutex);if(binding==binding_version&&requested==desired&&command_size){command=commands[command_head];command_head=(command_head+1)%commands.size();--command_size;available=true;}}
   if(available&&command.epoch==epoch){n::Frame f;Event e;e.epoch=epoch;e.request=command.request;
    if(command.request.deadline_qpc<=n::qpc()){close();}
    else {e.kind=sequence<UINT32_MAX&&n::encode(command.request,++sequence,f)&&port.send({f.bytes.data(),f.size})?Kind::sent:Kind::uncertain;push(e);}
   }
   psx::sync::Frame frame;for(unsigned budget=0;budget<16&&port.receive(frame);++budget){n::Result result;std::uint32_t wire{};if(!n::decode({frame.bytes.data(),frame.size},result,wire)||result.server!=expected||result.session!=session){close();break;}Event e;e.kind=Kind::result;e.epoch=epoch;e.result=result;push(e);}
   Sleep(2);
  }
  retire_actor();close();plugin_done=true;
 }
 void plugin_worker(){
  if(psx::sync::enabled()){plugin_worker_network();return;}
  n::Client client;std::uint64_t counter{},epoch{},binding{};n::ProcessIdentity expected{};
  while(!plugin_stop){
   n::ProcessIdentity wanted;std::uint64_t version{};n::Request remove;std::wstring fixture;
   {std::lock_guard<std::mutex> lock(mutex);wanted=desired;version=binding_version;fixture=fixture_plugin;if(version!=binding){remove=retirement;retirement={};}}
   if(version!=binding){
    remove_before_close(client,remove);if(epoch){Event lost;lost.kind=Kind::plugin_lost;lost.epoch=epoch;push(lost);}epoch=0;binding=version;expected=wanted;
   }
   if(!expected.pid){Sleep(10);continue;}
   if(!client.is_connected()){
    const auto endpoint=fixture.empty()?n::plugin_endpoint(expected):fixture;
    if(!client.connect(endpoint,expected)){Sleep(20);continue;}
    if(counter==UINT64_MAX)break;epoch=++counter;
    {std::lock_guard<std::mutex> lock(mutex);plugin_epoch=epoch;}
    Event opened;opened.kind=Kind::plugin_open;opened.epoch=epoch;opened.peer=client.server();opened.session=client.session();push(opened);
   }
   Command command;bool available=false;
   {std::lock_guard<std::mutex> lock(mutex);if(binding==binding_version&&requested==desired&&command_size){command=commands[command_head];command_head=(command_head+1)%commands.size();--command_size;available=true;}}
   if(available&&command.epoch==epoch){
    if(command.request.deadline_qpc<=n::qpc()){client.disconnect();}
    else if(!client.send(command.request,100)){Event failure;failure.kind=Kind::uncertain;failure.epoch=epoch;failure.request=command.request;push(failure);}
    else{Event sent;sent.kind=Kind::sent;sent.epoch=epoch;sent.request=command.request;push(sent);}
   }
   n::Result result;
   for(unsigned budget=0;budget<16&&client.receive(result,0);++budget){Event event;event.kind=Kind::result;event.epoch=epoch;event.result=result;push(event);}
   if(!client.is_connected()){Event lost;lost.kind=Kind::plugin_lost;lost.epoch=epoch;push(lost);epoch=0;Sleep(20);}
   else Sleep(2);
  }
  n::Request remove;{std::lock_guard<std::mutex> lock(mutex);remove=retirement;retirement={};}remove_before_close(client,remove);plugin_done=true;
 }
};
NonNormalService::NonNormalService():impl_(std::make_unique<Impl>()){}
NonNormalService::~NonNormalService(){request_stop();join();}
bool NonNormalService::start(){
 auto& p=*impl_;if(!done()||p.acquisition.joinable()||p.ios.joinable()||p.plugin.joinable())return false;
 ResetEvent(p.stop.get());ResetEvent(p.server_ready.get());p.plugin_stop=false;p.server_error=ERROR_IO_PENDING;
 try{
  p.acquisition_done=false;p.acquisition=std::thread([&p]{try{p.acquire();}catch(...){SetEvent(p.stop.get());p.plugin_stop=true;p.acquisition_done=true;}});
  p.plugin_done=false;p.plugin=std::thread([&p]{try{p.plugin_worker();}catch(...){SetEvent(p.stop.get());p.plugin_done=true;}});
  p.ios_done=false;p.ios=std::thread([&p]{try{p.ios_worker();}catch(...){p.server_error=ERROR_UNHANDLED_EXCEPTION;SetEvent(p.server_ready.get());SetEvent(p.stop.get());p.ios_done=true;}});
 }catch(...){if(!p.acquisition.joinable())p.acquisition_done=true;if(!p.ios.joinable())p.ios_done=true;if(!p.plugin.joinable())p.plugin_done=true;request_stop();join();return false;}
 if(WaitForSingleObject(p.server_ready.get(),1000)!=WAIT_OBJECT_0||p.server_error!=ERROR_SUCCESS){request_stop();join();return false;}return true;
}
void NonNormalService::select(n::ProcessIdentity selected){auto& p=*impl_;std::lock_guard<std::mutex> lock(p.mutex);if(selected==p.requested)return;Impl::Event event;event.kind=Impl::Kind::selection;event.peer=selected;p.push_locked(event);p.requested=selected;}
void NonNormalService::associate_psx(n::ProcessIdentity psx){auto& p=*impl_;std::lock_guard<std::mutex> lock(p.mutex);if(psx==p.psx)return;Impl::Event event;event.kind=Impl::Kind::psx;event.peer=psx;p.push_locked(event);p.psx=psx;}
void NonNormalService::request_stop()noexcept{SetEvent(impl_->stop.get());SetEvent(impl_->wake.get());}
void NonNormalService::join(){if(impl_->acquisition.joinable())impl_->acquisition.join();impl_->plugin_stop=true;if(impl_->ios.joinable())impl_->ios.join();if(impl_->plugin.joinable())impl_->plugin.join();}
bool NonNormalService::done()const noexcept{return impl_->acquisition_done&&impl_->ios_done&&impl_->plugin_done;}
NonNormalView NonNormalService::snapshot()const{std::lock_guard<std::mutex> lock(impl_->mutex);auto view=impl_->view;if(impl_->requested!=impl_->desired||impl_->stopping()){view.plugin_connected=view.report_current=false;view.report={};}return view;}
bool NonNormalService::pop_visibility(VisibilityIpcEvent& event){auto& p=*impl_;std::lock_guard<std::mutex> lock(p.mutex);if(!p.visibility_size){ResetEvent(p.visibility_ready.get());return false;}event=p.visibility_events[p.visibility_head];p.visibility_head=(p.visibility_head+1)%p.visibility_events.size();--p.visibility_size;if(!p.visibility_size)ResetEvent(p.visibility_ready.get());return true;}
bool NonNormalService::visibility_result(std::uint64_t epoch,const v2::VisibilityResult& result){auto& p=*impl_;std::lock_guard<std::mutex> lock(p.mutex);if(!v2::valid(result))return false;if(p.visibility_response_size==p.visibility_responses.size()){++p.visibility_abort;p.visibility_response_head=p.visibility_response_size=0;return false;}p.visibility_responses[(p.visibility_response_head+p.visibility_response_size)%p.visibility_responses.size()]={epoch,result};++p.visibility_response_size;return true;}
std::uintptr_t NonNormalService::visibility_ready_event()const noexcept{return reinterpret_cast<std::uintptr_t>(impl_->visibility_ready.get());}
void NonNormalService::fixture_endpoints(std::wstring ios,std::wstring plugin){if(!done())return;std::lock_guard<std::mutex> lock(impl_->mutex);impl_->fixture_ios=std::move(ios);impl_->fixture_plugin=std::move(plugin);}
}
