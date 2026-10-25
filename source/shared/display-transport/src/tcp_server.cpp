#include <psx/display/tcp.hpp>
#include <mswsock.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstring>
namespace psx::du::transport::tcp {
namespace {
struct Wsa {Wsa(){WSADATA d{};if(WSAStartup(MAKEWORD(2,2),&d))throw std::runtime_error("Winsock startup");}~Wsa(){WSACleanup();}};
std::atomic<uint32_t> connection_ordinal{};
void socket_options(SOCKET s,int&tx,int&rx){BOOL one=TRUE;int n=65536;if(setsockopt(s,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<char*>(&one),sizeof one)||setsockopt(s,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<char*>(&n),sizeof n)||setsockopt(s,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&n),sizeof n))throw std::runtime_error("TCP socket options");int len=sizeof n;getsockopt(s,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<char*>(&tx),&len);len=sizeof n;getsockopt(s,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&rx),&len);}
template<class T,class B>void put(B&b,size_t at,T v){std::memcpy(b.data()+at,&v,sizeof v);}
template<class T>T get(std::span<const uint8_t>b,size_t at){if(at>b.size()||sizeof(T)>b.size()-at)throw std::runtime_error("TCP control bounds");T v;std::memcpy(&v,b.data()+at,sizeof v);return v;}
}
struct Server::Impl {
 struct Peer;
 struct Op {OVERLAPPED ov{};unsigned kind{};Peer*peer{};bool pending{};};
 struct InstrumentLane {Frame current;std::optional<Ack> early_ack;psx::norm::Correlation acknowledged;uint64_t began{},last_publication{},last_status{},last_ack{};bool waiting_ack{};};
 struct Peer {
  SOCKET socket{INVALID_SOCKET};Identity id{};Op rx{},tx{};unsigned outstanding{},unit{},ordinal{};bool closing{},counted{},subscribed{},reject{},waiting_ack{};
  uint64_t born{},began{},last_publication{},last_status{},last_ack{},last_generation{};Frame current,sending;InstrumentLane instrument;bool wants_instrument{},instrument_counted{},sending_instrument{},last_instrument{};uint16_t protocol{1};
  std::optional<Ack> early_ack;psx::norm::Correlation acknowledged,pending_token;uint64_t pending_end{};Origin advertised;uint32_t peer_address{};
  std::array<uint8_t,32> send_header{},read_header{};std::array<uint8_t,4096> control{},read_control{};
  size_t control_bytes{},send_at{},issued{},read_at{},read_need{32};bool read_body{};Header received;
  Peer(){rx.kind=2;rx.peer=this;tx.kind=3;tx.peer=this;}
  ~Peer(){if(socket!=INVALID_SOCKET)closesocket(socket);}
 };
 Wsa wsa;HANDLE port{CreateIoCompletionPort(INVALID_HANDLE_VALUE,nullptr,0,1)};SOCKET listener{INVALID_SOCKET},incoming{INVALID_SOCKET};
 LPFN_ACCEPTEX accept_ex{};Op accept;std::array<uint8_t,2*(sizeof(sockaddr_storage)+16)>addresses{};
 std::array<std::unique_ptr<Peer>,9>peers;std::atomic<unsigned>&aggregate;unsigned binding;ServerOptions options;std::function<Selection(unsigned)>select;std::function<void(int)>interest;
 Trace*trace{};unsigned producer{},round_robin{};Snapshot stats;psx::norm::NativeMonitor monitor;bool active{};
 psx::norm::DiagnosticSink event_sink{};void*event_context{};
 Impl(unsigned b,std::atomic<unsigned>&a):aggregate(a),binding(b){if(!port)throw std::runtime_error("TCP completion port");accept.kind=1;}
 ~Impl(){stop();CloseHandle(port);}
 void mark(psx::norm::NativeCall call,unsigned unit=0){monitor.thread=GetCurrentThreadId();monitor.unit=unit;monitor.entered=qpc();monitor.call=uint32_t(call);}
 void leave(){monitor.completed.fetch_add(1);monitor.call=0;}
 void event(psx::norm::Stage stage,const Peer&p,uint64_t value=0){auto f=p.sending?p.sending:p.current;psx::norm::DiagnosticRecord r{f?f->token:psx::norm::Correlation{options.session},qpc(),value,f?f->end:0,GetCurrentThreadId(),binding,uint16_t(f&&f->instrument?0x8001:p.unit),uint16_t(stage),p.ordinal};if(trace)trace->event(producer,r);if(event_sink)event_sink(event_context,r);}
 void close(Peer&p){if(p.closing)return;if(p.instrument_counted){p.instrument_counted=false;if(interest)interest(-1);}p.closing=true;p.waiting_ack=false;
  if(p.socket!=INVALID_SOCKET){mark(psx::norm::NativeCall::tcp_cancel,p.unit);CancelIoEx(reinterpret_cast<HANDLE>(p.socket),nullptr);leave();linger reset{1,0};setsockopt(p.socket,SOL_SOCKET,SO_LINGER,reinterpret_cast<char*>(&reset),sizeof reset);mark(psx::norm::NativeCall::tcp_close,p.unit);closesocket(p.socket);leave();p.socket=INVALID_SOCKET;}
 }
 void sweep(){for(auto&p:peers)if(p&&p->closing&&!p->outstanding){if(p->current){++stats.released;p->current.reset();}if(p->counted)aggregate.fetch_sub(1);p.reset();}}
 void issue_accept(){if(!active||accept.pending)return;incoming=WSASocketW(AF_INET,SOCK_STREAM,IPPROTO_TCP,nullptr,0,WSA_FLAG_OVERLAPPED);if(incoming==INVALID_SOCKET)throw std::runtime_error("TCP accept socket");accept.ov={};DWORD bytes{};mark(psx::norm::NativeCall::tcp_accept);BOOL ok=accept_ex(listener,incoming,addresses.data(),0,sizeof(sockaddr_storage)+16,sizeof(sockaddr_storage)+16,&bytes,&accept.ov);auto error=ok?0:WSAGetLastError();leave();if(!ok&&error!=ERROR_IO_PENDING){closesocket(incoming);incoming=INVALID_SOCKET;throw std::runtime_error("TCP AcceptEx");}accept.pending=true;}
 void read(Peer&p){if(p.closing||p.rx.pending)return;p.rx.ov={};auto*base=p.read_body?p.read_control.data():p.read_header.data();WSABUF b{ULONG(p.read_need-p.read_at),reinterpret_cast<char*>(base+p.read_at)};DWORD flags{};mark(psx::norm::NativeCall::tcp_recv,p.unit);auto rc=WSARecv(p.socket,&b,1,nullptr,&flags,&p.rx.ov,nullptr);auto err=rc?WSAGetLastError():0;leave();if(rc&&err!=WSA_IO_PENDING){stats.socket_error=err;close(p);return;}p.rx.pending=true;++p.outstanding;}
 void send(Peer&p){if(p.closing||p.tx.pending)return;
  auto envelope=p.sending?std::span<const uint8_t>(*p.sending->envelope):std::span<const uint8_t>(p.control.data(),p.control_bytes);
  auto payload=p.sending&&p.sending->payload?std::span<const uint8_t>(*p.sending->payload):std::span<const uint8_t>{};
  std::array<std::span<const uint8_t>,3>parts{p.send_header,envelope,payload};std::array<WSABUF,3>buffers{};unsigned count{};size_t skip=p.send_at,left=65536;
  for(auto bytes:parts){if(skip>=bytes.size()){skip-=bytes.size();continue;}auto n=std::min(left,bytes.size()-skip);if(n){buffers[count++]={ULONG(n),reinterpret_cast<char*>(const_cast<uint8_t*>(bytes.data()+skip))};left-=n;}skip=0;if(!left)break;}
  if(!count)return;p.issued=65536-left;p.tx.ov={};event(psx::norm::Stage::socket_send_enter,p,p.issued);mark(psx::norm::NativeCall::tcp_send,p.unit);auto rc=WSASend(p.socket,buffers.data(),count,nullptr,0,&p.tx.ov,nullptr);auto error=rc?WSAGetLastError():0;leave();event(psx::norm::Stage::socket_send_return,p,error);if(rc&&error!=WSA_IO_PENDING){stats.socket_error=error;close(p);return;}p.tx.pending=true;++p.outstanding;
 }
 void control(Peer&p,Type type,std::span<const uint8_t>b){if(p.tx.pending||p.sending)throw std::runtime_error("TCP ordered control ownership");if(b.size()>p.control.size())throw std::runtime_error("TCP control limit");std::copy(b.begin(),b.end(),p.control.begin());p.control_bytes=b.size();p.sending.reset();p.sending_instrument=false;p.send_header=encode_header({type,uint32_t(b.size()),p.id,p.protocol});p.send_at=0;send(p);}
 void accepted(){SOCKET s=incoming;incoming=INVALID_SOCKET;auto slot=std::find_if(peers.begin(),peers.end(),[](auto&p){return !p;});if(slot==peers.end()){closesocket(s);++stats.rejected;return;}
  auto p=std::make_unique<Peer>();p->socket=s;p->id=identity();p->ordinal=connection_ordinal.fetch_add(1)+1;if(!p->ordinal)throw std::runtime_error("TCP connection ordinal exhausted");put(p->id,0,p->ordinal);p->born=qpc();if(setsockopt(s,SOL_SOCKET,SO_UPDATE_ACCEPT_CONTEXT,reinterpret_cast<char*>(&listener),sizeof listener))throw std::runtime_error("TCP accept context");
  sockaddr_storage actual{};int actual_size=sizeof actual;if(getpeername(s,reinterpret_cast<sockaddr*>(&actual),&actual_size)||actual.ss_family!=AF_INET)throw std::runtime_error("TCP subscriber peer address");p->peer_address=reinterpret_cast<sockaddr_in*>(&actual)->sin_addr.s_addr;if(!p->peer_address)throw std::runtime_error("TCP unspecified subscriber address");
  socket_options(s,stats.send_buffer,stats.receive_buffer);if(!CreateIoCompletionPort(reinterpret_cast<HANDLE>(s),port,0,0))throw std::runtime_error("TCP accepted completion association");
  auto count=aggregate.load();do{if(count>=subscriber_limit)break;}while(!aggregate.compare_exchange_weak(count,count+1));p->counted=count<subscriber_limit;
  *slot=std::move(p);auto&peer=**slot;if(trace)trace->event(producer,{{options.session},qpc(),ntohl(peer.peer_address),0,GetCurrentThreadId(),binding,uint16_t(peer.unit),uint16_t(psx::norm::Stage::connection_peer),peer.ordinal});
  if(!peer.counted){peer.reject=true;++stats.rejected;constexpr char busy[]="Source busy: eight DU subscribers already connected";control(peer,Type::error,{reinterpret_cast<const uint8_t*>(busy),sizeof busy-1});}
  else {stats.peak=std::max<uint64_t>(stats.peak,aggregate.load());read(peer);}
 }
 void ack_event(const Peer&p,const Ack&a,bool instrument){auto frame=instrument?p.instrument.current:p.current;psx::norm::DiagnosticRecord r{a.token,qpc(),uint32_t(a.disposition),frame?frame->end:0,GetCurrentThreadId(),binding,uint16_t(instrument?0x8001:p.unit),uint16_t(psx::norm::Stage::acknowledged),p.ordinal};if(trace)trace->event(producer,r);if(event_sink)event_sink(event_context,r);}
 void retire(Peer&p,const Ack&a){ack_event(p,a,false);p.last_ack=a.token.publication;p.acknowledged=a.token;p.waiting_ack=false;++stats.acknowledged;if(a.disposition==Disposition::budget)++stats.rejected;if(a.disposition==Disposition::replaced){++stats.replaced;++stats.mailbox_replaced;}++stats.released;p.current.reset();}
 void retire_instrument(Peer&p,const Ack&a){ack_event(p,a,true);auto&lane=p.instrument;lane.last_ack=a.token.publication;lane.acknowledged=a.token;lane.waiting_ack=false;lane.current.reset();++stats.instrument_acknowledged;if(a.disposition==Disposition::budget)++stats.instrument_rejected;}
 void instrument_ack(Peer&p,const Ack&a){auto&lane=p.instrument;if(!p.wants_instrument||a.unit!=psx::instrument::captain_stab_trim||a.token.session!=options.session)throw std::runtime_error("TCP instrument ACK identity");
  if(a.token.publication<=lane.last_ack){if(a.token.publication==lane.acknowledged.publication&&a.token.generation==lane.acknowledged.generation&&a.token.frame==lane.acknowledged.frame)return;throw std::runtime_error("TCP obsolete instrument ACK");}
  if(!lane.current||a.token.generation!=lane.current->token.generation||a.token.publication!=lane.current->token.publication||a.token.frame!=lane.current->token.frame)throw std::runtime_error("TCP unissued instrument ACK");
  if(!lane.waiting_ack){if(!p.sending_instrument||!p.tx.pending||p.send_at+p.issued!=32+lane.current->bytes()||lane.early_ack)throw std::runtime_error("TCP premature instrument ACK");lane.early_ack=a;return;}retire_instrument(p,a);
 }
 void message(Peer&p){auto&h=p.received;
  if(!p.subscribed){if(h.protocol>options.maximum_protocol)throw std::runtime_error("TCP unsupported protocol version");if(h.type!=Type::subscribe||h.bytes!=16||h.connection!=Identity{})throw std::runtime_error("TCP Subscribe framing");p.protocol=h.protocol;p.unit=get<uint32_t>(p.read_control,0);p.wants_instrument=p.protocol==2&&(get<uint32_t>(p.read_control,12)&1);if(p.unit>=6||(p.wants_instrument&&p.unit!=4&&p.unit!=5)||get<uint32_t>(p.read_control,4)!=1||get<uint32_t>(p.read_control,8)!=2||get<uint32_t>(p.read_control,12)>(p.protocol==2?1u:0u))throw std::runtime_error("TCP Subscribe capabilities");p.subscribed=true;if(p.wants_instrument){p.instrument_counted=true;if(interest)interest(1);}auto source=select(p.unit).origin;p.advertised=source;std::array<uint8_t,128>b{};std::copy(options.session.begin(),options.session.end(),b.begin());put(b,16,p.unit);put(b,20,options.freshness_ms);put(b,24,uint32_t(frame_limit));put(b,28,uint32_t(1));put(b,32,uint32_t(2));put(b,36,source.pid);put(b,40,source.created);put(b,48,source.clock);put(b,88,uint32_t(source.pid?State::waiting:State::disconnected));if(!source.identity.empty()&&BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,reinterpret_cast<PUCHAR>(source.identity.data()),ULONG(source.identity.size()),b.data()+56,32)<0)throw std::runtime_error("TCP source identity digest");for(unsigned word=0;word<2;++word){psx::norm::DiagnosticRecord r{{options.session},qpc(),get<uint64_t>(p.id,8*word),0,GetCurrentThreadId(),word,uint16_t(p.unit),uint16_t(psx::norm::Stage::connection_identity),p.ordinal};if(trace)trace->event(producer,r);}if(p.protocol==2){put(b,92,uint32_t(1));put(b,96,uint32_t(psx::instrument::captain_stab_trim));put(b,100,uint32_t(psx::instrument::frame_limit));}control(p,Type::accept,b);return;}
  if(h.connection!=p.id||h.protocol!=p.protocol)throw std::runtime_error("TCP connection/protocol identity");if(h.type==Type::instrument_ack){instrument_ack(p,decode_ack({p.read_control.data(),h.bytes}));return;}
  if(h.type!=Type::ack)throw std::runtime_error("TCP connection/control identity");auto a=decode_ack({p.read_control.data(),h.bytes});if(a.unit!=p.unit||a.token.session!=options.session)throw std::runtime_error("TCP ACK source");
  if(a.token.publication<=p.last_ack){if(a.token.publication==p.acknowledged.publication&&a.token.generation==p.acknowledged.generation&&a.token.frame==p.acknowledged.frame)return;throw std::runtime_error("TCP obsolete ACK identity");}
  if(!p.current||a.token.generation!=p.current->token.generation||a.token.publication!=p.current->token.publication||a.token.frame!=p.current->token.frame)throw std::runtime_error("TCP ACK unissued frame");
  // RX and TX completions can arrive in either order. A valid ACK never
  // retires scatter/gather storage while its final send is still outstanding.
  if(!p.waiting_ack){if(!p.tx.pending||p.send_at+p.issued!=32+p.current->bytes()||p.early_ack)throw std::runtime_error("TCP premature ACK");p.early_ack=a;return;}
  retire(p,a);
 }
 void completion(Op&op,DWORD bytes,DWORD error){op.pending=false;if(op.kind==1){if(error||!active){if(incoming!=INVALID_SOCKET)closesocket(incoming);incoming=INVALID_SOCKET;}else accepted();if(active)issue_accept();return;}auto&p=*op.peer;--p.outstanding;
  if(p.closing){++stats.cancelled;return;}if(error||!bytes){stats.socket_error=int(error);close(p);return;}
  if(op.kind==2){p.read_at+=bytes;if(p.read_at!=p.read_need){read(p);return;}
   if(!p.read_body){p.received=decode_header(p.read_header);if(p.received.type!=Type::subscribe&&p.received.type!=Type::ack&&p.received.type!=Type::instrument_ack)throw std::runtime_error("TCP client message type");if(!p.received.bytes||p.received.bytes>p.read_control.size())throw std::runtime_error("TCP client control length");p.read_body=true;p.read_at=0;p.read_need=p.received.bytes;read(p);}
   else {message(p);p.read_body=false;p.read_at=0;p.read_need=32;read(p);}
  }else{if(bytes>p.issued)throw std::runtime_error("TCP completion length");p.send_at+=bytes;stats.bytes+=bytes;if(bytes<p.issued){++stats.partial;event(psx::norm::Stage::socket_partial,p,bytes);}size_t total=32+(p.sending?p.sending->bytes():p.control_bytes);
   if(p.send_at<total)send(p);else{event(psx::norm::Stage::socket_complete,p,total);if(p.reject)close(p);else if(p.sending){if(p.sending_instrument){auto&lane=p.instrument;lane.waiting_ack=true;++stats.instrument_submitted;if(lane.early_ack){auto a=*lane.early_ack;lane.early_ack.reset();retire_instrument(p,a);}}else{p.waiting_ack=true;++stats.submitted;if(p.early_ack){auto a=*p.early_ack;p.early_ack.reset();retire(p,a);}}}p.sending.reset();p.control_bytes=0;}
  }
 }
 void tick(){auto now=qpc();sweep();auto first=round_robin++%peers.size();for(size_t offset=0;offset<peers.size();++offset){auto&p=peers[(first+offset)%peers.size()];if(!p||p->closing)continue;
  if(!p->subscribed){if(now-p->born>5*frequency())close(*p);continue;}auto latest=select(p->unit);
  if(latest.origin.pid!=p->advertised.pid||latest.origin.created!=p->advertised.created||latest.origin.identity!=p->advertised.identity){++stats.reconnects;close(*p);continue;}
  if(p->current){if(latest.frame&&latest.frame->token.generation!=p->current->token.generation){++stats.reconnects;close(*p);continue;}
   if(latest.frame&&latest.frame->token.publication>p->current->token.publication){if(latest.frame->token.publication!=p->pending_token.publication){if(p->pending_token.publication){++stats.pending_replaced;if(trace)trace->event(producer,{p->pending_token,now,now-p->began,p->pending_end,GetCurrentThreadId(),binding,uint16_t(p->unit),uint16_t(psx::norm::Stage::replaced),p->ordinal});}p->pending_token=latest.frame->token;p->pending_end=latest.frame->end;}if(now-p->began>=frequency()*options.freshness_ms/1000){++stats.reconnects;close(*p);continue;}}
  }
  auto&lane=p->instrument;
  if(lane.current&&latest.instrument_frame&&(latest.instrument_frame->token.generation!=lane.current->token.generation||(latest.instrument_frame->token.publication>lane.current->token.publication&&now-lane.began>=frequency()*options.freshness_ms/1000))){++stats.reconnects;close(*p);continue;}
  if(p->tx.pending)continue;
  bool base=latest.frame&&!p->current&&latest.frame->token.publication>p->last_publication;
  bool trim=p->wants_instrument&&p->advertised.pid&&latest.instrument_origin.pid==p->advertised.pid&&latest.instrument_origin.created==p->advertised.created&&latest.instrument_frame&&!lane.current&&latest.instrument_frame->token.publication>lane.last_publication;
  const bool instrument=trim&&(!base||!p->last_instrument);
  if(base||trim){auto f=instrument?latest.instrument_frame:latest.frame;auto&last=instrument?lane.last_publication:p->last_publication;last=f->token.publication;
   now=qpc();if(now>=f->end&&now-f->end>frequency()/30){if(instrument)++stats.instrument_expired;else ++stats.expired;continue;}
   if(instrument){lane.current=f;lane.began=now;}else{p->current=f;p->pending_token={};p->began=now;p->last_generation=f->token.generation;++stats.enqueued;}
   p->sending=std::move(f);p->sending_instrument=instrument;p->last_instrument=instrument;p->send_header=encode_header({instrument?Type::instrument:Type::data,uint32_t(p->sending->bytes()),p->id,p->protocol});p->send_at=0;send(*p);
  }else if(p->wants_instrument&&!lane.current&&latest.instrument_status&&latest.instrument_status->token.publication>lane.last_publication&&now-lane.last_status>=frequency()){
   auto&h=*latest.instrument_status->envelope;control(*p,Type::instrument_status,h);lane.last_status=now;lane.last_publication=latest.instrument_status->token.publication;
  }else if(!p->current&&latest.status&&latest.status->token.publication>p->last_publication&&now-p->last_status>=frequency()){auto&h=*latest.status->envelope;control(*p,Type::status,h);p->last_status=now;p->last_publication=latest.status->token.publication;}
 }stats.active=std::count_if(peers.begin(),peers.end(),[](const auto&p){return p&&p->counted&&!p->closing;});stats.capacity_used=aggregate.load();}
 void start(const ServerOptions&o,std::function<Selection(unsigned)>s,std::function<void(int)>i={}){stop();interest=std::move(i);if(o.address.empty()||!o.port||o.freshness_ms<250||o.freshness_ms>10000||(o.maximum_protocol!=1&&o.maximum_protocol!=2))throw std::runtime_error("TCP listener configuration");options=o;select=std::move(s);listener=WSASocketW(AF_INET,SOCK_STREAM,IPPROTO_TCP,nullptr,0,WSA_FLAG_OVERLAPPED);if(listener==INVALID_SOCKET)throw std::runtime_error("TCP listener socket");
  try{BOOL exclusive=TRUE;if(setsockopt(listener,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<char*>(&exclusive),sizeof exclusive))throw std::runtime_error("TCP listener exclusivity");auto e=psx::network::Endpoint::parse(o.address,o.port);if(e.address.ss_family!=AF_INET)throw std::runtime_error("TCP listener must be explicit IPv4");auto address=ntohl(reinterpret_cast<sockaddr_in*>(&e.address)->sin_addr.s_addr);if((!address&&!o.allow_wildcard)||address==0xffffffffu||(address>>28)==14)throw std::runtime_error("TCP listener cannot be wildcard/multicast/broadcast");if(options.session==Identity{})throw std::runtime_error("TCP host session is required");if(bind(listener,reinterpret_cast<sockaddr*>(&e.address),e.length)||listen(listener,8))throw std::runtime_error("TCP listener bind/listen");if(!CreateIoCompletionPort(reinterpret_cast<HANDLE>(listener),port,0,0))throw std::runtime_error("TCP listener completion association");GUID guid=WSAID_ACCEPTEX;DWORD bytes{};if(WSAIoctl(listener,SIO_GET_EXTENSION_FUNCTION_POINTER,&guid,sizeof guid,&accept_ex,sizeof accept_ex,&bytes,nullptr,nullptr))throw std::runtime_error("TCP AcceptEx binding");active=true;issue_accept();}catch(...){stop();throw;}
 }
 void pump(unsigned wait){tick();DWORD bytes{};ULONG_PTR key{};OVERLAPPED*ov{};mark(psx::norm::NativeCall::tcp_completion);BOOL ok=GetQueuedCompletionStatus(port,&bytes,&key,&ov,std::min(wait,100u));auto error=ok?0:GetLastError();leave();if(ov){auto&op=*reinterpret_cast<Op*>(ov);try{completion(op,bytes,error);}catch(...){if(op.peer){++stats.rejected;close(*op.peer);}else throw;}}for(unsigned i=0;i<127;++i){ov=nullptr;ok=GetQueuedCompletionStatus(port,&bytes,&key,&ov,0);error=ok?0:GetLastError();if(!ov)break;auto&op=*reinterpret_cast<Op*>(ov);try{completion(op,bytes,error);}catch(...){if(op.peer){++stats.rejected;close(*op.peer);}else throw;}}tick();}
 void stop(){active=false;if(listener!=INVALID_SOCKET){CancelIoEx(reinterpret_cast<HANDLE>(listener),nullptr);closesocket(listener);listener=INVALID_SOCKET;}if(incoming!=INVALID_SOCKET){closesocket(incoming);incoming=INVALID_SOCKET;}for(auto&p:peers)if(p)close(*p);
  for(;;){sweep();bool pending=accept.pending;for(auto&p:peers)if(p&&p->outstanding)pending=true;if(!pending)break;DWORD bytes{};ULONG_PTR key{};OVERLAPPED*ov{};mark(psx::norm::NativeCall::tcp_completion);BOOL ok=GetQueuedCompletionStatus(port,&bytes,&key,&ov,1000);auto error=ok?0:GetLastError();leave();if(ov)completion(*reinterpret_cast<Op*>(ov),bytes,error);}sweep();select={};
 }
};
Server::Server(unsigned b,std::atomic<unsigned>&a):impl_(std::make_unique<Impl>(b,a)){}Server::~Server()=default;
void Server::start(const ServerOptions&o,std::function<Selection(unsigned)>s){impl_->start(o,std::move(s));}
void Server::start(const ServerOptions&o,std::function<Selection(unsigned)>s,std::function<void(int)>i){impl_->start(o,std::move(s),std::move(i));}
void Server::observe(Trace*t,unsigned p){impl_->trace=t;impl_->producer=p;}void Server::notify()noexcept{PostQueuedCompletionStatus(impl_->port,0,1,nullptr);}
void Server::events(psx::norm::DiagnosticSink s,void*c){impl_->event_sink=s;impl_->event_context=c;}
void Server::pump(unsigned n){impl_->pump(n);}void Server::stop(){impl_->stop();}Snapshot Server::snapshot()const{auto s=impl_->stats;s.native=impl_->monitor.snapshot();return s;}
psx::norm::NativeSnapshot Server::native_snapshot()const noexcept{return impl_->monitor.snapshot();}
}
