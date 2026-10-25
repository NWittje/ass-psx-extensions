#include <psx/display/tcp.hpp>
#include <psx/display/dpf_protocol.hpp>
#include <mswsock.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstring>
namespace psx::du::transport::tcp {
namespace {
template<class T>T get(std::span<const uint8_t>b,size_t p){if(p>b.size()||sizeof(T)>b.size()-p)throw std::runtime_error("DUT1 client bounds");T v;std::memcpy(&v,b.data()+p,sizeof v);return v;}
template<class T,class B>void put(B&b,size_t p,T v){std::memcpy(b.data()+p,&v,sizeof v);}
struct Io {
 SOCKET socket{INVALID_SOCKET};HANDLE port{};HANDLE registered{};OVERLAPPED ov{};bool pending{},established{};psx::norm::NativeMonitor*monitor{};unsigned unit{};DWORD last_bytes{},last_error{};int send_buffer{},receive_buffer{};uint64_t handshake_deadline{};
 static void CALLBACK wake(void*p,BOOLEAN){PostQueuedCompletionStatus(p,0,1,nullptr);}
 Io(HANDLE control,psx::norm::NativeMonitor*m,unsigned u):monitor(m),unit(u){WSADATA d{};if(WSAStartup(MAKEWORD(2,2),&d))throw std::runtime_error("TCP client Winsock");port=CreateIoCompletionPort(INVALID_HANDLE_VALUE,nullptr,0,1);if(!port){WSACleanup();throw std::runtime_error("TCP client completion port");}if(!RegisterWaitForSingleObject(&registered,control,wake,port,INFINITE,WT_EXECUTEDEFAULT)){CloseHandle(port);WSACleanup();throw std::runtime_error("TCP control wake registration");}}
 void mark(psx::norm::NativeCall c){if(monitor){monitor->thread=GetCurrentThreadId();monitor->unit=unit;monitor->entered=qpc();monitor->call=uint32_t(c);}}
 void leave(){if(monitor){monitor->completed.fetch_add(1);monitor->call=0;}}
 ~Io(){if(registered)UnregisterWaitEx(registered,INVALID_HANDLE_VALUE);if(socket!=INVALID_SOCKET){mark(psx::norm::NativeCall::tcp_cancel);CancelIoEx(reinterpret_cast<HANDLE>(socket),&ov);linger reset{1,0};setsockopt(socket,SOL_SOCKET,SO_LINGER,reinterpret_cast<char*>(&reset),sizeof reset);closesocket(socket);socket=INVALID_SOCKET;leave();}
  // Completion, not the cancellation request, retires the borrowed buffer.
  while(pending){DWORD n{};ULONG_PTR key{};OVERLAPPED*p{};mark(psx::norm::NativeCall::tcp_completion);BOOL ok=GetQueuedCompletionStatus(port,&n,&key,&p,1000);auto e=ok?0:GetLastError();leave();if(p==&ov){pending=false;last_bytes=n;last_error=e;}}
  CloseHandle(port);WSACleanup();}
 void cancel_pending(){if(socket!=INVALID_SOCKET){mark(psx::norm::NativeCall::tcp_cancel);CancelIoEx(reinterpret_cast<HANDLE>(socket),&ov);closesocket(socket);socket=INVALID_SOCKET;leave();}while(pending){DWORD n{};ULONG_PTR key{};OVERLAPPED*p{};BOOL ok=GetQueuedCompletionStatus(port,&n,&key,&p,1000);if(p==&ov){pending=false;last_bytes=n;last_error=ok?0:GetLastError();}}}
 void await(const ClientCallbacks&c,uint64_t deadline){for(;;){bool stopped=c.stop();auto now=qpc();if(stopped||now>=deadline){cancel_pending();throw std::runtime_error(stopped?"TCP reception stopped":"TCP connect/handshake deadline");}DWORD n{};ULONG_PTR key{};OVERLAPPED*p{};mark(psx::norm::NativeCall::tcp_completion);auto ms=std::min<uint64_t>(1000,std::max<uint64_t>(1,(deadline-now)/std::max<uint64_t>(1,frequency()/1000)));BOOL ok=GetQueuedCompletionStatus(port,&n,&key,&p,DWORD(ms));auto error=ok?0:GetLastError();leave();if(p==&ov){pending=false;last_bytes=n;last_error=error;if(error)throw std::runtime_error("TCP overlapped I/O error "+std::to_string(error));return;}}}
 void connect(const ClientOptions&o,const ClientCallbacks&c){socket=WSASocketW(AF_INET,SOCK_STREAM,IPPROTO_TCP,nullptr,0,WSA_FLAG_OVERLAPPED);if(socket==INVALID_SOCKET)throw std::runtime_error("TCP client socket");auto local=psx::network::Endpoint::parse(o.local,0);if(bind(socket,reinterpret_cast<sockaddr*>(&local.address),local.length))throw std::runtime_error("TCP selected adapter bind");if(!CreateIoCompletionPort(reinterpret_cast<HANDLE>(socket),port,0,0))throw std::runtime_error("TCP client completion association");GUID guid=WSAID_CONNECTEX;LPFN_CONNECTEX fn{};DWORD n{};if(WSAIoctl(socket,SIO_GET_EXTENSION_FUNCTION_POINTER,&guid,sizeof guid,&fn,sizeof fn,&n,nullptr,nullptr))throw std::runtime_error("TCP ConnectEx binding");auto remote=psx::network::Endpoint::parse(o.host,o.port);mark(psx::norm::NativeCall::tcp_connect);BOOL ok=fn(socket,reinterpret_cast<sockaddr*>(&remote.address),remote.length,nullptr,0,nullptr,&ov);int error=ok?0:WSAGetLastError();leave();if(!ok&&error!=ERROR_IO_PENDING)throw std::runtime_error("TCP connect failed");pending=true;handshake_deadline=qpc()+5*frequency();await(c,handshake_deadline);if(setsockopt(socket,SOL_SOCKET,SO_UPDATE_CONNECT_CONTEXT,nullptr,0))throw std::runtime_error("TCP connect context");sockaddr_storage peer{};int length=sizeof peer;if(getpeername(socket,reinterpret_cast<sockaddr*>(&peer),&length)||peer.ss_family!=AF_INET||reinterpret_cast<sockaddr_in*>(&peer)->sin_addr.s_addr!=reinterpret_cast<sockaddr_in*>(&remote.address)->sin_addr.s_addr)throw std::runtime_error("TCP server peer mismatch");BOOL one=TRUE;int bytes=65536;if(setsockopt(socket,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<char*>(&one),sizeof one)||setsockopt(socket,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<char*>(&bytes),sizeof bytes)||setsockopt(socket,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&bytes),sizeof bytes))throw std::runtime_error("TCP client socket options");int len=sizeof(int);getsockopt(socket,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<char*>(&send_buffer),&len);len=sizeof(int);getsockopt(socket,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&receive_buffer),&len);
  if(c.endpoints){psx::network::Endpoint actual;actual.length=sizeof(actual.address);
   if(!getsockname(socket,reinterpret_cast<sockaddr*>(&actual.address),&actual.length))
    try{c.endpoints(actual.host(),o.host);}catch(...){/* Endpoint diagnostics cannot disable reception. */}
  }}
 void transfer(std::span<uint8_t>b,bool write,const ClientCallbacks&c,uint64_t deadline,uint64_t*first=nullptr){size_t at{};while(at<b.size()){if(c.stop())throw std::runtime_error("TCP reception stopped");ov={};WSABUF w{ULONG(std::min<size_t>(65536,b.size()-at)),reinterpret_cast<char*>(b.data()+at)};DWORD flags{};mark(write?psx::norm::NativeCall::tcp_send:psx::norm::NativeCall::tcp_recv);int rc=write?WSASend(socket,&w,1,nullptr,0,&ov,nullptr):WSARecv(socket,&w,1,nullptr,&flags,&ov,nullptr);int error=rc?WSAGetLastError():0;leave();if(rc&&error!=WSA_IO_PENDING)throw std::runtime_error("TCP I/O initiation "+std::to_string(error));pending=true;
  // The host sends status at one-second intervals even when PSX produces no
  // pixels. Bound missing socket progress, independently of pixel freshness.
  // Each completed partial I/O gets a fresh interval; a large/slow frame is
  // allowed to continue while bytes advance. Time in client callbacks is not
  // charged to the peer. await() drains cancellation before Borrow can release.
  const auto progress_deadline=established?std::min(deadline,qpc()+5*frequency()):deadline;
  try{await(c,progress_deadline);}catch(const std::exception&){if(established&&!c.stop()&&qpc()>=progress_deadline)throw std::runtime_error("TCP host protocol progress timeout (5 seconds)");throw;}
  if(!last_bytes||last_bytes>w.len)throw std::runtime_error("TCP peer closed/invalid completion");if(first&&!*first)*first=qpc();at+=last_bytes;}}
};
}
struct VersionUnavailable : std::runtime_error { using std::runtime_error::runtime_error; };
static void receive_once(const ClientOptions&o,HANDLE control,const ClientCallbacks&c,Trace*trace,psx::norm::NativeMonitor*monitor){
 if(o.unit>=6||!o.port||(o.protocol!=1&&o.protocol!=2)||(o.instrument&&(o.protocol!=2||(o.unit!=4&&o.unit!=5)||!c.instrument_acquire||!c.instrument_complete||!c.instrument_abort||!c.instrument_status))||!c.acquire||!c.complete||!c.abort||!c.status||!c.stop)throw std::runtime_error("TCP client callbacks/configuration");
 // Declared before Io: incomplete raw storage is released AFTER cancellation
 // completions retire, including exceptions from a partial frame transfer.
 struct Borrow {const ClientCallbacks&c;bool active{},instrument{};~Borrow(){if(active)c.abort();if(instrument&&c.instrument_abort)c.instrument_abort();}}borrow{c};Io io(control,monitor,o.unit);io.connect(o,c);
 std::array<uint8_t,16> subscribe{};put(subscribe,0,uint32_t(o.unit));put(subscribe,4,uint32_t(1));put(subscribe,8,uint32_t(2));put(subscribe,12,uint32_t(o.instrument));auto sh=encode_header({Type::subscribe,16,{},o.protocol});auto deadline=io.handshake_deadline;io.transfer(sh,true,c,deadline);io.transfer(subscribe,true,c,deadline);
 Identity connection{},session{};bool accepted=false;Admission admission;unsigned ordinal{};uint32_t source_pid{};uint64_t source_created{},instrument_generation{},instrument_sequence{};std::string instrument_identity;std::array<uint8_t,32>source_hash{};
 for(;;){uint64_t first{};std::array<uint8_t,32>raw{};uint64_t until=accepted?UINT64_MAX:io.handshake_deadline;try{io.transfer(raw,false,c,until,&first);}catch(const std::exception&){if(!accepted&&o.protocol==2)throw VersionUnavailable("Host did not accept DUT version 2");throw;}auto h=decode_header(raw);
  if(h.type==Type::error){std::array<uint8_t,4096>b{};io.transfer({b.data(),h.bytes},false,c,until);throw std::runtime_error(std::string(reinterpret_cast<char*>(b.data()),h.bytes));}
  if(h.protocol!=o.protocol)throw std::runtime_error("TCP protocol changed within connection");
  if(!accepted){if(h.type!=Type::accept||h.bytes!=128||h.connection==Identity{})throw std::runtime_error("TCP Accept framing");std::array<uint8_t,128>b{};io.transfer(b,false,c,until);if(get<uint32_t>(b,16)!=o.unit||get<uint32_t>(b,20)<250||get<uint32_t>(b,20)>10000||get<uint32_t>(b,24)!=frame_limit||get<uint32_t>(b,28)!=1||get<uint32_t>(b,32)!=2)throw std::runtime_error("TCP Accept capability");if(o.protocol==2&&(get<uint32_t>(b,92)!=1||get<uint32_t>(b,96)!=psx::instrument::captain_stab_trim||get<uint32_t>(b,100)!=psx::instrument::frame_limit))throw std::runtime_error("TCP instrument capability");for(size_t p=o.protocol==2?104:92;p<b.size();++p)if(b[p])throw std::runtime_error("TCP Accept reserved");std::copy_n(b.begin(),16,session.begin());if(session==Identity{})throw std::runtime_error("TCP host session");connection=h.connection;ordinal=get<uint32_t>(connection,0);source_pid=get<uint32_t>(b,36);source_created=get<uint64_t>(b,40);std::copy_n(b.begin()+56,32,source_hash.begin());if(c.connected)c.connected(connection,io.send_buffer,io.receive_buffer);if(c.instrument_capability)c.instrument_capability(o.protocol==2);if(trace)for(unsigned word=0;word<2;++word)trace->event(0,{{session},qpc(),get<uint64_t>(connection,8*word),0,GetCurrentThreadId(),word,uint16_t(o.unit),uint16_t(psx::norm::Stage::connection_identity),ordinal});io.established=true;accepted=true;continue;}
  if(h.type==Type::instrument||h.type==Type::instrument_status){
   if(o.protocol!=2||!o.instrument||h.connection!=connection||h.bytes<128)throw std::runtime_error("TCP unsolicited instrument");
   std::array<uint8_t,4096>env{};io.transfer({env.data(),128},false,c,UINT64_MAX);auto size=get<uint32_t>(env,8),payload=get<uint32_t>(env,12);
   if(size<128||size>envelope_limit||payload>psx::instrument::frame_limit||size_t(size)+payload!=h.bytes||(h.type==Type::instrument_status&&payload)||(h.type==Type::instrument&&!payload))throw std::runtime_error("TCP instrument lengths");
   io.transfer({env.data()+128,size-128},false,c,UINT64_MAX);std::span<uint8_t>body;
   if(payload){body=c.instrument_acquire(payload);borrow.instrument=true;if(body.size()!=payload)throw std::runtime_error("TCP instrument raw budget");io.transfer(body,false,c,UINT64_MAX);}
   auto completed=qpc();auto e=decode_instrument_parts({env.data(),size},body);
   const psx::norm::Correlation instrument_token{e.session,e.generation,e.sequence,payload?psx::display::word(body.data(),16):0};
   const auto instrument_end=payload?psx::display::word(body.data(),48):0;
   if(trace&&payload){trace->event(0,{instrument_token,first,h.bytes,instrument_end,GetCurrentThreadId(),0,0x8001,uint16_t(psx::norm::Stage::socket_first_byte),ordinal});trace->event(0,{instrument_token,completed,h.bytes,instrument_end,GetCurrentThreadId(),0,0x8001,uint16_t(psx::norm::Stage::receive_complete),ordinal});trace->event(0,{instrument_token,completed,h.bytes,instrument_end,GetCurrentThreadId(),0,0x8001,uint16_t(psx::norm::Stage::validation_enter),ordinal});}
   if(e.session!=session||e.instrument!=psx::instrument::captain_stab_trim||e.generation<instrument_generation||(e.generation==instrument_generation&&e.sequence<=instrument_sequence))throw std::runtime_error("TCP instrument admission");
   if(payload&&(e.source.pid!=source_pid||e.source.created!=source_created))throw std::runtime_error("TCP instrument is from another PSX source");
   if(payload&&e.generation==instrument_generation&&!instrument_identity.empty()&&e.source.identity!=instrument_identity)throw std::runtime_error("TCP instrument identity changed");
   instrument_generation=e.generation;instrument_sequence=e.sequence;if(payload)instrument_identity=e.source.identity;
   if(payload){if(trace)trace->event(0,{instrument_token,qpc(),h.bytes,instrument_end,GetCurrentThreadId(),0,0x8001,uint16_t(psx::norm::Stage::validated),ordinal});auto disposition=c.instrument_complete(e,first,completed);borrow.instrument=false;if(trace)trace->event(0,{instrument_token,qpc(),uint64_t(disposition),instrument_end,GetCurrentThreadId(),0,0x8001,uint16_t(psx::norm::Stage::raw_commit),ordinal});Ack ack{{e.session,e.generation,e.sequence,psx::display::word(body.data(),16)},e.instrument,disposition,qpc(),frequency()};auto bytes=encode_ack(ack);auto header=encode_header({Type::instrument_ack,64,connection,2});io.transfer(header,true,c,UINT64_MAX);io.transfer(bytes,true,c,UINT64_MAX);}else c.instrument_status(e);
   continue;
  }
  if(h.connection!=connection||(h.type!=Type::data&&h.type!=Type::status))throw std::runtime_error("TCP connection/message mismatch");if(h.bytes<128)throw std::runtime_error("TCP DUN header length");
  std::array<uint8_t,4096>envelope{};io.transfer({envelope.data(),128},false,c,UINT64_MAX);auto env=get<uint32_t>(envelope,8),payload=get<uint32_t>(envelope,12);if(env<128||env>envelope_limit||payload>frame_limit||size_t(env)+payload!=h.bytes||(h.type==Type::status&&payload)||(h.type==Type::data&&!payload))throw std::runtime_error("TCP DUN payload bound");io.transfer({envelope.data()+128,env-128},false,c,UINT64_MAX);
  std::span<uint8_t>body;if(payload){body=c.acquire(payload);borrow.active=true;if(body.size()!=payload)throw std::runtime_error("TCP raw slot budget");io.transfer(body,false,c,UINT64_MAX);}auto completed=qpc();auto e=decode_parts({envelope.data(),env},body);if(e.session!=session||e.unit!=o.unit||!admission.accept(e))throw std::runtime_error("TCP source admission");auto token=psx::norm::Correlation{e.session,e.generation,e.sequence,payload?psx::display::word(body.data(),16):0};
  if(source_pid){std::array<uint8_t,32>hash{};if(e.source.pid!=source_pid||e.source.created!=source_created||BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,reinterpret_cast<PUCHAR>(e.source.identity.data()),ULONG(e.source.identity.size()),hash.data(),ULONG(hash.size()))<0||hash!=source_hash)throw std::runtime_error("TCP advertised source changed");}
  else if(payload)throw std::runtime_error("TCP source requires a fresh handshake");
  if(trace){auto end=payload?psx::display::word(body.data(),48):0;trace->event(0,{token,first,h.bytes,end,GetCurrentThreadId(),0,uint16_t(o.unit),uint16_t(psx::norm::Stage::socket_first_byte),ordinal});trace->event(0,{token,completed,h.bytes,end,GetCurrentThreadId(),0,uint16_t(o.unit),uint16_t(psx::norm::Stage::receive_complete),ordinal});trace->event(0,{token,completed,h.bytes,end,GetCurrentThreadId(),0,uint16_t(o.unit),uint16_t(psx::norm::Stage::validation_enter),ordinal});}
  if(payload){auto disposition=c.complete(e,first,completed);borrow.active=false;Ack ack{token,o.unit,disposition,qpc(),frequency()};auto ab=encode_ack(ack);auto ah=encode_header({Type::ack,64,connection,o.protocol});io.transfer(ah,true,c,UINT64_MAX);io.transfer(ab,true,c,UINT64_MAX);}else c.status(e);
 }
}
void receive(const ClientOptions&o,HANDLE control,const ClientCallbacks&c,Trace*trace,psx::norm::NativeMonitor*monitor)
{
 try { receive_once(o,control,c,trace,monitor); }
 catch(const VersionUnavailable&) {
  if(o.protocol!=2||c.stop())throw;
  auto legacy=o;legacy.protocol=1;legacy.instrument=false;
  if(c.instrument_capability)c.instrument_capability(false);
  receive_once(legacy,control,c,trace,monitor);
 }
}

}
