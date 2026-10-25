#include "coordinator/readiness_evidence.hpp"
#include "evidence_file_worker.hpp"
#include <array>
#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
namespace coordinator {
namespace s=status_wire;
namespace {
constexpr std::size_t capacity=4096;constexpr std::uint64_t limit=128ull*1024*1024;
struct Bytes {
 std::array<std::uint8_t,capacity> data{};std::size_t size{},pos{};bool ok{true};
 void put(std::uint64_t v,unsigned n=8){if(size+n>data.size()){ok=false;return;}for(unsigned i=0;i<n;++i)data[size++]=static_cast<std::uint8_t>(v>>(8*i));}
 std::uint64_t get(unsigned n=8){if(pos+n>size){ok=false;return 0;}std::uint64_t v{};for(unsigned i=0;i<n;++i)v|=std::uint64_t(data[pos++])<<(8*i);return v;}
 void frame(const s::Frame& f){put(f.size,4);if(size+f.size>data.size()){ok=false;return;}std::copy_n(f.bytes.data(),f.size,data.data()+size);size+=f.size;}
 s::Frame frame(){s::Frame f;const auto n=get(4);if(n>s::max_frame||pos+n>size){ok=false;return f;}f.size=static_cast<std::size_t>(n);std::copy_n(data.data()+pos,f.size,f.bytes.data());pos+=f.size;return f;}
 void instance(psx::motion::InstanceId v){put(v.high);put(v.low);}
 psx::motion::InstanceId instance(){return {get(),get()};}
 void process(ProcessIdentity v){put(v.pid,4);put(v.creation_filetime);}
 ProcessIdentity process(){const auto p=get(4);return {static_cast<std::uint32_t>(p),get()};}
};
template<class T> bool decoded(const s::Frame& f,s::Kind kind,T& v){s::Header h;return f.size>=s::header_size&&s::read_header(f.bytes.data(),s::header_size,h)&&h.kind==kind&&f.size==s::header_size+h.payload_size&&s::decode(f.bytes.data()+s::header_size,h.payload_size,v);}
void detail(Bytes& b,const ReadinessDetail& d){b.put(static_cast<unsigned>(d.reason),4);b.put(d.communication_healthy,1);b.put(d.last_known_available,1);b.put(d.last_known_state,4);b.put(d.last_known_qpc);b.put(d.last_known_phase_qpc);b.instance(d.last_known_producer);b.put(d.revision);}
void detail(Bytes& b,ReadinessDetail& d){const auto reason=b.get(4),health=b.get(1),known=b.get(1),state=b.get(4);d.reason=static_cast<ReadinessReason>(reason);d.communication_healthy=health!=0;d.last_known_available=known!=0;d.last_known_state=static_cast<unsigned>(state);d.last_known_qpc=static_cast<std::int64_t>(b.get());d.last_known_phase_qpc=static_cast<std::int64_t>(b.get());d.last_known_producer=b.instance();d.revision=b.get();if(reason>static_cast<unsigned>(ReadinessReason::invalid)||health>1||known>1||state>3)b.ok=false;}
void view(Bytes& b,const ReadinessView& v){b.put(static_cast<unsigned>(v.xplane),4);b.put(static_cast<unsigned>(v.psx),4);b.put(v.revision);b.put(v.accepted);b.put(v.obsolete);b.put(v.faults);detail(b,v.plugin);detail(b,v.exporter);}
void view(Bytes& b,ReadinessView& v){const auto xp=b.get(4),psx=b.get(4);v.xplane=static_cast<XPlaneState>(xp);v.psx=static_cast<PsxState>(psx);v.revision=b.get();v.accepted=b.get();v.obsolete=b.get();v.faults=b.get();detail(b,v.plugin);detail(b,v.exporter);if(xp>3||psx>3)b.ok=false;}
bool equal(const ReadinessView& a,const ReadinessView& v){Bytes x,y;view(x,a);view(y,v);return x.ok&&y.ok&&x.size==y.size&&std::equal(x.data.begin(),x.data.begin()+x.size,y.data.begin());}
void checkpoint(Bytes& b,const ReadinessModel::Checkpoint& c){b.process(c.selected_plugin);view(b,c.view);for(const auto& p:c.peers){b.put(p.connected,1);b.put(p.have_snapshot,1);b.process(p.process);b.put(p.connection);b.put(p.wire_sequence);b.put(p.source_sequence);b.put(p.lifecycle);b.put(p.floor);b.put(p.heartbeat);b.put(p.capabilities,4);b.put(static_cast<unsigned>(p.reason),4);s::Frame h,v;if(p.connection&&!s::encode(p.hello,1,h))b.ok=false;if(p.source_sequence&&!s::encode(p.snapshot,1,v))b.ok=false;b.frame(h);b.frame(v);}}
void checkpoint(Bytes& b,ReadinessModel::Checkpoint& c){c.selected_plugin=b.process();view(b,c.view);for(auto& p:c.peers){const auto connected=b.get(1),available=b.get(1);p.connected=connected!=0;p.have_snapshot=available!=0;if(connected>1||available>1)b.ok=false;p.process=b.process();p.connection=b.get();p.wire_sequence=b.get();p.source_sequence=b.get();p.lifecycle=b.get();p.floor=static_cast<std::int64_t>(b.get());p.heartbeat=static_cast<std::int64_t>(b.get());p.capabilities=static_cast<unsigned>(b.get(4));const auto r=b.get(4);p.reason=static_cast<s::Reason>(r);if(r>9)b.ok=false;const auto h=b.frame(),v=b.frame();if(p.connection&&!decoded(h,s::Kind::hello,p.hello))b.ok=false;if(!p.connection&&h.size)b.ok=false;if(p.source_sequence&&!decoded(v,s::Kind::snapshot,p.snapshot))b.ok=false;if(!p.source_sequence&&v.size)b.ok=false;}}
void event(Bytes& b,const ReadinessEvent& e){b.put(static_cast<unsigned>(e.kind),4);b.put(static_cast<unsigned>(e.role),4);b.process(e.process);b.put(e.connection_id);b.put(e.received_qpc);b.put(e.subscribe_floor_qpc);b.put(e.error,4);s::Frame f;if(e.kind==ReadinessEventKind::hello){if(!s::encode(e.hello,1,f))b.ok=false;}else if(e.kind==ReadinessEventKind::frame)f=e.frame;b.frame(f);}
void event(Bytes& b,ReadinessEvent& e){const auto kind=b.get(4),role=b.get(4);if(kind>3||role<1||role>2)b.ok=false;e.kind=static_cast<ReadinessEventKind>(kind);e.role=static_cast<s::Role>(role);e.process=b.process();e.connection_id=b.get();e.received_qpc=static_cast<std::int64_t>(b.get());e.subscribe_floor_qpc=static_cast<std::int64_t>(b.get());e.error=static_cast<unsigned>(b.get(4));const auto f=b.frame();if(e.kind==ReadinessEventKind::hello){if(!decoded(f,s::Kind::hello,e.hello))b.ok=false;}else if(e.kind==ReadinessEventKind::frame)e.frame=f;else if(f.size)b.ok=false;}
}
struct ReadinessEvidence::Impl {
 struct Record {unsigned type{};ReadinessAdmission admission{};std::int64_t time{};ReadinessEvent event{};ReadinessView view{};ReadinessModel::Checkpoint checkpoint{};};
 mutable std::mutex mutex;std::condition_variable ready;EvidenceFileWorker file_owner;
 std::array<Record,512> queue{};std::size_t head{},size{};ReadinessEvidenceStatus view{};
 bool begun{},stopping{};std::int64_t start{},end{};std::uint64_t frequency{};
 void fail(DWORD error){std::lock_guard<std::mutex> lock(mutex);view.active=view.complete=false;view.incomplete=true;view.error=error;stopping=true;size=0;}
 bool write(const void* p,DWORD n){DWORD count{};const bool success=WriteFile(file_owner.file(),p,n,&count,nullptr)!=FALSE;const auto error=success?ERROR_WRITE_FAULT:GetLastError();std::lock_guard<std::mutex> lock(mutex);if(success&&count==n){view.bytes+=count;return true;}view.error=error;view.incomplete=true;view.active=false;stopping=true;return false;}
 void enqueue(const Record& r){std::lock_guard<std::mutex> lock(mutex);if(!view.active)return;if(size==queue.size()){++view.overflows;view.incomplete=true;view.error=ERROR_BUFFER_OVERFLOW;view.active=false;stopping=true;end=r.time;}else{queue[(head+size)%queue.size()]=r;++size;}ready.notify_one();}
 void run(){
  {std::unique_lock<std::mutex> lock(mutex);ready.wait(lock,[&]{return begun||stopping;});}
  Bytes h;h.put('P',1);h.put('X',1);h.put('S',1);h.put('C',1);h.put('A',1);h.put('1',1);h.put(0,2);h.put(frequency);h.put(start);bool healthy=write(h.data.data(),static_cast<DWORD>(h.size));
  while(healthy){Record r;{std::unique_lock<std::mutex> lock(mutex);ready.wait(lock,[&]{return size||stopping;});if(!size&&stopping)break;r=queue[head];head=(head+1)%queue.size();--size;}
   Bytes b;if(r.type==1||r.type==6){b.put(static_cast<unsigned>(r.admission),1);event(b,static_cast<const ReadinessEvent&>(r.event));}else if(r.type==2)view_record(b,r.view);else checkpoint(b,static_cast<const ReadinessModel::Checkpoint&>(r.checkpoint));
   if(!b.ok){std::lock_guard<std::mutex> lock(mutex);view.incomplete=true;view.error=ERROR_INVALID_DATA;view.active=false;stopping=true;end=r.time;continue;}
   Bytes envelope;envelope.put(r.time);envelope.put(r.type,1);envelope.put(0,3);envelope.put(b.size,4);healthy=write(envelope.data.data(),16)&&write(b.data.data(),static_cast<DWORD>(b.size));std::lock_guard<std::mutex> lock(mutex);++view.records;if(view.bytes>limit){view.incomplete=true;view.error=ERROR_FILE_TOO_LARGE;view.active=false;stopping=true;end=r.time;}
  }
  bool incomplete;std::int64_t cutoff;{std::lock_guard<std::mutex> lock(mutex);incomplete=view.incomplete||!begun;cutoff=end;}
  if(healthy){Bytes tail;tail.put(cutoff);tail.put(incomplete?4:3,1);tail.put(0,7);healthy=write(tail.data.data(),16);}
  std::lock_guard<std::mutex> lock(mutex);view.active=false;view.complete=healthy&&!view.incomplete&&begun;
 }
 static void view_record(Bytes& b,const ReadinessView& v){coordinator::view(b,v);}
};
ReadinessEvidence::ReadinessEvidence():impl_(std::make_unique<Impl>()){}
ReadinessEvidence::~ReadinessEvidence(){finish(0);join();}
bool ReadinessEvidence::prepare(const std::wstring& name,std::uint64_t frequency){if(!frequency)return false;auto* p=impl_.get();
 return p->file_owner.prepare(name,[p,frequency]{std::lock_guard<std::mutex> lock(p->mutex);p->view={};p->begun=p->stopping=false;p->head=p->size=0;p->frequency=frequency;p->start=p->end=0;},[p]{p->run();},[p](DWORD error){p->fail(error);});}
void ReadinessEvidence::begin(std::int64_t now,const ReadinessModel::Checkpoint& c){std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->file_owner.done()||impl_->stopping||impl_->begun)return;impl_->start=now;impl_->begun=impl_->view.active=true;Impl::Record r;r.type=5;r.time=now;r.checkpoint=c;impl_->queue[0]=r;impl_->size=1;impl_->ready.notify_one();}
void ReadinessEvidence::record(const ReadinessEvent& e,std::int64_t now,ReadinessAdmission admission){Impl::Record r;r.type=admission==ReadinessAdmission::accepted?1:6;r.admission=admission;r.time=now;r.event=e;impl_->enqueue(r);}
void ReadinessEvidence::assertion(const ReadinessView& v,std::int64_t now){Impl::Record r;r.type=2;r.time=now;r.view=v;impl_->enqueue(r);}
void ReadinessEvidence::finish(std::int64_t now) noexcept {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->view.active=false;if(!impl_->stopping)impl_->end=now;impl_->stopping=true;impl_->ready.notify_one();}
void ReadinessEvidence::incomplete(std::uint32_t error,std::int64_t now) noexcept {std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->view.active)return;impl_->view.incomplete=true;impl_->view.error=error;impl_->view.active=false;impl_->end=now;impl_->stopping=true;impl_->ready.notify_one();}
void ReadinessEvidence::join(){impl_->file_owner.join();}
bool ReadinessEvidence::done() const noexcept{return impl_->file_owner.done();}
bool ReadinessEvidence::test_open_hook(void (*hook)(void*) noexcept,void* context){return impl_->file_owner.test_open_hook(hook,context);}
ReadinessEvidenceStatus ReadinessEvidence::status() const {std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->view;}
ReadinessReplayResult inspect_readiness_capture(const std::wstring& name){ReadinessReplayResult r;r.view.xplane=XPlaneState::Unknown;r.view.psx=PsxState::Unknown;std::error_code error;if(!std::filesystem::exists(name,error)&&!error){r.valid=r.missing=true;return r;}std::ifstream f(std::filesystem::path(name),std::ios::binary);const auto fail=[&](const char* text){r.error=text;return r;};if(!f)return fail("Cannot open readiness sidecar");Bytes h;h.size=24;if(!f.read(reinterpret_cast<char*>(h.data.data()),24)||!std::equal(h.data.begin(),h.data.begin()+8,std::array<std::uint8_t,8>{{'P','X','S','C','A','1',0,0}}.begin()))return fail("Invalid readiness header");h.pos=8;r.qpc_frequency=h.get();r.begin_qpc=h.get();if(!r.qpc_frequency||!r.begin_qpc)return fail("Invalid readiness clock");ReadinessModel model;model.configure(r.qpc_frequency);bool seeded=false;std::uint64_t previous=r.begin_qpc,total=24;
 for(;;){Bytes e;e.size=16;if(!f.read(reinterpret_cast<char*>(e.data.data()),16))return fail("Missing readiness terminal");const auto time=e.get(),type=e.get(1),reserved=e.get(3),size=e.get(4);if(reserved||time<previous||time>INT64_MAX||size>capacity||total+16+size>limit+capacity)return fail("Invalid readiness envelope");previous=time;total+=16+size;
  if(type==3||type==4){if(size||!seeded||f.peek()!=std::char_traits<char>::eof())return fail("Invalid readiness terminal");model.tick(static_cast<std::int64_t>(time));r.view=model.view();r.cutoff_qpc=time;r.complete=type==3;r.valid=true;return r;}
  Bytes b;b.size=static_cast<std::size_t>(size);if(!f.read(reinterpret_cast<char*>(b.data.data()),static_cast<std::streamsize>(size)))return fail("Truncated readiness record");
  if(type==5){ReadinessModel::Checkpoint c;checkpoint(b,c);if(seeded||time!=r.begin_qpc||!b.ok||b.pos!=b.size||!model.restore(c,static_cast<std::int64_t>(time)))return fail("Invalid readiness checkpoint");seeded=true;}
  else if(!seeded)return fail("Readiness checkpoint absent");
  else if(type==1||type==6){const auto a=b.get(1);ReadinessEvent v;event(b,v);if(!b.ok||b.pos!=b.size||a>2||(type==1)!=(a==0)||v.received_qpc>static_cast<std::int64_t>(time)||model.event(v)!=static_cast<ReadinessAdmission>(a))return fail("Invalid readiness event");}
  else if(type==2){ReadinessView v;view(b,v);model.tick(static_cast<std::int64_t>(time));if(!b.ok||b.pos!=b.size||!equal(v,model.view()))return fail("Readiness derived assertion mismatch");++r.assertions;}
  else return fail("Unknown readiness record");++r.records;
 }
}
}
