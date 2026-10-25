#include <psx/display/tcp.hpp>
#include <psx/display/dpf_protocol.hpp>
#include <psx/display/timing.hpp>
#include <algorithm>
namespace psx::du::transport {
using psx::norm::Stage;using psx::norm::Correlation;
struct TcpHostPublisher::Impl {
 mutable std::mutex mutex;std::thread supervisor;std::array<std::thread,2>owners;std::array<std::unique_ptr<LocalSource>,6>sources;std::unique_ptr<LocalSource>instrument_source;
 std::atomic<bool>quit{};std::atomic<unsigned>subscribers{},instrument_subscribers{};TcpHostOptions options;TcpHostView status;uint64_t revision{1};
 struct Pending{tcp::Frame frame,status;Origin origin;uint64_t generation{1},sequence{},local_epoch{},revision{};};std::array<Pending,6>pending;Pending instrument_pending;
 tcp::Identity session{tcp::identity()};std::array<std::unique_ptr<tcp::Server>,2>servers;HANDLE changed{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
 std::shared_ptr<Trace>trace_owner;std::atomic<Trace*>trace{};
 std::filesystem::path source_output;std::unique_ptr<TimingCapture> source_timing;
 psx::norm::DiagnosticSink event_sink{};void*event_context{};
 Impl(){options.enabled=false;if(!changed)throw std::runtime_error("TCP source control event");try{for(unsigned b=0;b<2;++b)servers[b]=std::make_unique<tcp::Server>(b,subscribers);supervisor=std::thread(&Impl::source_loop,this);for(unsigned b=0;b<2;++b)owners[b]=std::thread(&Impl::binding_loop,this,b);}catch(...){stop();CloseHandle(changed);throw;}}
 ~Impl(){stop();CloseHandle(changed);}
 void signal()noexcept{SetEvent(changed);for(auto&s:servers)if(s)s->notify();}
 void request_stop()noexcept{quit=true;signal();}
 void stop(){request_stop();if(supervisor.joinable())supervisor.join();for(auto&t:owners)if(t.joinable())t.join();}
 void event(unsigned p,Stage stage,const Correlation&c,unsigned u,uint64_t end=0,uint64_t value=0,uint64_t stamp=0){if(auto t=trace.load())t->record(p,stage,c,u,end,value,0,stamp);}
 void frame(unsigned u,Owned body,const LocalView&v){auto b=std::span<const uint8_t>(*body);auto begin=qpc();auto d=psx::recorder::decode(b);uint64_t gen,seq,rev;
  {std::lock_guard l(mutex);auto&p=pending[u];if(quit||!options.enabled||!(options.mask&(1u<<u)))return;if(p.local_epoch!=v.epoch||p.origin.pid!=v.origin.pid||p.origin.created!=v.origin.created||p.origin.identity!=v.origin.identity){++p.generation;p.local_epoch=v.epoch;p.origin=v.origin;p.status.reset();}gen=p.generation;seq=++p.sequence;rev=++p.revision;}
  Correlation token{session,gen,seq,d.sequence};event(u,Stage::validation_enter,token,u,d.end,b.size(),begin);event(u,Stage::validated,token,u,d.end,b.size());event(u,Stage::source_complete,token,u,d.end,0,d.end);event(u,Stage::assembly,token,u,d.end,b.size(),begin);event(u,Stage::source_generation,token,u,d.end,d.generation);event(u,Stage::source_paint,token,u,d.end,d.paint);
  if(auto t=trace.load()){t->record(u,Stage::source_identity,token,u,d.end,v.origin.pid,0);t->record(u,Stage::source_identity,token,u,d.end,v.origin.created,1);}
  try{auto packet=tcp::packet({session,v.origin,u,State::frame,gen,seq,qpc(),b},std::move(body),rev);
   {std::lock_guard l(mutex);auto&p=pending[u];if(quit||!options.enabled||!(options.mask&(1u<<u))||p.generation!=gen||p.revision!=rev){++status.replaced;return;}if(p.frame){++status.replaced;event(u,Stage::replaced,p.frame->token,u,p.frame->end);}p.frame=std::move(packet);++status.prepared;status.prepare_max_qpc=std::max(status.prepare_max_qpc,qpc()-begin);}
   event(u,Stage::prepared,token,u,d.end,b.size());for(auto&s:servers)s->notify();if(auto t=trace.load())t->digest(u,token,u,b);
  }catch(...){std::lock_guard l(mutex);++status.rejected;}
 }
 void instrument_frame(Owned body,const LocalView&v){auto b=std::span<const uint8_t>(*body);auto begin=qpc();auto d=psx::instrument::decode(b);uint64_t gen,seq,rev;
  {std::lock_guard l(mutex);auto&p=instrument_pending;if(quit||!options.enabled||!instrument_subscribers)return;if(p.local_epoch!=v.epoch||p.origin.pid!=v.origin.pid||p.origin.created!=v.origin.created||p.origin.identity!=v.origin.identity){++p.generation;p.local_epoch=v.epoch;p.origin=v.origin;p.status.reset();}gen=p.generation;seq=++p.sequence;rev=++p.revision;}
  Correlation token{session,gen,seq,d.sequence};event(6,Stage::validation_enter,token,0x8001,d.end,b.size(),begin);event(6,Stage::validated,token,0x8001,d.end,b.size());event(6,Stage::source_complete,token,0x8001,d.end,0,d.end);event(6,Stage::assembly,token,0x8001,d.end,b.size(),begin);event(6,Stage::source_generation,token,0x8001,d.end,d.generation);event(6,Stage::source_paint,token,0x8001,d.end,d.paint);event(6,Stage::source_allocated,token,0x8001,d.end,d.allocated);event(6,Stage::invocation_enter,token,0x8001,d.end,0,d.start);event(6,Stage::invocation_return,token,0x8001,d.end,0,d.end);
  try{auto packet=tcp::instrument_packet({session,v.origin,psx::instrument::captain_stab_trim,State::frame,gen,seq,qpc(),b},std::move(body),rev);
   {std::lock_guard l(mutex);auto&p=instrument_pending;if(quit||!options.enabled||!instrument_subscribers||p.generation!=gen||p.revision!=rev)return;if(p.frame)++status.instrument_replaced;p.frame=std::move(packet);++status.instrument_received;}
   event(6,Stage::prepared,token,0x8001,d.end,b.size());if(auto t=trace.load())t->digest(6,token,0x8001,b);
   for(auto&server:servers)server->notify();
  }catch(...){std::lock_guard l(mutex);++status.instrument_invalid;}
 }
 void release(){for(auto&s:sources)if(s)s->request_stop();if(instrument_source)instrument_source->request_stop();if(source_timing)source_timing->request_stop();for(auto&s:sources)s.reset();instrument_source.reset();source_timing.reset();}
 void clear(){std::lock_guard l(mutex);for(auto&p:pending){p.frame.reset();p.status.reset();p.origin={};p.local_epoch=0;++p.generation;}instrument_pending.frame.reset();instrument_pending.status.reset();instrument_pending.origin={};instrument_pending.local_epoch=0;++instrument_pending.generation;signal();}
 void source_loop(){TcpHostOptions current;uint64_t seen{},due{};uint32_t pid{};uint64_t born{};std::array<std::vector<Source>,6>inventory;std::array<uint64_t,6>heartbeat{};uint64_t instrument_heartbeat{},instrument_discovery_due{};
  try{while(!quit){TcpHostOptions next;uint64_t rev;{std::lock_guard l(mutex);next=options;rev=revision;}
   if(rev!=seen){if(next.pid!=current.pid||next.created!=current.created||next.enabled!=current.enabled||next==current){release();clear();pid=0;born=0;due=0;}current=next;seen=rev;}
   if(!current.enabled){release();WaitForSingleObject(changed,1000);continue;}
   auto now=qpc();if(current.mask&&now>=due){for(unsigned u=0;u<6;++u)inventory[u]=discover(u);due=now+frequency();}
   if(pid&&std::none_of(inventory[0].begin(),inventory[0].end(),[&](auto&s){return s.pid==pid&&s.created==born;})){release();clear();pid=0;born=0;}
   if(!pid&&current.mask){auto&choices=inventory[0];auto choice=choices.end();if(current.pid)choice=std::find_if(choices.begin(),choices.end(),[&](auto&s){return s.pid==current.pid&&s.created==current.created;});else if(choices.size()==1)choice=choices.begin();if(choice!=choices.end()){pid=choice->pid;born=choice->created;if(!source_output.empty())try{source_timing=std::make_unique<TimingCapture>(source_output/(std::to_string(pid)+"-"+std::to_string(born)+"-"+std::to_string(qpc())),pid,born);}catch(...){std::lock_guard l(mutex);status.diagnostics_failed=true;}}}
   for(unsigned u=0;u<6;++u)if(!(current.mask&(1u<<u))&&sources[u])sources[u]->request_stop();
   for(unsigned u=0;u<6;++u){if(!(current.mask&(1u<<u))){if(sources[u]){sources[u].reset();std::lock_guard l(mutex);auto&p=pending[u];p.frame.reset();p.status.reset();p.local_epoch=0;++p.generation;}}else if(!sources[u]&&pid){auto&choices=inventory[u];auto choice=std::find_if(choices.begin(),choices.end(),[&](auto&s){return s.pid==pid&&s.created==born;});if(choice!=choices.end())sources[u]=std::make_unique<LocalSource>(u,choice->pipe,OwnedSink{[this,u](Owned b,const auto&v){frame(u,std::move(b),v);},trace.load(),u});}
    auto v=sources[u]?sources[u]->view():LocalView{};{std::lock_guard l(mutex);status.units[u]=v;}
    if(now>=heartbeat[u]){std::lock_guard l(mutex);auto&p=pending[u];auto state=!(current.mask&(1u<<u))?State::disabled:v.status.find(L"Source busy")!=std::wstring::npos?State::busy:v.connected?State::waiting:State::disconnected;try{p.status=tcp::packet({session,p.origin,u,state,p.generation,++p.sequence,now,{}},{});}catch(...){++status.rejected;}heartbeat[u]=now+frequency();}
   }
   if(instrument_source&&(!instrument_subscribers||!pid)){instrument_source->request_stop();instrument_source.reset();std::lock_guard l(mutex);auto&p=instrument_pending;p.frame.reset();p.status.reset();p.origin={};p.local_epoch=0;++p.generation;}
   if(!instrument_source&&instrument_subscribers&&pid&&now>=instrument_discovery_due){instrument_discovery_due=now+frequency();auto choices=discover_instruments();auto choice=std::find_if(choices.begin(),choices.end(),[&](auto&s){return s.pid==pid&&s.created==born;});if(choice!=choices.end())instrument_source=std::make_unique<LocalSource>(psx::instrument::captain_stab_trim,choice->pipe,OwnedSink{[this](Owned b,const auto&v){instrument_frame(std::move(b),v);},trace.load(),6},InstrumentSourceTag{});}
   auto iv=instrument_source?instrument_source->view():LocalView{};
   {std::lock_guard l(mutex);status.instrument=iv;if(now>=instrument_heartbeat){auto&p=instrument_pending;auto state=!instrument_subscribers?State::disabled:iv.status.find(L"Source busy")!=std::wstring::npos?State::busy:iv.connected?State::waiting:State::disconnected;try{p.status=tcp::instrument_packet({session,p.origin,psx::instrument::captain_stab_trim,state,p.generation,++p.sequence,now,{}},{});}catch(...){++status.instrument_invalid;}instrument_heartbeat=now+frequency();}}
   if(source_timing&&source_timing->failed()){std::lock_guard l(mutex);status.diagnostics_failed=true;}WaitForSingleObject(changed,50);
  }}catch(const std::exception&e){std::lock_guard l(mutex);status.error=e.what();}release();clear();
 }
 void binding_loop(unsigned b){auto&server=*servers[b];TcpHostOptions current;uint64_t seen{},retry{};bool active=false;
  try{while(!quit){TcpHostOptions next;uint64_t rev;{std::lock_guard l(mutex);next=options;rev=revision;}
   if(rev!=seen){if(next.enabled!=current.enabled||next.port!=current.port||next.freshness_ms!=current.freshness_ms||next.all_interfaces!=current.all_interfaces||(b==0&&next.ethernet!=current.ethernet)||next==current){server.stop();active=false;retry=0;}current=next;seen=rev;}
   auto ip=current.all_interfaces?(b?std::string{}:std::string("0.0.0.0")):(b?std::string("127.0.0.1"):current.ethernet);if(current.enabled&&!ip.empty()&&!active&&qpc()>=retry){try{server.observe(trace.load(),7+b);server.events(event_sink,event_context);server.start({ip,current.port,current.freshness_ms,session,2,current.all_interfaces},[this](unsigned u){std::lock_guard l(mutex);auto&p=pending[u];return tcp::Selection{p.frame,p.status,p.origin,instrument_pending.frame,instrument_pending.status,instrument_pending.origin};},[this](int delta){if(delta>0)instrument_subscribers.fetch_add(unsigned(delta));else instrument_subscribers.fetch_sub(unsigned(-delta));SetEvent(changed);});active=true;}catch(const std::exception&e){retry=qpc()+frequency();std::lock_guard l(mutex);status.error=e.what();}}
   server.pump(active?100:1000);auto s=server.snapshot();{std::lock_guard l(mutex);status.bindings[b]=s;if(current.all_interfaces){if(!b)status.loopback=status.ethernet=active;}else if(b)status.loopback=active;else status.ethernet=active;status.published=status.bindings[0].acknowledged+status.bindings[1].acknowledged;status.expired=status.bindings[0].expired+status.bindings[1].expired;status.owned_peak=std::max(status.owned_peak,uint64_t(tcp::payload_peak()));}
  }}catch(const std::exception&e){std::lock_guard l(mutex);status.error=e.what();}server.stop();std::lock_guard l(mutex);if(b)status.loopback=false;else status.ethernet=false;
 }
};
TcpHostPublisher::TcpHostPublisher():impl_(std::make_unique<Impl>()){}TcpHostPublisher::~TcpHostPublisher()=default;
void TcpHostPublisher::configure(TcpHostOptions o){if(!o.port||o.mask>63||o.freshness_ms<250||o.freshness_ms>10000)throw std::runtime_error("TCP host options");{std::lock_guard l(impl_->mutex);if(o==impl_->options)return;impl_->options=std::move(o);++impl_->revision;}impl_->signal();}
void TcpHostPublisher::reconnect(){{std::lock_guard l(impl_->mutex);++impl_->revision;}impl_->signal();}
void TcpHostPublisher::request_stop()noexcept{impl_->request_stop();}void TcpHostPublisher::stop(){impl_->stop();}
void TcpHostPublisher::observe(std::shared_ptr<Trace>t,std::filesystem::path source_output){std::lock_guard l(impl_->mutex);if(impl_->options.enabled||impl_->trace_owner)throw std::runtime_error("Attach TCP trace before activation");impl_->source_output=std::move(source_output);impl_->trace_owner=std::move(t);impl_->trace=impl_->trace_owner.get();}
void TcpHostPublisher::events(psx::norm::DiagnosticSink s,void*c){std::lock_guard l(impl_->mutex);if(impl_->options.enabled)throw std::runtime_error("Attach TCP event sink before activation");impl_->event_sink=s;impl_->event_context=c;}
TcpHostView TcpHostPublisher::view()const{std::lock_guard l(impl_->mutex);auto v=impl_->status;for(unsigned b=0;b<2;++b)if(impl_->servers[b])v.bindings[b].native=impl_->servers[b]->native_snapshot();v.payload_bytes=tcp::payload_bytes();v.payload_peak=tcp::payload_peak();if(auto t=impl_->trace.load()){v.diagnostic_lost=t->lost();v.diagnostics_failed=v.diagnostics_failed||t->failed();}return v;}
}
