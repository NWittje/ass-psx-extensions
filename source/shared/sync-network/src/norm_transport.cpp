#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <normApi.h>
#include <psx/sync/transport.hpp>
#include <psx/sync/mailbox.hpp>
#include <psx/aircraft_sync/protocol.hpp>
#include <psx/status/protocol.hpp>
#include <psx/bridge/protocol.hpp>
#include <psx/sync/presentation.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace psx::sync {
namespace {std::atomic<std::uint64_t> published_margin{UINT64_MAX};}
std::uint64_t clock_margin_ticks()noexcept{return published_margin.load(std::memory_order_acquire);}
std::int64_t native_qpc()noexcept{LARGE_INTEGER n{};QueryPerformanceCounter(&n);return n.QuadPart;}
std::uint64_t native_frequency()noexcept{LARGE_INTEGER n{};QueryPerformanceFrequency(&n);return n.QuadPart;}
namespace {
// The NORM engine owns receive work on its worker. Even malformed remote DATA
// advertisements cannot cause an unbounded allocation before completion checks.
struct ReceiveSlot {std::atomic<bool> used{};alignas(std::max_align_t) std::array<char,max_object> bytes;};
std::array<ReceiveSlot,256> receive_slots;
char* allocate_receive(std::size_t n){if(!n||n>max_object)return nullptr;for(auto& slot:receive_slots){bool free=false;if(slot.used.compare_exchange_strong(free,true,std::memory_order_acq_rel))return slot.bytes.data();}return nullptr;}
void free_receive(char* p){if(!p)return;for(auto& slot:receive_slots)if(slot.bytes.data()==p){slot.used.store(false,std::memory_order_release);return;}}
std::uint64_t nonce(){std::uint64_t n{};do{if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&n),sizeof(n),BCRYPT_USE_SYSTEM_PREFERRED_RNG))throw std::runtime_error("NORM identity generation failed");}while(!n);return n;}
Identity identity(){FILETIME c{},e{},k{},u{};if(!GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u))throw std::runtime_error("NORM process identity unavailable");return {nonce(),nonce(),(std::uint64_t(c.dwHighDateTime)<<32)|c.dwLowDateTime,GetCurrentProcessId()};}
std::wstring adjacent_dll(){HMODULE image{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&native_qpc),&image))throw std::runtime_error("NORM image path unavailable");std::array<wchar_t,32768> b{};const auto n=GetModuleFileNameW(image,b.data(),DWORD(b.size()));if(!n||n>=b.size())throw std::runtime_error("NORM image path truncated");std::wstring p(b.data(),n);return p.substr(0,p.find_last_of(L"\\/"))+L"\\Norm.dll";}
std::string resolve(const std::string& h,HANDLE stop){
 IN_ADDR numeric{};if(InetPtonA(AF_INET,h.c_str(),&numeric)==1)return h;
 ADDRINFOEXW hints{};hints.ai_family=AF_INET;hints.ai_socktype=SOCK_DGRAM;PADDRINFOEXW list{};
 const std::wstring host(h.begin(),h.end());OVERLAPPED op{};op.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!op.hEvent)return {};
 HANDLE cancel{};auto error=GetAddrInfoExW(host.c_str(),nullptr,NS_DNS,nullptr,&hints,&list,nullptr,&op,nullptr,&cancel);
 if(error==WSA_IO_PENDING){HANDLE waits[]{stop,op.hEvent};if(WaitForMultipleObjects(2,waits,FALSE,1000)!=WAIT_OBJECT_0+1){GetAddrInfoExCancel(&cancel);WaitForSingleObject(op.hEvent,INFINITE);}error=GetAddrInfoExOverlappedResult(&op);}
 std::array<char,INET_ADDRSTRLEN>b{};if(!error&&list)InetNtopA(AF_INET,&reinterpret_cast<sockaddr_in*>(list->ai_addr)->sin_addr,b.data(),DWORD(b.size()));if(list)FreeAddrInfoExW(list);CloseHandle(op.hEvent);return b.data();
}
void put(std::uint8_t* p,std::uint64_t n){for(unsigned i=0;i<8;++i)p[i]=std::uint8_t(n>>(i*8));}
std::uint64_t get(const std::uint8_t*p){std::uint64_t n{};for(unsigned i=0;i<8;++i)n|=std::uint64_t(p[i])<<(i*8);return n;}
struct Api {
 HMODULE dll{};bool loaded{};
#define FN(name) decltype(&Norm##name) name{}
 FN(CreateInstance);FN(DestroyInstance);FN(CreateSession);FN(DestroySession);FN(StartReceiver);FN(StopReceiver);FN(StartSender);FN(StopSender);
 FN(SetTxOnly);FN(SetTxPort);FN(SetRxPortReuse);FN(SetDefaultUnicastNack);FN(SetDefaultSyncPolicy);FN(SetRxCacheLimit);FN(SetTxCacheBounds);
 FN(SetTxRate);FN(GetTxRate);FN(SetCongestionControl);FN(SetGrttEstimate);FN(SetTxLoss);FN(SetRxLoss);FN(DataEnqueue);FN(ObjectCancel);FN(GetNextEvent);
 FN(ObjectGetSize);FN(ObjectGetType);FN(DataAccessData);FN(DataDetachData);FN(NodeGetAddress);FN(SendCommand);FN(CancelCommand);FN(NodeGetCommand);FN(GetDescriptor);
 FN(SetAllocationFunctions);
#undef FN
 void load(const std::wstring& p){if(loaded)return;if(dll)FreeLibrary(dll);dll=LoadLibraryExW(p.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);if(!dll)throw std::runtime_error("App-local Norm.dll or its runtime dependencies unavailable");
  // The dynamic adapter must not create static imports of exported constants.
  // Verify this DLL's invalid-handle representation before using null sentinels.
  for(const auto* name:{"NORM_INSTANCE_INVALID","NORM_SESSION_INVALID","NORM_OBJECT_INVALID"}){const auto* value=reinterpret_cast<const void* const*>(GetProcAddress(dll,name));if(!value||*value)throw std::runtime_error("Unsupported NORM invalid-handle representation");}
#define LOAD(name) name=reinterpret_cast<decltype(name)>(GetProcAddress(dll,"Norm" #name));if(!name)throw std::runtime_error("Missing NORM API: " #name)
 LOAD(CreateInstance);LOAD(DestroyInstance);LOAD(CreateSession);LOAD(DestroySession);LOAD(StartReceiver);LOAD(StopReceiver);LOAD(StartSender);LOAD(StopSender);
 LOAD(SetTxOnly);LOAD(SetTxPort);LOAD(SetRxPortReuse);LOAD(SetDefaultUnicastNack);LOAD(SetDefaultSyncPolicy);LOAD(SetRxCacheLimit);LOAD(SetTxCacheBounds);
 LOAD(SetTxRate);LOAD(GetTxRate);LOAD(SetCongestionControl);LOAD(SetGrttEstimate);LOAD(SetTxLoss);LOAD(SetRxLoss);LOAD(DataEnqueue);LOAD(ObjectCancel);LOAD(GetNextEvent);
 LOAD(ObjectGetSize);LOAD(ObjectGetType);LOAD(DataAccessData);LOAD(DataDetachData);LOAD(NodeGetAddress);LOAD(SendCommand);LOAD(CancelCommand);LOAD(NodeGetCommand);LOAD(GetDescriptor);
 LOAD(SetAllocationFunctions);
 loaded=true;
#undef LOAD
 }
 ~Api(){if(dll)FreeLibrary(dll);}
};
struct Pending {Family family{};Lane lane{};std::uint64_t key{},sequence{},floor{};Frame wire;std::int64_t queued{},sent{},completed{};NormObjectHandle object{nullptr};bool active{},control{};};
bool capture_detail(Family family,std::uint64_t key)noexcept{
 const auto kind=unsigned(key&65535u);
 return (family==Family::traffic&&kind==8)||(family==Family::terrain&&(kind==9||kind==10||kind==11||kind==13||kind==14));
}
}
struct Endpoint::Impl {
 Options options;HANDLE stop{CreateEventW(nullptr,TRUE,FALSE,nullptr)},wake{CreateEventW(nullptr,FALSE,FALSE,nullptr)},ready{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
 std::thread thread;mutable std::mutex mutex;LinkStatus view;ClockMap clocks;
 std::atomic<double> requested_loss{};std::atomic<bool> connected_fast{},command_stall{},state_stall{},completion_stall{},fixed_state_pacing{};
 std::array<Pending,256> pending{};
 std::array<unsigned,8> state_cursor{};
 std::array<unsigned,8> state_burst_head{};
 std::array<bool,8> state_burst_active{};
 std::array<unsigned,8> mailbox_cursor{};
 std::array<Pending,128> latest{}; // Producer mailboxes never borrow in-flight DATA memory.
 struct Inbox {std::array<Received,32> values{};unsigned head{},size{};};
 std::array<Inbox,8> inbox;std::array<Received,128> traces{};unsigned trace_head{},trace_size{};bool capturing{};
 // Only local source omissions travel in heartbeat.key. Received peer losses
 // affect the local Capture result without being echoed back as new omissions.
 std::uint64_t capture_detail_lost{},peer_capture_detail_lost{};bool have_peer_capture_detail_lost{};
 Identity self;Hello peer;std::string peer_host;std::uint64_t connection{},boot{},wire_sequence{};
 std::array<std::uint64_t,9> control_sequence{},received_control{},acknowledged{},state_barrier{};
 struct KeyBarrier {std::uint64_t key{},floor{};bool active{};};std::array<KeyBarrier,64> key_barriers{};
 Identity latest_peer;std::array<Identity,16> superseded{};unsigned superseded_cursor{};std::array<std::uint64_t,32> retired_connections{};unsigned retired_cursor{};
 StateWatermarks state_sequences;std::int64_t last_progress{},last_hello{},last_clock{},last_heartbeat{},handshake_started{};
 std::array<std::uint64_t,16> clock_requests{};unsigned clock_cursor{};
 Api api;NormInstanceHandle instance{nullptr};std::array<NormSessionHandle,8> rx{},tx{};NormSessionHandle command_tx{};std::array<std::uint32_t,17> node_ids{};
 struct Cadence {Family family{};std::uint64_t key{};std::int64_t sent{};bool active{};};
 std::array<Cadence,64> last_sent{};
 bool attached{},recreate_native{};ServiceBindings services;std::uint16_t native_session{};
 std::array<Frame,8> command_frames{};std::array<std::int64_t,8> command_queued{};unsigned command_head{},command_size{};bool command_busy{};
 std::array<aircraft_sync::Sample,6> appearance_baseline{};std::array<bool,6> have_appearance_baseline{};
 status::Snapshot readiness{};bool have_readiness{};
 audio::State audio_state{};bool have_audio{};
 std::array<bool,bridge::source_slots> observation_baseline{};
 std::array<std::uint64_t,4> observation_epoch{};
 Impl(){rx.fill(nullptr);tx.fill(nullptr);}
 void service_view(){view.ready_services=0;for(unsigned i=1;i<8;++i){const auto& s=services.at(Family(i));view.local_services[i]=s.local;view.remote_services[i]=s.remote;view.service_generations[i]=s.generation;if(s.ready())view.ready_services|=1u<<i;}}
 void reset_service(Family family){
  for(auto& p:pending)if(p.active&&p.family==family){if(!p.completed)omitted(p.family,p.key,true);cancel(p);}
  for(auto& p:latest)if(p.active&&p.family==family){omitted(p.family,p.key,true);p.active=false;}
  for(auto& p:deferred)if(p.active&&p.header.family==family){omitted(p.header.family,p.header.key,false);p.active=false;}
  auto& q=inbox[unsigned(family)];for(unsigned i=0;i<q.size;++i){const auto& h=q.values[(q.head+i)%q.values.size()].header;omitted(h.family,h.key,false);}q.head=q.size=0;state_sequences.clear(family);
  const auto domain=unsigned(family);
  for(unsigned d=1;d<9;++d)if(d==domain||(family==Family::observation&&d==8)){
   control_sequence[d]=received_control[d]=acknowledged[d]=state_barrier[d]=0;
   for(auto& b:key_barriers)if((b.key>>32)==d)b={};
  }
  for(auto& c:last_sent)if(c.family==family)c={};
  if(family==Family::observation){have_audio=false;observation_baseline={};observation_epoch={};}
  if(family==Family::aircraft)have_appearance_baseline={};
  if(family==Family::status)have_readiness=false;
  services.at(family).dirty=false;service_view();
 }
 void fail_service(Family family){services.close(family,services.at(family).generation);reset_service(family);}
 ~Impl(){if(thread.joinable()){SetEvent(stop);SetEvent(wake);thread.join();}if(stop)CloseHandle(stop);if(wake)CloseHandle(wake);if(ready)CloseHandle(ready);}
 void cancel(Pending& p){if(p.object!=nullptr){api.ObjectCancel(p.object);p.object=nullptr;}p.active=false;}
 void omitted(Family family,std::uint64_t key,bool local)noexcept{
  if(!capture_detail(family,key))return;
  if(local&&capture_detail_lost!=UINT64_MAX)++capture_detail_lost;
  if(capturing&&view.capture_lost!=UINT64_MAX)++view.capture_lost;
 }
 void cancel_command(){if(command_busy&&command_tx)api.CancelCommand(command_tx);command_busy=false;}
 void retire(){connected_fast.store(false,std::memory_order_release);if(connection)retired_connections[retired_cursor++%retired_connections.size()]=connection;for(auto& p:pending)if(p.active){if(!p.completed)omitted(p.family,p.key,true);cancel(p);}cancel_command();if(command_tx){api.StopSender(command_tx);api.DestroySession(command_tx);command_tx=nullptr;}for(auto& t:tx)if(t!=nullptr){api.StopSender(t);api.DestroySession(t);t=nullptr;}for(auto& p:latest){if(p.active)omitted(p.family,p.key,true);p.active=false;}for(auto& q:inbox){for(unsigned i=0;i<q.size;++i){const auto& h=q.values[(q.head+i)%q.values.size()].header;omitted(h.family,h.key,false);}q.head=q.size=0;}for(auto& d:deferred){if(d.active)omitted(d.header.family,d.header.key,false);d.active=false;}peer_capture_detail_lost=0;have_peer_capture_detail_lost=false;command_head=command_size=0;clock_requests={};have_appearance_baseline={};have_readiness=false;have_audio=false;clocks.reset();peer={};connection=0;received_control={};control_sequence={};acknowledged={};state_barrier={};key_barriers={};state_sequences.clear();attached=false;view.connected=view.clock_valid=false;view.peer={};view.remote_address={};view.remote_receive_port=0;view.connection=0;published_margin.store(UINT64_MAX,std::memory_order_release);last_sent={};services.clear();observation_baseline={};observation_epoch={};service_view();++view.generation;++view.reconnects;ResetEvent(ready);}
 void clear_native(){for(auto& p:pending)if(p.active)cancel(p);cancel_command();if(command_tx){api.StopSender(command_tx);api.DestroySession(command_tx);command_tx=nullptr;}for(auto& t:tx)if(t!=nullptr){api.StopSender(t);api.DestroySession(t);t=nullptr;}for(auto& r:rx)if(r!=nullptr){api.StopReceiver(r);api.DestroySession(r);r=nullptr;}if(instance!=nullptr){api.DestroyInstance(instance);instance=nullptr;}}
 std::uint32_t node_id(unsigned slot){if(node_ids[slot])return node_ids[slot];std::uint32_t n{};do{n=std::uint32_t(nonce());}while(!n||n==UINT32_MAX||std::find(node_ids.begin(),node_ids.end(),n)!=node_ids.end());return node_ids[slot]=n;}
 std::uint16_t next_native_session(){if(!++native_session)++native_session;return native_session;}
 void sender(const std::string& host,std::uint16_t port){
  cancel_command();if(command_tx){api.StopSender(command_tx);api.DestroySession(command_tx);command_tx=nullptr;}
  for(auto& t:tx)if(t!=nullptr){api.StopSender(t);api.DestroySession(t);t=nullptr;}
  command_head=command_size=0;
  // Receivers use OS-selected feedback source ports. Connecting a UDP TX socket
  // to the DATA destination port would reject their NACK/congestion feedback.
  // Keep native cache minima at the application slot limits. Otherwise an
  // asynchronous purge may remove an object before its queued PURGED event is
  // consumed, racing our cancellation of that same object during service reset.
  // The larger ID windows also cover sparse, slow-changing pose records mixed
  // with100Hz keys; actual live DATA count/bytes stay bounded by our slots.
  // Ordered DATA has128 fixed slots and a1s lifetime. At the existing20Mbps
  // allocation, even128 maximum8192-byte objects serialize in about420ms before
  // protocol overhead. Idle NORM congestion backoff can otherwise reduce this
  // lane below one small control burst per second. Like the separate command
  // sender, pace this bounded lane explicitly; every state lane remains adaptive.
  const bool fixture_fixed=fixed_state_pacing.load(std::memory_order_acquire);
  for(unsigned i=0;i<8;++i){auto& t=tx[i];t=api.CreateSession(instance,host.c_str(),std::uint16_t(port+i),node_id(8+i));if(t==nullptr)throw std::runtime_error("NORM unicast session creation failed");api.SetTxOnly(t,true,false);if(!api.SetTxPort(t,0,false,nullptr))throw std::runtime_error("NORM OS-route sender bind failed");api.SetCongestionControl(t,i!=0&&!fixture_fixed,i!=0&&!fixture_fixed);api.SetGrttEstimate(t,.005);api.SetTxCacheBounds(t,i==0?1024*1024:256*1024,i==0?128:32,8192);if(!api.StartSender(t,next_native_session(),512*1024,1200,16,0,0))throw std::runtime_error("NORM unicast sender start failed");api.SetTxRate(t,20e6);if(options.fixture_loss_percent)api.SetTxLoss(t,options.fixture_loss_percent);}
  view.fixture_fixed_state_pacing=fixture_fixed;
  command_tx=api.CreateSession(instance,host.c_str(),port,node_id(16));if(!command_tx)throw std::runtime_error("NORM command sender creation failed");
  api.SetTxOnly(command_tx,true,false);if(!api.SetTxPort(command_tx,0,false,nullptr))throw std::runtime_error("NORM command sender bind failed");
  api.SetCongestionControl(command_tx,false,false);if(!api.StartSender(command_tx,next_native_session(),32768,1200,16,0,0))throw std::runtime_error("NORM command sender start failed");
  api.SetTxRate(command_tx,2e6);if(options.fixture_loss_percent)api.SetTxLoss(command_tx,options.fixture_loss_percent);
  handshake_started=native_qpc();
 }
 bool command(Kind kind,std::span<const std::uint8_t> bytes={},std::uint64_t key=0,unsigned domain=0,Family family=Family::link){
  if(command_tx==nullptr||command_size==command_frames.size())return false;
  if(kind==Kind::ack)family=domain==8?Family::observation:Family(domain);
  Header h;h.kind=kind;h.family=family;h.producer=self;h.frequency=view.local_frequency;h.connection=connection;h.sequence=++wire_sequence;h.observed_qpc=native_qpc();h.key=key;h.control_floor=domain;
  if(family!=Family::link){const auto& s=services.at(family);h.source_service=s.local;h.target_service=s.remote;}
  const auto slot=(command_head+command_size)%command_frames.size();auto& f=command_frames[slot];if(!encode(h,bytes,f))return false;
  command_queued[slot]=std::int64_t(h.observed_qpc);++command_size;return true;
 }
 void hello(){Hello h;h.role=options.role;h.receive_port=options.receive_port;h.boot=boot;h.frequency=view.local_frequency;h.identity=self;std::array<std::uint8_t,56>b{};std::size_t n{};if(encode_hello(h,b,n))command(Kind::hello,std::span(b).first(n));}
 bool peer_address(NormNodeHandle n,std::string& h){std::array<char,16>b{};unsigned size=unsigned(b.size());UINT16 port{};if(!api.NodeGetAddress(n,b.data(),&size,&port)||size!=4)return false;std::array<char,INET_ADDRSTRLEN> s{};if(!InetNtopA(AF_INET,b.data(),s.data(),DWORD(s.size())))return false;h=s.data();return true;}
 void trace(const Header& h,std::span<const std::uint8_t> b,std::int64_t now,bool transmitted=false){if(!capturing)return;if(trace_size==traces.size()){++view.capture_lost;return;}auto& p=traces[(trace_head+trace_size++)%traces.size()];p.header=h;p.size=b.size();p.received_qpc=now;p.transmitted=transmitted;p.source_time=clocks.to_local(std::int64_t(h.observed_qpc),now);std::copy(b.begin(),b.end(),p.payload.begin());}
 bool admit(const Header& h,std::span<const std::uint8_t> b,std::int64_t now){auto& q=inbox[unsigned(h.family)];Received* selected{};
  if(!(h.flags&ordered)){
   // A new reliable floor does not give the same latest-only state another
   // inbox slot. Remove older-floor copies with one bounded stable compaction;
   // the successor is appended after its already-admitted ordered prerequisites.
   // Replacing an older-floor slot in place could move new state ahead of the
   // very Subscribe/ownership/control on which it depends. Ordered records never
   // move relative to one another or lose reserved capacity to obsolete state.
   unsigned kept{};
   for(unsigned i=0;i<q.size;++i){auto& prior=q.values[(q.head+i)%q.values.size()];
    if(!(prior.header.flags&ordered)&&prior.header.key==h.key&&prior.header.control_floor<h.control_floor){++view.replaced;omitted(prior.header.family,prior.header.key,false);continue;}
    if(kept!=i)q.values[(q.head+kept)%q.values.size()]=prior;++kept;
   }q.size=kept;
   for(unsigned i=0;i<q.size;++i){auto& prior=q.values[(q.head+i)%q.values.size()];if(!(prior.header.flags&ordered)&&prior.header.key==h.key&&prior.header.control_floor==h.control_floor){selected=&prior;++view.replaced;omitted(prior.header.family,prior.header.key,false);break;}}
  }
  if(!selected){if(q.size==q.values.size()){++view.rejected;omitted(h.family,h.key,false);return !(h.flags&ordered);}selected=&q.values[(q.head+q.size++)%q.values.size()];}
  auto& p=*selected;p.header=h;p.size=b.size();p.received_qpc=now;p.source_time=clocks.to_local(std::int64_t(h.observed_qpc),now);std::copy(b.begin(),b.end(),p.payload.begin());SetEvent(ready);return true;}
 struct Deferred {Header header;std::array<std::uint8_t,max_payload> bytes{};std::size_t size{};std::int64_t received{};bool active{};};
 // Observation also carries the independent audio control domain. Reserve
 // enough slots per family so a stalled optional lane cannot consume pose's.
 std::array<Deferred,256> deferred;
 void barrier(const Header& h,std::span<const std::uint8_t> body){const auto domain=unsigned(h.key>>32);const auto kind=unsigned(h.key&65535u);bool ordinary_control{};std::uint64_t key{};classify(h.family,body,ordinary_control,key);
  if(!ordinary_control){KeyBarrier* slot{};for(auto& b:key_barriers)if(b.active&&b.key==h.key){slot=&b;break;}if(!slot)for(auto& b:key_barriers)if(!b.active){slot=&b;break;}if(slot)*slot={h.key,h.sequence,true};}
  else if(kind==1||kind==2||(domain==1&&(kind==7||kind==10))||(domain==8&&kind==14)||
      (h.family==Family::terrain&&kind==5)||(h.family==Family::aircraft&&kind==4)||(h.family==Family::environment&&kind==4))state_barrier[domain]=h.sequence;
 }
 bool obsolete(const Header& h)const{const auto domain=unsigned(h.key>>32);if(h.control_floor<state_barrier[domain])return true;for(const auto& b:key_barriers)if(b.active&&b.key==h.key&&h.control_floor<b.floor)return true;return false;}
 Family flush_deferred(){bool changed=true;while(changed){changed=false;for(auto& p:deferred)if(p.active&&(p.header.flags&ordered)){const auto domain=unsigned(p.header.key>>32);if((domain==8&&!received_control[1])||p.header.sequence!=received_control[domain]+1)continue;if(!admit(p.header,std::span(p.bytes).first(p.size),p.received))return p.header.family;received_control[domain]=p.header.sequence;barrier(p.header,std::span(p.bytes).first(p.size));p.active=false;changed=true;}}
  // Audio has an independent repair domain, but shares the observation Hello.
  // No typed message may reach its service framer before that binding exists.
  for(auto& p:deferred)if(p.active&&!(p.header.flags&ordered)&&((p.header.key>>32)!=8||received_control[1])&&p.header.control_floor<=received_control[unsigned(p.header.key>>32)]){if(!obsolete(p.header)&&state_sequences.newer(p.header.family,p.header.key,p.header.sequence)){if(!admit(p.header,std::span(p.bytes).first(p.size),p.received)||!state_sequences.commit(p.header.family,p.header.key,p.header.sequence))return p.header.family;}p.active=false;}return Family::link;}
 void packet(std::span<const std::uint8_t> bytes,const std::string& host){Header h;std::span<const std::uint8_t>b;if(!decode(bytes,h,b)||h.wire_version!=version){++view.rejected;return;}const auto now=native_qpc();
  if(h.kind==Kind::hello){Hello next;if(!decode_hello(b,next)||next.identity!=h.producer||next.frequency!=h.frequency||next.role==options.role){++view.rejected;return;}
   if(!options.host.empty()&&!peer_host.empty()&&host!=peer_host){++view.rejected;return;}
   if(h.connection&&std::find(retired_connections.begin(),retired_connections.end(),h.connection)!=retired_connections.end()){++view.rejected;return;}if(attached&&next.identity!=peer.identity&&host!=peer_host){++view.rejected;return;}
   if(std::find(superseded.begin(),superseded.end(),next.identity)!=superseded.end()){++view.rejected;return;}
   const bool replaced=valid(latest_peer)&&next.identity!=latest_peer;
   if(replaced)superseded[superseded_cursor++%superseded.size()]=latest_peer;latest_peer=next.identity;
   // Release receiver metadata for the departed process as well as TX caches.
   // Same-process reconnects reuse native node IDs with a new session number.
   if(replaced){recreate_native=true;return;}
   // A zero-connection Hello can be delayed or repeated while the opposite
   // peer is still learning the coordinator's newly chosen connection. It is
   // not a new producer generation and must not repeatedly tear that binding
   // down. Nor may it refresh an established link's qualified progress: if the
   // peer has genuinely retired this connection, normal one-second expiry
   // chooses a fresh identity instead of keeping the dead binding alive.
   if(connection&&!h.connection&&next.identity==peer.identity&&next.boot==peer.boot){hello();return;}
   const bool reset_connection=connection&&options.role==Role::plugin&&h.connection&&h.connection!=connection;
   if(next.identity!=peer.identity||next.boot!=peer.boot||reset_connection){retire();peer=next;peer_host=host;sender(host,next.receive_port);connection=options.role==Role::coordinator?nonce():h.connection;last_hello=0;}
   if(options.role==Role::plugin&&h.connection)connection=h.connection;
   view.peer=peer.identity;view.remote_address={};std::copy_n(host.data(),(std::min)(host.size(),view.remote_address.size()-1),view.remote_address.data());view.remote_receive_port=peer.receive_port;view.remote_frequency=peer.frequency;view.connection=connection;last_progress=now;attached=connection!=0;command(Kind::attach);return;
  }
  if(!connection||h.connection!=connection||h.producer!=peer.identity||h.frequency!=peer.frequency||host!=peer_host){++view.rejected;return;}
  last_progress=now;view.last_received_qpc=now;trace(h,b,now);
  if(h.kind==Kind::heartbeat){
   ServiceAnnouncements announced;if(!decode_services(b,announced)){++view.rejected;return;}
   if(!have_peer_capture_detail_lost){peer_capture_detail_lost=h.key;have_peer_capture_detail_lost=true;}
   else if(h.key>peer_capture_detail_lost){const auto omitted=h.key-peer_capture_detail_lost;peer_capture_detail_lost=h.key;
    if(capturing)view.capture_lost=omitted>UINT64_MAX-view.capture_lost?UINT64_MAX:view.capture_lost+omitted;
   }
   for(unsigned f=1;f<8;++f){const auto& a=announced[f-1];if(!a.incarnation)continue;const auto family=Family(f);
    if(services.observe(family,a.incarnation,a.echo,a.open)&&services.at(family).dirty)reset_service(family);
   }service_view();return;
  }
  if(h.kind==Kind::frame||h.kind==Kind::ack){
   if(h.family==Family::link){++view.rejected;return;}auto& s=services.at(h.family);
   if(!s.active||!s.remote_open||h.source_service!=s.remote||h.target_service!=s.local){++view.rejected;return;}
   if(s.echo!=s.local){s.echo=s.local;service_view();}
  }
  if(h.kind==Kind::detach){retire();return;}
  if(h.kind==Kind::attach){attached=true;return;}
  if(h.kind==Kind::clock_request){std::array<std::uint8_t,24> reply{};put(reply.data(),h.key);put(reply.data()+8,std::uint64_t(now));put(reply.data()+16,std::uint64_t(native_qpc()));command(Kind::clock_reply,reply);return;}
  if(h.kind==Kind::clock_reply){if(b.size()!=24)return;const auto t1=get(b.data());auto found=std::find(clock_requests.begin(),clock_requests.end(),t1);if(found==clock_requests.end()||!t1)return;*found=0;const auto before=clocks.generation();const bool was_current=clocks.current(now);if(clocks.accept({t1,get(b.data()+8),get(b.data()+16),std::uint64_t(now),view.local_frequency,peer.frequency})){if(was_current&&before!=clocks.generation()){retire();return;}view.clock_valid=true;view.clock_uncertainty_ms=clocks.uncertainty_ms(now);}return;}
  if(h.kind==Kind::ack){const auto domain=h.control_floor;if(!domain||domain>=acknowledged.size()){++view.rejected;return;}acknowledged[domain]=(std::max)(acknowledged[domain],h.key);for(auto& p:pending)if(p.active&&p.control&&(p.key>>32)==domain&&p.sequence<=acknowledged[domain])cancel(p);return;}
  if(h.kind!=Kind::frame||h.family==Family::link||!clocks.current(now)){++view.rejected;return;}
  const auto domain=unsigned(h.key>>32);if(!domain||domain>8||(domain!=(h.family==Family::observation&&((h.key&65535u)>=12&&(h.key&65535u)<=15)?8:unsigned(h.family)))){++view.rejected;return;}
  bool ordinary_control{};std::uint64_t semantic_key{};classify(h.family,b,ordinary_control,semantic_key);
  if(!semantic_key||h.key!=(semantic_key|(std::uint64_t(domain)<<32))){++view.rejected;return;}
  if((h.flags&ordered)&&h.sequence<=received_control[domain]){command(Kind::ack,{},received_control[domain],domain);return;}
  if(!(h.flags&ordered)&&!state_sequences.newer(h.family,h.key,h.sequence))return;
  unsigned occupied{};for(const auto& p:deferred)if(p.active&&p.header.family==h.family)++occupied;Deferred* slot{};for(auto& p:deferred)if(p.active&&(p.header.key>>32)==domain&&p.header.sequence==h.sequence&&p.header.flags==h.flags)return;
  if(!(h.flags&ordered))for(auto& p:deferred)if(p.active&&!(p.header.flags&ordered)&&p.header.family==h.family&&p.header.key==h.key){slot=&p;++view.replaced;omitted(p.header.family,p.header.key,false);break;}
  if(!slot&&occupied>=(h.family==Family::observation?64u:32u)){++view.rejected;fail_service(h.family);return;}if(!slot)for(auto& p:deferred)if(!p.active){slot=&p;break;}if(!slot){++view.rejected;fail_service(h.family);return;}
  slot->header=h;slot->size=b.size();slot->received=now;slot->active=true;std::copy(b.begin(),b.end(),slot->bytes.begin());for(unsigned budget=0;budget<7;++budget){const auto failed=flush_deferred();if(failed==Family::link)break;fail_service(failed);if(failed==h.family)return;}++view.received;if(h.flags&ordered)command(Kind::ack,{},received_control[domain],domain);
 }
 void events(){NormEvent e{};for(unsigned budget=0;budget<128&&api.GetNextEvent(instance,&e,false);++budget){
   if(e.type==NORM_TX_OBJECT_PURGED){for(auto& p:pending)if(p.active&&p.object==e.object){p.object=nullptr;if(!p.control)p.active=false;break;}}
   else if(e.type==NORM_TX_CMD_SENT&&e.session==command_tx){if(command_stall.load(std::memory_order_acquire))continue;if(command_busy&&command_size){command_head=(command_head+1)%command_frames.size();--command_size;}command_busy=false;}
   else if(e.type==NORM_RX_CMD_NEW){std::array<char,1024>b{};unsigned n=unsigned(b.size());std::string host;if(peer_address(e.sender,host)&&api.NodeGetCommand(e.sender,b.data(),&n))packet({reinterpret_cast<std::uint8_t*>(b.data()),n},host);}
   else if(e.type==NORM_TX_OBJECT_SENT){++view.native_objects_sent;if(!completion_stall.load(std::memory_order_acquire))for(auto& p:pending)if(p.active&&p.object==e.object){p.completed=native_qpc();break;}}
   else if(e.type==NORM_RX_OBJECT_ABORTED)++view.native_objects_aborted;
   else if(e.type==NORM_RX_OBJECT_NEW){++view.native_objects_new;if(api.ObjectGetType(e.object)!=NORM_OBJECT_DATA||api.ObjectGetSize(e.object)>max_object){++view.rejected;api.ObjectCancel(e.object);}}
   else if(e.type==NORM_RX_OBJECT_COMPLETED){++view.native_objects_complete;const auto n=api.ObjectGetSize(e.object);if(api.ObjectGetType(e.object)!=NORM_OBJECT_DATA||n>max_object||n<header_size){++view.rejected;api.ObjectCancel(e.object);continue;}std::unique_ptr<char,void(*)(char*)> data(api.DataDetachData(e.object),free_receive);if(data){std::string host;if(peer_address(e.sender,host))packet({reinterpret_cast<const std::uint8_t*>(data.get()),std::size_t(n)},host);}}
   else if(e.type==NORM_SEND_ERROR)++view.native_errors;
  }}
 void transmit(std::int64_t now){
  for(unsigned priority:std::array<unsigned,8>{0,2,1,3,4,6,5,7}){
   if(priority&&state_stall.load(std::memory_order_acquire))continue;
   // Replenished low-index mailboxes cannot monopolize the next freed native
   // slot while another semantic key waits behind a full16-slot reservation.
   const auto mailbox_start=mailbox_cursor[priority];
   if(priority)for(unsigned visited=0;visited<latest.size();++visited){const auto index=(mailbox_start+visited)&(latest.size()-1);auto& waiting=latest[index];if(!waiting.active||unsigned(waiting.lane)!=priority)continue;
    const auto ttl=waiting.family==Family::observation&&(waiting.key&65535u)==3&&(((waiting.key>>16)&65535u)!=6)?1000u:100u;
    if(now-waiting.queued>=std::int64_t(view.local_frequency*ttl/1000)){omitted(waiting.family,waiting.key,true);waiting.active=false;++view.expired;continue;}
    // The native cache retains completed DATA for repair. Once a newer complete
    // state for the same semantic key is waiting, that older state is obsolete;
    // cancel it on this NORM owner before reusing its immutable borrowed bytes.
    // Otherwise sixteen already-sent records can occupy the entire lane for
    //100ms, starving the next seven-slot SDK observation despite a healthy link.
    // Unconfirmed transmissions and keys without a successor keep their normal
    // completion/repair lifetime and omission accounting.
    for(unsigned i=128+priority*16;i<128+(priority+1)*16;++i){auto& prior=pending[i];
     if(prior.active&&!prior.control&&prior.completed&&prior.family==waiting.family&&prior.key==waiting.key&&prior.sequence<waiting.sequence)cancel(prior);
    }
    Pending* target{};for(unsigned i=128+priority*16;i<128+(priority+1)*16;++i)if(!pending[i].active){target=&pending[i];break;}
    if(!target)break;*target=waiting;waiting.active=false;mailbox_cursor[priority]=unsigned((index+1)&(latest.size()-1));
   }
   unsigned budget=priority==0?16:4;
   const unsigned first=priority?128+priority*16:0,count=priority?16:128;
   const unsigned start=priority?state_cursor[priority]:0;
   // Continuously refreshed low-index keys must not consume every bounded
   // native enqueue opportunity before the other keys in this lane. Keep the
   // ordered reservation's scan order, and rotate only independent state work.
   for(unsigned visited=0;visited<count&&budget;++visited){const unsigned offset=(start+visited)&(count-1);auto& p=pending[first+offset];if(!p.active||unsigned(p.lane)!=priority)continue;
    if(p.control&&now-p.queued>=std::int64_t(view.local_frequency)){fail_service(p.family);return;}
    const auto ttl=p.family==Family::observation&&(p.key&65535u)==3&&(((p.key>>16)&65535u)!=6)?1000u:100u;
    if(!p.control&&now-p.queued>=std::int64_t(view.local_frequency*ttl/1000)){if(!p.completed)omitted(p.family,p.key,true);cancel(p);++view.expired;continue;}
    // Enqueueing does not mean the native sender has transmitted this DATA.
    // Cancelling a paced, incomplete object every250ms can repeatedly replace
    // the tail of a bounded burst before it ever reaches the wire. Native NORM
    // owns transmission/repair until completion; ACK retirement and the original
    // one-second service deadline still bound the borrowed bytes.
    if(p.object!=nullptr){if(!p.control||!p.completed||now-p.completed<std::int64_t(view.local_frequency/4))continue;api.ObjectCancel(p.object);p.object=nullptr;}
    else if(p.control&&p.sent&&now-p.sent<std::int64_t(view.local_frequency/4))continue;
    Cadence* cadence{};if(!p.control){for(auto& c:last_sent)if(c.active&&c.family==p.family&&c.key==p.key){cadence=&c;break;}if(!cadence)for(auto& c:last_sent)if(!c.active){c={p.family,p.key,0,true};cadence=&c;break;}if(!cadence){++view.rejected;cancel(p);continue;}if(now-cadence->sent<std::int64_t(view.local_frequency*period_ms(p.lane,p.key)/1000))continue;}
    if(tx[priority]==nullptr)continue;
    p.object=api.DataEnqueue(tx[priority],reinterpret_cast<const char*>(p.wire.bytes.data()),NormSize(p.wire.size),nullptr,0);
    if(p.object!=nullptr){Header h;std::span<const std::uint8_t>b;if(decode(std::span(p.wire.bytes).first(p.wire.size),h,b))trace(h,b,now,true);p.sent=now;p.completed=0;if(cadence)cadence->sent=now;++view.sent;--budget;if(priority){if(!state_burst_active[priority]){state_burst_head[priority]=offset;state_burst_active[priority]=true;}state_cursor[priority]=(offset+1)&(count-1);}}
   }
   // Advancing only after the final submitted slot repeats the same FIFO head
   // when a six-key burst spans two four-enqueue passes. A paced native sender
   // can then lose the same tail at every100ms expiry. Once a complete scan has
   // spare budget, begin the next burst after this burst's first submitted slot.
   if(priority&&budget&&state_burst_active[priority]){state_cursor[priority]=(state_burst_head[priority]+1)&(count-1);state_burst_active[priority]=false;}
  }
 }
 void run(){WSADATA winsock{};bool ws=false;try{
  if(WSAStartup(MAKEWORD(2,2),&winsock))throw std::runtime_error("Winsock initialization failed");ws=true;
  api.load(options.dll_path.empty()?adjacent_dll():options.dll_path);instance=api.CreateInstance(false);if(instance==nullptr)throw std::runtime_error("NORM instance creation failed");
  api.SetAllocationFunctions(instance,allocate_receive,free_receive);
  for(unsigned i=0;i<8;++i){auto& r=rx[i];r=api.CreateSession(instance,"127.0.0.1",std::uint16_t(options.receive_port+i),node_id(i));if(r==nullptr)throw std::runtime_error("NORM receiver session creation failed");api.SetRxPortReuse(r,false,nullptr,nullptr,0);api.SetDefaultUnicastNack(r,true);api.SetDefaultSyncPolicy(r,NORM_SYNC_CURRENT);api.SetRxCacheLimit(r,32);if(!api.StartReceiver(r,512*1024))throw std::runtime_error("NORM receive ports unavailable");if(options.fixture_loss_percent)api.SetRxLoss(r,options.fixture_loss_percent);}
  {std::lock_guard lock(mutex);recreate_native=false;view.running=true;view.error=0;view.failure={};}
  std::int64_t resolve_at{};
  while(WaitForSingleObject(stop,0)!=WAIT_OBJECT_0){
   {std::lock_guard lock(mutex);auto now=native_qpc();for(unsigned f=1;f<8;++f)if(services.at(Family(f)).dirty)reset_service(Family(f));
    const bool fixture_fixed=fixed_state_pacing.load(std::memory_order_acquire);
    if(fixture_fixed!=view.fixture_fixed_state_pacing){
     for(unsigned i=1;i<tx.size();++i)if(tx[i]){api.SetCongestionControl(tx[i],!fixture_fixed,!fixture_fixed);if(fixture_fixed)api.SetTxRate(tx[i],20e6);}
     view.fixture_fixed_state_pacing=fixture_fixed;
    }
    const auto loss=requested_loss.load(std::memory_order_acquire);if(loss!=options.fixture_loss_percent){options.fixture_loss_percent=loss;for(auto r:rx)if(r)api.SetRxLoss(r,loss);for(auto t:tx)if(t)api.SetTxLoss(t,loss);if(command_tx)api.SetTxLoss(command_tx,loss);}
    if(!options.host.empty()&&tx[0]==nullptr&&now>=resolve_at){peer_host=resolve(options.host,stop);if(!peer_host.empty())sender(peer_host,options.destination_port);resolve_at=now+std::int64_t(view.local_frequency);}
    events();
    if(recreate_native)break;
    // Receive processing records newer QPC observations. Expiry must use a
    // subsequently sampled time, never the loop-entry time preceding them.
    now=native_qpc();
    if(view.clock_valid&&!clocks.current(now))retire();
    if(attached&&last_progress&&now-last_progress>=std::int64_t(view.local_frequency)){retire();last_hello=now-std::int64_t(view.local_frequency);}
    if(tx[0]!=nullptr&&now-last_hello>=std::int64_t(view.local_frequency/4)){hello();last_hello=now;}
    if(attached&&now-last_clock>=std::int64_t(view.local_frequency/10)){const auto stamp=std::uint64_t(native_qpc());clock_requests[clock_cursor++%clock_requests.size()]=stamp;command(Kind::clock_request,{},stamp);last_clock=now;}
    // One coherent keepalive carries all service bindings. Separate periodic
    // announcements can crowd clock replies out of the native command queue.
    if(attached&&now-last_heartbeat>=std::int64_t(view.local_frequency/4)){
     ServiceAnnouncements announced;for(unsigned f=1;f<8;++f){const auto& s=services.at(Family(f));announced[f-1]={s.local,s.remote,s.active};}
     std::array<std::uint8_t,service_announcement_bytes> body{};if(encode_services(announced,body))command(Kind::heartbeat,body,capture_detail_lost);last_heartbeat=now;for(unsigned i=0;i<tx.size();++i)view.native_lane_rate_bps[i]=tx[i]?api.GetTxRate(tx[i]):0;view.native_control_rate_bps=view.native_lane_rate_bps[0];
    }
    view.clock_valid=clocks.current(now);view.connected=attached&&view.clock_valid;connected_fast.store(view.connected,std::memory_order_release);view.clock_uncertainty_ms=clocks.uncertainty_ms(now);
    // A peer that has been discovered but never reaches a qualified connection
    // must not remain in an unbounded native half-open session. Rebuild the
    // complete native instance after the same one-second progress deadline;
    // the outer worker waits one second before its next connection attempt.
    if(!view.connected&&tx[0]!=nullptr&&handshake_started&&now-handshake_started>=std::int64_t(view.local_frequency)){
        view.error=ERROR_TIMEOUT;constexpr char reason[]="NORM handshake progress expired";std::copy(std::begin(reason),std::end(reason),view.failure.begin());break;
    }
    published_margin.store(view.clock_valid?static_cast<std::uint64_t>(std::ceil(view.clock_uncertainty_ms*.001*view.local_frequency)):UINT64_MAX,std::memory_order_release);
    if(view.connected)transmit(now);
    // Disposable link/clock commands cannot wedge handshake or retain borrowed
    // bytes indefinitely. Cancel suppresses a late completion for the old slot.
    for(unsigned budget=0;budget<command_frames.size()&&command_size&&now-command_queued[command_head]>=std::int64_t(view.local_frequency/4);++budget){cancel_command();command_head=(command_head+1)%command_frames.size();--command_size;++view.expired;}
    if(!command_busy&&command_size&&command_tx){const auto& f=command_frames[command_head];command_busy=api.SendCommand(command_tx,reinterpret_cast<const char*>(f.bytes.data()),unsigned(f.size),false);if(command_busy){Header h;std::span<const std::uint8_t>b;if(decode(std::span(f.bytes).first(f.size),h,b))trace(h,b,now,true);}}
   }
   HANDLE waits[]={stop,wake,reinterpret_cast<HANDLE>(api.GetDescriptor(instance))};WaitForMultipleObjects(3,waits,FALSE,2);
  }
 }catch(const std::exception& e){std::lock_guard lock(mutex);view.error=GetLastError()?GetLastError():ERROR_INVALID_DATA;view.failure={};std::copy_n(e.what(),(std::min)(std::strlen(e.what()),view.failure.size()-1),view.failure.data());++view.native_errors;}
 catch(...){std::lock_guard lock(mutex);view.error=GetLastError()?GetLastError():ERROR_INVALID_DATA;++view.native_errors;}
 {std::lock_guard lock(mutex);retire();clear_native();view.running=view.connected=view.clock_valid=false;SetEvent(ready);}if(ws)WSACleanup();}
};
Endpoint::Endpoint():impl_(std::make_unique<Impl>()){}
Endpoint::~Endpoint()=default;
bool Endpoint::start(const Options& o){if(!impl_->stop||!impl_->wake||!impl_->ready||impl_->thread.joinable()||!o.receive_port||o.receive_port>65528||!o.destination_port||o.destination_port>65528||o.fixture_loss_percent<0||o.fixture_loss_percent>100)return false;impl_->options=o;impl_->requested_loss.store(o.fixture_loss_percent);impl_->self=identity();impl_->boot=nonce();impl_->view.local=impl_->self;impl_->view.local_frequency=native_frequency();ResetEvent(impl_->stop);impl_->thread=std::thread([p=impl_.get()]{do{p->run();}while(WaitForSingleObject(p->stop,1000)==WAIT_TIMEOUT);});return true;}
void Endpoint::stop()noexcept{SetEvent(impl_->stop);SetEvent(impl_->wake);if(impl_->thread.joinable())impl_->thread.join();}
bool Endpoint::send(Family family,std::span<const std::uint8_t> bytes,std::uint64_t expected_connection,std::uint64_t expected_service){if(family==Family::link||family>Family::actor||bytes.size()<16||bytes.size()>max_payload)return false;auto& s=*impl_;std::lock_guard lock(s.mutex);if(!s.view.connected||(expected_connection&&s.connection!=expected_connection))return false;const auto& service=s.services.at(family);if(!service.ready()||service.dirty||(expected_service&&service.generation!=expected_service))return false;bool ordered{};std::uint64_t key{};auto lane=classify(family,bytes,ordered,key);if(!key){++s.view.rejected;return false;}const auto domain=family==Family::observation&&bytes[6]>=12&&bytes[6]<=15?8u:unsigned(family);key|=std::uint64_t(domain)<<32;Pending* slot{};unsigned controls{};
 aircraft_sync::Sample appearance;status::Snapshot readiness;audio::State audio_state;int appearance_group=-1,initial_observation=-1;bool changed_readiness=false,changed_audio=false;
 std::array<std::uint64_t,4> observation_epoch{};bool changed_observation_epoch=false;
 if(family==Family::observation&&bytes[6]==2){bridge::SessionStatus session;if(bridge::decode(bytes.data()+16,bytes.size()-16,session)){
  observation_epoch={session.session_epoch,session.configuration_epoch,session.boost_generation,session.motion_epoch};changed_observation_epoch=observation_epoch!=s.observation_epoch;
 }}
 if(family==Family::observation&&bytes[6]==3){
  bridge::ObservationMessage observation;
  if(bridge::decode(bytes.data()+16,bytes.size()-16,observation)){
   const auto source=unsigned(observation.observation.source);
   // Main records are often sent only when changed. Losing their first
   // replaceable packet can otherwise leave pitch/configuration absent forever.
   // Deliver one ordered baseline per source and service binding, then keep
   // ordinary updates latest-only with their unchanged original timestamps.
   if(source<s.observation_baseline.size()&&!s.observation_baseline[source]){ordered=true;lane=Lane::control;initial_observation=int(source);}
  }
 }
 if(family==Family::observation&&bytes[6]==12&&bridge::decode(bytes.data()+16,bytes.size()-16,audio_state)){
  const auto& before=s.audio_state;changed_audio=!s.have_audio||before.audio_epoch!=audio_state.audio_epoch||before.producer_high!=audio_state.producer_high||before.producer_low!=audio_state.producer_low||before.available_groups!=audio_state.available_groups;
  for(unsigned g=0;g<8&&!changed_audio;++g)if(audio_state.available_groups&(1u<<g)){const auto mask=~audio_continuous_mask(g);const auto& a=before.groups[g];const auto& b=audio_state.groups[g];changed_audio=a.lifecycle_epoch!=b.lifecycle_epoch||a.configuration_epoch!=b.configuration_epoch||a.source!=b.source||((a.valid_fields^b.valid_fields)&mask);
   for(unsigned i=0;i<32&&!changed_audio;++i)if((mask&b.valid_fields&(1u<<i))&&a.values[i]!=b.values[i])changed_audio=true;}
  if(changed_audio){ordered=true;lane=Lane::control;}
 }
 if(family==Family::aircraft&&bytes[6]==3&&aircraft_sync::decode(bytes.data()+16,bytes.size()-16,appearance)){
  const auto group=unsigned(appearance.group);const auto& before=s.appearance_baseline[group];appearance_group=int(group);
  const bool changed=!s.have_appearance_baseline[group]||before.producer!=appearance.producer||before.lifecycle!=appearance.lifecycle||before.configuration!=appearance.configuration||before.source!=appearance.source||before.flags!=appearance.flags||before.valid_mask!=appearance.valid_mask||before.approximation_mask!=appearance.approximation_mask||before.values!=appearance.values;
  // Public Main records may remain silent while their value is unchanged.
  // Preserve their first/changed baseline reliably, without new observations
  // or retimed source clocks. Native continuous surfaces stay latest-only.
  if(changed&&(group>=4||appearance.source==aircraft_sync::Source::public_record)){ordered=true;lane=Lane::control;}
 }
 if(family==Family::status&&bytes[6]==3&&status::decode(bytes.data()+16,bytes.size()-16,readiness)){
  const auto& before=s.readiness;
  if(!s.have_readiness||before.producer!=readiness.producer||before.lifecycle!=readiness.lifecycle||before.flags!=readiness.flags||before.reason!=readiness.reason||before.placement_attempt!=readiness.placement_attempt||before.aircraft_generation!=readiness.aircraft_generation||before.scenery_generation!=readiness.scenery_generation){ordered=true;lane=Lane::control;changed_readiness=true;}
 }
 for(auto& p:s.pending){if(p.active&&p.control&&(p.key>>32)==domain)++controls;if(!ordered&&p.active&&!p.control&&p.family==family&&p.key==key&&p.object==nullptr){slot=&p;++s.view.replaced;s.omitted(p.family,p.key,true);break;}}
 if(!ordered&&!slot){for(auto& p:s.latest)if(p.active&&p.family==family&&p.key==key){slot=&p;++s.view.replaced;s.omitted(p.family,p.key,true);break;}if(!slot)for(auto& p:s.latest)if(!p.active){slot=&p;break;}}
 if(ordered&&controls>=ordered_capacity){++s.view.rejected;return false;}if(ordered&&!slot)for(unsigned i=0;i<128;++i)if(!s.pending[i].active){slot=&s.pending[i];break;}if(!slot){++s.view.rejected;return false;}
 Header h;h.kind=Kind::frame;h.family=family;h.lane=lane;h.flags=ordered?Flag::ordered:Flag::replaceable;h.producer=s.self;h.frequency=s.view.local_frequency;h.connection=s.connection;h.sequence=ordered?++s.control_sequence[domain]:++s.wire_sequence;h.control_floor=s.control_sequence[domain];h.observed_qpc=native_qpc();h.source_service=service.local;h.target_service=service.remote;h.key=key;
 if(!encode(h,bytes,slot->wire))return false;slot->family=family;slot->lane=lane;slot->key=key;slot->sequence=h.sequence;slot->floor=h.control_floor;slot->control=ordered;slot->queued=std::int64_t(h.observed_qpc);slot->sent=slot->completed=0;slot->active=true;slot->object=nullptr;if(changed_observation_epoch){s.observation_epoch=observation_epoch;s.observation_baseline={};}if(initial_observation>=0)s.observation_baseline[initial_observation]=true;if(appearance_group>=0){s.appearance_baseline[appearance_group]=appearance;s.have_appearance_baseline[appearance_group]=true;}if(changed_readiness){s.readiness=readiness;s.have_readiness=true;}if(changed_audio){s.audio_state=audio_state;s.have_audio=true;}++s.view.queued;SetEvent(s.wake);return true;
}
bool Endpoint::receive(Family family,Received& out,LinkContext* context,std::uint64_t expected_connection,std::uint64_t expected_service){if(family>Family::actor)return false;auto& s=*impl_;std::lock_guard lock(s.mutex);if((expected_connection&&s.connection!=expected_connection)||(expected_service&&s.services.at(family).generation!=expected_service))return false;auto& q=s.inbox[unsigned(family)];if(!q.size)return false;if(context)*context={s.view,s.clocks};out=q.values[q.head];q.head=(q.head+1)%q.values.size();--q.size;bool any=false;for(const auto& i:s.inbox)any|=i.size!=0;if(!any)ResetEvent(s.ready);return true;}
Endpoint::Binding Endpoint::open_service(Family family){if(family==Family::link||family>Family::actor)return {};auto& s=*impl_;std::lock_guard lock(s.mutex);if(!s.view.connected)return {};const auto generation=s.services.open(family);s.service_view();SetEvent(s.wake);return {s.connection,generation};}
void Endpoint::close_service(Family family,Binding binding)noexcept{if(family==Family::link||family>Family::actor)return;auto& s=*impl_;std::lock_guard lock(s.mutex);if(binding.connection&&binding.connection==s.connection){s.services.close(family,binding.generation);s.service_view();SetEvent(s.wake);}}
bool Endpoint::evidence(Received& out){auto& s=*impl_;std::lock_guard lock(s.mutex);if(!s.trace_size)return false;out=s.traces[s.trace_head];s.trace_head=(s.trace_head+1)%s.traces.size();--s.trace_size;return true;}
void Endpoint::fixture_loss(double percent)noexcept{if(!std::isfinite(percent)||percent<0||percent>100)return;impl_->requested_loss.store(percent,std::memory_order_release);SetEvent(impl_->wake);}
void Endpoint::fixture_command_stall(bool on)noexcept{impl_->command_stall.store(on,std::memory_order_release);SetEvent(impl_->wake);}
void Endpoint::fixture_state_stall(bool on)noexcept{impl_->state_stall.store(on,std::memory_order_release);SetEvent(impl_->wake);}
void Endpoint::fixture_completion_stall(bool on)noexcept{impl_->completion_stall.store(on,std::memory_order_release);SetEvent(impl_->wake);}
void Endpoint::fixture_fixed_state_pacing(bool on)noexcept{impl_->fixed_state_pacing.store(on,std::memory_order_release);SetEvent(impl_->wake);}
void Endpoint::capture(bool on)noexcept{std::lock_guard lock(impl_->mutex);if(on&&!impl_->capturing)impl_->trace_head=impl_->trace_size=0;impl_->capturing=on;}
LinkStatus Endpoint::status()const{std::lock_guard lock(impl_->mutex);return impl_->view;}
LinkContext Endpoint::context()const{std::lock_guard lock(impl_->mutex);return {impl_->view,impl_->clocks};}
Projection Endpoint::remote_time(std::int64_t t)const{std::lock_guard lock(impl_->mutex);return impl_->clocks.to_remote(t,native_qpc());}
Projection Endpoint::local_time(std::int64_t t)const{std::lock_guard lock(impl_->mutex);return impl_->clocks.to_local(t,native_qpc());}
ClockMap Endpoint::clock_map()const{std::lock_guard lock(impl_->mutex);return impl_->clocks;}
bool Endpoint::connected_fast()const noexcept{return impl_->connected_fast.load(std::memory_order_acquire);}
std::uintptr_t Endpoint::ready_event()const noexcept{return reinterpret_cast<std::uintptr_t>(impl_->ready);}
}
