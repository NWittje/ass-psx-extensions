#include <psx/display/publisher.hpp>
#include <psx/display/dpf_protocol.hpp>
#include <algorithm>
#include <cstring>
namespace psx::du::transport {
using psx::norm::Correlation;using psx::norm::Stage;
struct HostPublisher::Impl {
 mutable std::mutex mutex;std::thread supervisor;std::array<std::thread,2> owners;
 std::atomic<bool>quit{};HostOptions options;HostView status;uint64_t control_revision{1};
 std::array<std::unique_ptr<LocalSource>,6> sources;
 struct Pending{Owned data,heartbeat;uint64_t end{},revision{},generation{1},sequence{},local_epoch{};unsigned sent_mask{};Origin origin;Correlation token,heartbeat_token;};std::array<Pending,6> pending;
 std::array<uint8_t,16> session{};std::array<HANDLE,3> wake{};std::array<psx::norm::NativeMonitor,2> monitors;
 std::shared_ptr<Trace> trace_owner;std::atomic<Trace*> trace{};
 std::atomic<psx::norm::DiagnosticSink> event_sink{};std::atomic<void*> event_context{};
 Impl(){options.enabled=false;psx::norm::random_bytes(session);
  for(auto&h:wake){h=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!h){for(auto p:wake)if(p)CloseHandle(p);throw std::runtime_error("DU owner event");}}
  try{supervisor=std::thread(&Impl::run_sources,this);for(unsigned b=0;b<2;++b)owners[b]=std::thread(&Impl::run_binding,this,b);}
  catch(...){request_stop();if(supervisor.joinable())supervisor.join();for(auto&t:owners)if(t.joinable())t.join();for(auto h:wake)CloseHandle(h);throw;}
 }
 ~Impl(){stop();for(auto h:wake)CloseHandle(h);}
 void request_stop()noexcept{quit=true;for(auto h:wake)SetEvent(h);}
 void stop(){request_stop();if(supervisor.joinable())supervisor.join();for(auto&t:owners)if(t.joinable())t.join();}
 void emit(unsigned producer,Stage stage,const Correlation&t,unsigned u,uint64_t end=0,uint64_t value=0,uint32_t flags=0,uint64_t stamp=0)noexcept{if(auto log=trace.load())log->record(producer,stage,t,u,end,value,flags,stamp);}
 void frame(unsigned u,std::span<const uint8_t>b,const LocalView&local){
  const auto began=qpc();auto d=psx::recorder::decode(b);uint64_t generation;
  {std::lock_guard lock(mutex);auto&p=pending[u];if(p.local_epoch!=local.epoch||p.origin.pid!=local.origin.pid||p.origin.created!=local.origin.created||p.origin.identity!=local.origin.identity){++p.generation;p.local_epoch=local.epoch;p.origin=local.origin;p.heartbeat.reset();}generation=p.generation;}
  Correlation token{session,generation,0,d.sequence};emit(u,Stage::source_complete,token,u,d.end,0,0,d.end);emit(u,Stage::assembly,token,u,d.end,b.size(),0,began);
  try{
   auto next=encode({session,local.origin,u,State::frame,generation,1,qpc(),b});
   {std::lock_guard lock(mutex);auto&p=pending[u];if(p.generation!=generation||p.local_epoch!=local.epoch||!options.enabled||!(options.mask&(1u<<u))||quit){++status.replaced;emit(u,Stage::replaced,token,u,d.end);return;}
    token.publication=++p.sequence;auto writable=std::const_pointer_cast<Bytes>(next);std::memcpy(writable->data()+64,&token.publication,sizeof token.publication);
    const unsigned required=options.ethernet.empty()?2:3;if(p.data&&(p.sent_mask&required)!=required){++status.replaced;emit(u,Stage::replaced,p.token,u,p.end);}
    p.data=std::move(next);p.sent_mask=0;p.end=d.end;++p.revision;p.token=token;p.heartbeat.reset();++status.prepared;status.prepare_max_qpc=std::max(status.prepare_max_qpc,qpc()-began);
   }emit(u,Stage::prepared,token,u,d.end,b.size());SetEvent(wake[0]);SetEvent(wake[1]);if(auto log=trace.load())log->digest(u,token,u,b);
  }catch(const std::exception&){std::lock_guard lock(mutex);++status.rejected;}
 }
 void release_sources(){for(auto&s:sources)if(s)s->request_stop();for(auto&s:sources)s.reset();}
 void clear_pending(){std::lock_guard lock(mutex);for(auto&p:pending){p.data.reset();p.heartbeat.reset();++p.generation;p.local_epoch=0;}}
 void run_sources(){
  HostOptions current;uint64_t revision{},inventory_due{};uint32_t selected_pid{};uint64_t selected_creation{};
  std::array<std::vector<Source>,6> inventory;std::array<uint64_t,6> heartbeat{};
  try{while(!quit){HostOptions next;uint64_t version;{std::lock_guard lock(mutex);next=options;version=control_revision;}
   if(version!=revision){if(next.pid!=current.pid||next.created!=current.created||next.enabled!=current.enabled||next==current){release_sources();clear_pending();selected_pid=0;selected_creation=0;inventory_due=0;}current=next;revision=version;}
   if(!current.enabled){release_sources();WaitForSingleObject(wake[2],1000);continue;}
   auto now=qpc();if(current.mask&&now>=inventory_due){for(unsigned u=0;u<6;++u)inventory[u]=discover(u);inventory_due=now+frequency();}
   if(selected_pid&&std::none_of(inventory[0].begin(),inventory[0].end(),[&](auto&s){return s.pid==selected_pid&&s.created==selected_creation;})){release_sources();clear_pending();selected_pid=0;selected_creation=0;}
   if(!selected_pid&&current.mask){auto&choices=inventory[0];auto chosen=choices.end();if(current.pid)chosen=std::find_if(choices.begin(),choices.end(),[&](auto&s){return s.pid==current.pid&&s.created==current.created;});else if(choices.size()==1)chosen=choices.begin();if(chosen!=choices.end()){selected_pid=chosen->pid;selected_creation=chosen->created;}}
   // All disabled leases are signalled before any of them is joined.
   for(unsigned u=0;u<6;++u)if(!(current.mask&(1u<<u))&&sources[u])sources[u]->request_stop();
   for(unsigned u=0;u<6;++u){
    if(!(current.mask&(1u<<u))){if(sources[u]){sources[u].reset();std::lock_guard lock(mutex);pending[u].data.reset();pending[u].heartbeat.reset();++pending[u].generation;pending[u].local_epoch=0;}}
    else if(!sources[u]&&selected_pid){auto&choices=inventory[u];auto chosen=std::find_if(choices.begin(),choices.end(),[&](auto&s){return s.pid==selected_pid&&s.created==selected_creation;});if(chosen!=choices.end())sources[u]=std::make_unique<LocalSource>(u,chosen->pipe,[this,u](auto b,const auto&v){frame(u,b,v);});}
    auto v=sources[u]?sources[u]->view():LocalView{};{std::lock_guard lock(mutex);status.units[u]=v;}
    if(now>=heartbeat[u]){std::lock_guard lock(mutex);auto&p=pending[u];
     // Fresh frames need no heartbeat. Status must not overtake pending frames.
     if(!p.data||now-p.end>frequency())try{State state=!(current.mask&(1u<<u))?State::disabled:v.status.find(L"Source busy")!=std::wstring::npos?State::busy:v.connected?State::waiting:State::disconnected;
      p.heartbeat_token={session,p.generation,++p.sequence,0};p.heartbeat=encode({session,p.origin,u,state,p.generation,p.sequence,now,{}});emit(6,Stage::heartbeat,p.heartbeat_token,u,0,0,uint32_t(state));
     }catch(...){++status.rejected;}
     heartbeat[u]=now+frequency();SetEvent(wake[0]);SetEvent(wake[1]);}
   }WaitForSingleObject(wake[2],50);
  }}catch(const std::exception&e){std::lock_guard lock(mutex);status.error=e.what();}
  release_sources();clear_pending();
 }
 void run_binding(unsigned binding){
  psx::norm::DuEngine engine;struct Context{Impl*self;unsigned binding;}context{this,binding};
  engine.observe([](void*v,const psx::norm::DiagnosticRecord&r)noexcept{auto&c=*static_cast<Context*>(v);auto copy=r;copy.flags=c.binding;if(auto trace=c.self->trace.load())trace->event(7+c.binding,copy);if(auto sink=c.self->event_sink.load())sink(c.self->event_context.load(),copy);},&context,&monitors[binding]);
  bool active{};uint64_t retry{},revision{},sample_due{};HostOptions current;unsigned cursor{};
  std::array<uint64_t,6> sent{},heartbeat_sent{};
  auto mark_active=[&]{std::lock_guard lock(mutex);(binding?status.loopback:status.ethernet)=active;};
  try{while(!quit){HostOptions next;uint64_t version;{std::lock_guard lock(mutex);next=options;version=control_revision;}
   if(version!=revision){if(next.enabled!=current.enabled||(binding==0&&next.ethernet!=current.ethernet)||next==current){engine.stop();active=false;sent={};heartbeat_sent={};retry=0;mark_active();}current=next;revision=version;}
   auto ip=binding==1?"127.0.0.1":current.ethernet;auto now=qpc();
   if(!current.enabled||ip.empty()||(!current.mask&&!active)){WaitForSingleObject(wake[binding],1000);continue;}
   if(!active&&now>=retry)try{std::array<psx::norm::Options,6> opts;for(unsigned u=0;u<6;++u){opts[u].group=group(u);opts[u].port=port(u);opts[u].interface_address=ip;}engine.start(opts,true);active=true;sent={};heartbeat_sent={};mark_active();}
    catch(const std::exception&e){retry=qpc()+frequency();std::lock_guard lock(mutex);status.error=e.what();}
   DWORD wait_ms=1000;
   if(active)try{
    engine.pump(nullptr,nullptr,128);
    for(unsigned offset=0;offset<6;++offset){unsigned u=(cursor+offset)%6;Owned data;uint64_t candidate_revision,end;Correlation token;bool heartbeat_only=false;
     {std::lock_guard lock(mutex);auto&p=pending[u];candidate_revision=p.revision;end=p.end;if(p.data&&candidate_revision!=sent[u]){data=p.data;token=p.token;}else if(p.heartbeat&&p.heartbeat_token.publication!=heartbeat_sent[u]){data=p.heartbeat;token=p.heartbeat_token;heartbeat_only=true;}}
     if(!data)continue;auto before=qpc();bool expired=!heartbeat_only&&before>=end&&before-end>frequency()/30;
     bool delivered=false;if(!expired){engine.pump(nullptr,nullptr,32);before=qpc();expired=!heartbeat_only&&before>=end&&before-end>frequency()/30;
      bool still_current;{std::lock_guard lock(mutex);auto&p=pending[u];still_current=p.generation==token.generation&&(heartbeat_only?p.heartbeat_token.publication==token.publication:p.revision==candidate_revision&&(options.mask&(1u<<u)))&&options.enabled;}
      if(still_current&&!expired)delivered=engine.send(u,data,token);}
     if(delivered||expired){std::lock_guard lock(mutex);auto&p=pending[u];if(heartbeat_only)heartbeat_sent[u]=token.publication;else{sent[u]=candidate_revision;if(p.revision==candidate_revision&&p.generation==token.generation)p.sent_mask|=1u<<binding;if(expired){++status.expired;emit(7+binding,Stage::expired,token,u,end,before-end,binding);}else ++status.published;}}
     else if(!heartbeat_only)wait_ms=std::min<DWORD>(wait_ms,33);
     engine.pump(nullptr,nullptr,32);
    }cursor=(cursor+1)%6;
    if(qpc()>=sample_due){BindingView snapshot;snapshot.stats=engine.stats();for(unsigned u=0;u<6;++u){snapshot.rate[u]=engine.tx_rate(u);snapshot.grtt[u]=engine.grtt(u);emit(7+binding,Stage::rate,{},u,0,uint64_t(snapshot.rate[u]),binding);emit(7+binding,Stage::grtt,{},u,0,uint64_t(snapshot.grtt[u]*1e9),binding);}sample_due=qpc()+frequency();
     std::lock_guard lock(mutex);status.bindings[binding]=snapshot;status.owned_peak=std::max<uint64_t>(status.owned_peak,status.bindings[0].stats.owned+status.bindings[1].stats.owned);}
   }catch(const std::exception&e){engine.stop();active=false;retry=qpc()+frequency();mark_active();std::lock_guard lock(mutex);status.error=e.what();}
   HANDLE handles[2]{wake[binding],active?engine.event():nullptr};WaitForMultipleObjects(active?2:1,handles,FALSE,wait_ms);
  }}catch(const std::exception&e){std::lock_guard lock(mutex);status.error=e.what();}
  engine.stop();active=false;mark_active();
 }
};
HostPublisher::HostPublisher():impl_(std::make_unique<Impl>()){}
HostPublisher::~HostPublisher(){stop();}
void HostPublisher::configure(HostOptions options){{std::lock_guard lock(impl_->mutex);if(options==impl_->options)return;impl_->options=std::move(options);++impl_->control_revision;}for(auto h:impl_->wake)SetEvent(h);}
void HostPublisher::reconnect(){{std::lock_guard lock(impl_->mutex);++impl_->control_revision;}for(auto h:impl_->wake)SetEvent(h);}
void HostPublisher::observe(std::shared_ptr<Trace>trace){std::lock_guard lock(impl_->mutex);if(impl_->options.enabled||impl_->trace_owner)throw std::runtime_error("Attach DU trace once before enabling");impl_->trace_owner=std::move(trace);impl_->trace.store(impl_->trace_owner.get());}
void HostPublisher::events(psx::norm::DiagnosticSink sink,void*context){std::lock_guard lock(impl_->mutex);if(impl_->options.enabled)throw std::runtime_error("Attach DU events before enabling");impl_->event_context=context;impl_->event_sink=sink;}
void HostPublisher::request_stop()noexcept{if(impl_)impl_->request_stop();}
void HostPublisher::stop(){if(impl_)impl_->stop();}
HostView HostPublisher::view()const{std::lock_guard lock(impl_->mutex);auto result=impl_->status;result.payload_bytes=owned_bytes();result.payload_peak=owned_peak();for(unsigned b=0;b<2;++b)result.bindings[b].native=impl_->monitors[b].snapshot();if(auto trace=impl_->trace.load()){result.diagnostic_lost=trace->lost();result.diagnostics_failed=trace->failed();}return result;}
}
