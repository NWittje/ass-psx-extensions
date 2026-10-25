#include "coordinator/traffic_evidence.hpp"
#include "evidence_file_worker.hpp"
#include "traffic_visual_codec.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
namespace coordinator {
namespace w=traffic_wire;
namespace {
constexpr std::size_t capacity=4096;constexpr std::uint64_t file_limit=512ull*1024*1024;
struct Bytes {
 std::array<std::uint8_t,capacity> data{};std::size_t size{},pos{};bool ok{true};
 void put(std::uint64_t value,std::size_t n=8){if(n>capacity-size){ok=false;return;}for(std::size_t i=0;i<n;++i)data[size++]=static_cast<std::uint8_t>(value>>(8*i));}
 std::uint64_t get(std::size_t n=8){if(n>size-pos){ok=false;return 0;}std::uint64_t value{};for(std::size_t i=0;i<n;++i)value|=std::uint64_t(data[pos++])<<(8*i);return value;}
 void process(ProcessIdentity p){put(p.pid,4);put(p.creation_filetime);}ProcessIdentity process(){return {static_cast<std::uint32_t>(get(4)),get()};}
 void frame(const w::Frame& f){put(f.size,4);if(f.size>w::max_frame||f.size>capacity-size){ok=false;return;}std::copy(f.bytes.begin(),f.bytes.begin()+f.size,data.begin()+size);size+=f.size;}
 w::Frame frame(){w::Frame f;const auto n=get(4);if(n>w::max_frame||n>size-pos){ok=false;return f;}f.size=static_cast<std::size_t>(n);std::copy(data.begin()+pos,data.begin()+pos+f.size,f.bytes.begin());pos+=f.size;return f;}
};
template<class T> bool decoded(const w::Frame& f,w::Kind kind,T& value){w::Header h;return f.size>=16&&w::read_header(f.bytes.data(),16,h)&&h.kind==kind&&f.size==16+h.payload_size&&w::decode(f.bytes.data()+16,h.payload_size,value);}
void snapshot(Bytes& b,const psx::traffic::Snapshot& s){w::Frame f;if(s.header.sequence&&!w::encode(w::Sample{{1,1},1,1,s},1,f))b.ok=false;b.frame(f);}
void snapshot(Bytes& b,psx::traffic::Snapshot& s){const auto f=b.frame();s={};if(f.size){w::Sample value;if(!decoded(f,w::Kind::sample,value)||value.producer!=psx::motion::InstanceId{1,1}||value.connection_id!=1||value.selection_epoch!=1)b.ok=false;else s=value.snapshot;}}
void view(Bytes& b,const TrafficView& v){b.put(std::uint64_t(v.communication_healthy)|(std::uint64_t(v.source_available)<<1)|(std::uint64_t(v.retained)<<2),1);b.put(static_cast<unsigned>(v.reason),4);b.process(v.selected);b.process(v.process);b.put(v.producer.high);b.put(v.producer.low);
 for(auto value:{v.connection_id,v.selection_epoch,v.lifecycle,v.sequence})b.put(value);
 for(auto value:{v.observed_qpc,v.received_qpc,v.admitted_qpc,v.source_age_qpc,v.heartbeat_qpc})b.put(static_cast<std::uint64_t>(value));
 for(auto value:{v.capabilities,v.active_slots,v.pose_valid_slots,v.error})b.put(value,4);
 for(auto value:{v.accepted,v.obsolete,v.faults,v.generation_changes,v.revision})b.put(value);snapshot(b,v.source);b.put(static_cast<std::uint64_t>(v.status_received_qpc));b.frame(v.status_frame);
}
void view(Bytes& b,TrafficView& v){const auto flags=b.get(1),reason=b.get(4);if(flags>7||reason>12)b.ok=false;v.communication_healthy=(flags&1)!=0;v.source_available=(flags&2)!=0;v.retained=(flags&4)!=0;v.reason=static_cast<w::Reason>(reason);v.selected=b.process();v.process=b.process();v.producer={b.get(),b.get()};
 v.connection_id=b.get();v.selection_epoch=b.get();v.lifecycle=b.get();v.sequence=b.get();v.observed_qpc=static_cast<std::int64_t>(b.get());v.received_qpc=static_cast<std::int64_t>(b.get());v.admitted_qpc=static_cast<std::int64_t>(b.get());v.source_age_qpc=static_cast<std::int64_t>(b.get());v.heartbeat_qpc=static_cast<std::int64_t>(b.get());
 v.capabilities=static_cast<std::uint32_t>(b.get(4));v.active_slots=static_cast<std::uint32_t>(b.get(4));v.pose_valid_slots=static_cast<std::uint32_t>(b.get(4));v.error=static_cast<std::uint32_t>(b.get(4));
 v.accepted=b.get();v.obsolete=b.get();v.faults=b.get();v.generation_changes=b.get();v.revision=b.get();snapshot(b,v.source);v.status_received_qpc=static_cast<std::int64_t>(b.get());v.status_frame=b.frame();
}
bool equal(const TrafficView& a,const TrafficView& c){Bytes x,y;view(x,a);view(y,c);return x.ok&&y.ok&&x.size==y.size&&std::equal(x.data.begin(),x.data.begin()+x.size,y.data.begin());}
void checkpoint(Bytes& b,const TrafficModel::Checkpoint& c){b.process(c.selected);const auto& p=c.peer;b.put(std::uint64_t(p.connected)|(std::uint64_t(p.have_snapshot)<<1),1);b.process(p.process);w::Frame f;if(p.connection&&!w::encode(p.hello,1,f))b.ok=false;b.frame(f);
 for(auto value:{p.connection,p.selection,p.wire_sequence,p.source_sequence,p.lifecycle})b.put(value);
 for(auto value:{p.floor,p.source_floor,p.heartbeat,p.received,p.admitted})b.put(static_cast<std::uint64_t>(value));b.put(p.capabilities,4);b.put(p.error,4);b.put(static_cast<unsigned>(p.reason),4);snapshot(b,p.snapshot);b.put(static_cast<std::uint64_t>(p.status_received));b.frame(p.status_frame);view(b,c.view);
}
void checkpoint(Bytes& b,TrafficModel::Checkpoint& c){c.selected=b.process();auto& p=c.peer;const auto flags=b.get(1);if(flags>3)b.ok=false;p.connected=(flags&1)!=0;p.have_snapshot=(flags&2)!=0;p.process=b.process();const auto hello=b.frame();p.connection=b.get();p.selection=b.get();p.wire_sequence=b.get();p.source_sequence=b.get();p.lifecycle=b.get();
 p.floor=static_cast<std::int64_t>(b.get());p.source_floor=static_cast<std::int64_t>(b.get());p.heartbeat=static_cast<std::int64_t>(b.get());p.received=static_cast<std::int64_t>(b.get());p.admitted=static_cast<std::int64_t>(b.get());p.capabilities=static_cast<std::uint32_t>(b.get(4));p.error=static_cast<std::uint32_t>(b.get(4));const auto reason=b.get(4);if(reason>12)b.ok=false;p.reason=static_cast<w::Reason>(reason);if(p.connection?!decoded(hello,w::Kind::hello,p.hello):hello.size!=0)b.ok=false;snapshot(b,p.snapshot);p.status_received=static_cast<std::int64_t>(b.get());p.status_frame=b.frame();view(b,c.view);
}
void event(Bytes& b,const TrafficEvent& e){b.put(static_cast<unsigned>(e.kind),4);b.process(e.process);b.put(e.connection_id);b.put(e.selection_epoch);b.put(static_cast<std::uint64_t>(e.received_qpc));b.put(static_cast<std::uint64_t>(e.subscribe_floor_qpc));b.put(e.error,4);b.put(static_cast<unsigned>(e.reason),4);w::Frame f;if(e.kind==TrafficEventKind::hello){if(!w::encode(e.hello,1,f))b.ok=false;}else if(e.kind==TrafficEventKind::frame)f=e.frame;b.frame(f);}
void event(Bytes& b,TrafficEvent& e){const auto kind=b.get(4);if(kind>3)b.ok=false;e.kind=static_cast<TrafficEventKind>(kind);e.process=b.process();e.connection_id=b.get();e.selection_epoch=b.get();e.received_qpc=static_cast<std::int64_t>(b.get());e.subscribe_floor_qpc=static_cast<std::int64_t>(b.get());e.error=static_cast<std::uint32_t>(b.get(4));const auto reason=b.get(4);if(reason>12)b.ok=false;e.reason=static_cast<w::Reason>(reason);const auto f=b.frame();if(e.kind==TrafficEventKind::hello){if(!decoded(f,w::Kind::hello,e.hello))b.ok=false;}else if(e.kind==TrafficEventKind::frame)e.frame=f;else if(f.size)b.ok=false;}
std::uint32_t crc(const void* input,std::size_t n,std::uint32_t value=0xffffffffu){const auto* data=static_cast<const std::uint8_t*>(input);for(std::size_t i=0;i<n;++i){value^=data[i];for(unsigned j=0;j<8;++j)value=(value>>1)^((value&1u)?0xedb88320u:0);}return value;}
}
struct TrafficEvidence::Impl {
 struct Record {unsigned type{};TrafficAdmission admission{};TrafficVisualAdmission visual_admission{};std::int64_t time{};TrafficEvent event{};TrafficView view{};TrafficModel::Checkpoint checkpoint{};TrafficVisualEvent visual_event{};TrafficVisualView visual_view{};};
 mutable std::mutex mutex;std::condition_variable ready;EvidenceFileWorker file_owner;
 std::array<Record,256> queue{};std::size_t head{},size{};TrafficEvidenceStatus status{};TrafficEvidenceLimits limits{};
 bool begun{},stopping{};std::int64_t start{},end{};std::uint64_t frequency{},ordinal{};
 bool visual{};std::unique_ptr<TrafficVisualModel::State> visual_checkpoint;
 void fail(DWORD error){std::lock_guard<std::mutex> lock(mutex);status.active=status.complete=false;status.incomplete=true;status.error=error;stopping=true;size=0;}
 explicit Impl(TrafficEvidenceLimits l):limits(l){limits.queue_records=(std::max)(std::size_t{1},(std::min)(l.queue_records,queue.size()));limits.file_bytes=(std::min)(l.file_bytes,file_limit);}
 bool write(const void* data,DWORD count){DWORD done{};const bool success=WriteFile(file_owner.file(),data,count,&done,nullptr)!=FALSE;const auto error=success?ERROR_WRITE_FAULT:GetLastError();std::lock_guard<std::mutex> lock(mutex);if(success&&done==count){status.bytes+=done;return true;}status.error=error;status.incomplete=true;status.active=false;stopping=true;return false;}
 bool packet(unsigned type,std::int64_t time,const Bytes& b){
  {std::lock_guard<std::mutex> lock(mutex);const auto reserve=(type==3||type==4)?0u:28u;if(status.bytes+28+b.size+reserve>limits.file_bytes){status.error=ERROR_FILE_TOO_LARGE;status.incomplete=true;status.active=false;stopping=true;end=(std::max)(end,time);return false;}}
  Bytes envelope;envelope.put(static_cast<std::uint64_t>(time));envelope.put(type,4);envelope.put(b.size,4);envelope.put(++ordinal);envelope.put((crc(b.data.data(),b.size,crc(envelope.data.data(),envelope.size))^0xffffffffu),4);return write(envelope.data.data(),28)&&(!b.size||write(b.data.data(),static_cast<DWORD>(b.size)));
 }
 void enqueue(const Record& r){std::lock_guard<std::mutex> lock(mutex);if(!status.active)return;if(size==limits.queue_records){++status.overflows;status.incomplete=true;status.error=ERROR_BUFFER_OVERFLOW;status.active=false;stopping=true;end=r.time;}else{queue[(head+size)%queue.size()]=r;++size;}ready.notify_one();}
 void run(){
  {std::unique_lock<std::mutex> lock(mutex);ready.wait(lock,[&]{return begun||stopping;});}
  Bytes header;for(const auto c:std::array<unsigned char,8>{'P','X','F','C','A',static_cast<unsigned char>(visual?'2':'1'),0,0})header.put(c,1);header.put(frequency);header.put(static_cast<std::uint64_t>(start));bool healthy=write(header.data.data(),24);
  while(healthy){Record r;{std::unique_lock<std::mutex> lock(mutex);ready.wait(lock,[&]{return size||stopping;});if(!size&&stopping)break;r=queue[head];head=(head+1)%queue.size();--size;}
   if(limits.writer_delay_ms)Sleep(limits.writer_delay_ms);Bytes b;
   if(r.type==1){b.put(static_cast<unsigned>(r.admission),4);event(b,static_cast<const TrafficEvent&>(r.event));}else if(r.type==2)view(b,static_cast<const TrafficView&>(r.view));
   else if(r.type==5)checkpoint(b,static_cast<const TrafficModel::Checkpoint&>(r.checkpoint));
   else {
    std::vector<std::uint8_t> bytes;
    if(r.type==8){b.put(static_cast<unsigned>(r.visual_admission),4);if(!traffic_visual_capture::encode(r.visual_event,bytes))b.ok=false;}
    else if(r.type==9){if(!traffic_visual_capture::encode(r.visual_view,bytes))b.ok=false;}
    else if(r.type==10){if(!traffic_visual_capture::encode(r.visual_event,bytes))b.ok=false;}
    else b.ok=false;
    if(bytes.size()>capacity-b.size)b.ok=false;else {std::copy(bytes.begin(),bytes.end(),b.data.begin()+b.size);b.size+=bytes.size();}
   }
   if(!b.ok){std::lock_guard<std::mutex> lock(mutex);status.incomplete=true;status.error=ERROR_INVALID_DATA;status.active=false;stopping=true;end=r.time;continue;}
   healthy=packet(r.type,r.time,b);
   if(healthy&&r.type==5&&visual){
    std::vector<std::uint8_t> bytes;if(!visual_checkpoint||!traffic_visual_capture::encode(*visual_checkpoint,bytes)){std::lock_guard<std::mutex> lock(mutex);status.incomplete=true;status.error=ERROR_INVALID_DATA;status.active=false;stopping=true;end=(std::max)(end,r.time);}
    else {for(std::size_t offset=0;healthy&&offset<bytes.size();){Bytes chunk;chunk.put(offset);chunk.put(bytes.size());const auto count=(std::min)(capacity-16,bytes.size()-offset);std::copy(bytes.begin()+offset,bytes.begin()+offset+count,chunk.data.begin()+chunk.size);chunk.size+=count;healthy=packet(6,r.time,chunk);offset+=count;}if(healthy){Bytes empty;healthy=packet(7,r.time,empty);}}
   }
   std::lock_guard<std::mutex> lock(mutex);++status.records;if(status.bytes>limits.file_bytes){status.incomplete=true;status.error=ERROR_FILE_TOO_LARGE;status.active=false;stopping=true;end=(std::max)(end,r.time);}
  }
  bool incomplete;std::int64_t cutoff;{std::lock_guard<std::mutex> lock(mutex);incomplete=status.incomplete||!begun;cutoff=end;if(!healthy&&status.error==ERROR_FILE_TOO_LARGE)healthy=true;}
  if(healthy){Bytes empty;healthy=packet(incomplete?4:3,cutoff,empty);}
  std::lock_guard<std::mutex> lock(mutex);status.active=false;status.complete=healthy&&!status.incomplete&&begun;
 }
};
TrafficEvidence::TrafficEvidence(TrafficEvidenceLimits limits):impl_(std::make_unique<Impl>(limits)){}
TrafficEvidence::~TrafficEvidence(){finish(0);join();}
bool TrafficEvidence::prepare(const std::wstring& name,std::uint64_t frequency,bool visual){if(!frequency||frequency>1000000000000ULL||impl_->limits.file_bytes<52)return false;auto* p=impl_.get();
 return p->file_owner.prepare(name,[p,frequency,visual]{std::lock_guard<std::mutex> lock(p->mutex);p->status={};p->begun=p->stopping=false;p->head=p->size=0;p->frequency=frequency;p->start=p->end=0;p->ordinal=0;p->visual=visual;p->visual_checkpoint=visual?std::make_unique<TrafficVisualModel::State>():nullptr;},[p]{p->run();},[p](DWORD error){p->fail(error);});
}
void TrafficEvidence::begin(std::int64_t time,const TrafficModel::Checkpoint& c){std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->file_owner.done()||impl_->stopping||impl_->begun)return;impl_->start=time;impl_->begun=impl_->status.active=true;Impl::Record r;r.type=5;r.time=time;r.checkpoint=c;impl_->queue[0]=r;impl_->size=1;impl_->ready.notify_one();}
void TrafficEvidence::begin(std::int64_t time,const TrafficModel::Checkpoint& c,const TrafficVisualModel::State& visual){
 std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->file_owner.done()||impl_->stopping||impl_->begun||!impl_->visual_checkpoint)return;
 *impl_->visual_checkpoint=visual;impl_->start=time;impl_->begun=impl_->status.active=true;Impl::Record r;r.type=5;r.time=time;r.checkpoint=c;impl_->queue[0]=r;impl_->size=1;impl_->ready.notify_one();
}
void TrafficEvidence::record(const TrafficEvent& e,std::int64_t time,TrafficAdmission admission){Impl::Record r;r.type=1;r.time=time;r.admission=admission;r.event=e;impl_->enqueue(r);}
void TrafficEvidence::assertion(const TrafficView& v,std::int64_t time){Impl::Record r;r.type=2;r.time=time;r.view=v;impl_->enqueue(r);}
void TrafficEvidence::visual_record(const TrafficVisualEvent& e,std::int64_t time,TrafficVisualAdmission admission){Impl::Record r;r.type=8;r.time=time;r.visual_event=e;r.visual_admission=admission;impl_->enqueue(r);}
void TrafficEvidence::visual_dispatch(const visual_wire::Frame& frame,std::int64_t time){Impl::Record r;r.type=10;r.time=time;r.visual_event.kind=TrafficVisualEventKind::dispatch;r.visual_event.received_qpc=time;r.visual_event.frame=frame;impl_->enqueue(r);}
void TrafficEvidence::visual_assertion(const TrafficVisualView& v,std::int64_t time){Impl::Record r;r.type=9;r.time=time;r.visual_view=v;impl_->enqueue(r);}
void TrafficEvidence::finish(std::int64_t time) noexcept {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->status.active=false;if(!impl_->stopping)impl_->end=time;impl_->stopping=true;impl_->ready.notify_one();}
void TrafficEvidence::incomplete(std::uint32_t error,std::int64_t time) noexcept {std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->status.active)return;impl_->status.incomplete=true;impl_->status.error=error;impl_->status.active=false;impl_->end=time;impl_->stopping=true;impl_->ready.notify_one();}
void TrafficEvidence::join(){impl_->file_owner.join();}
bool TrafficEvidence::done() const noexcept{return impl_->file_owner.done();}
bool TrafficEvidence::test_open_hook(void (*hook)(void*) noexcept,void* context){return impl_->file_owner.test_open_hook(hook,context);}
TrafficEvidenceStatus TrafficEvidence::status() const {std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->status;}
TrafficReplayResult inspect_traffic_capture(const std::wstring& name){TrafficReplayResult result;std::error_code error;if(!std::filesystem::exists(name,error)&&!error){result.valid=result.missing=true;return result;}std::ifstream file(std::filesystem::path(name),std::ios::binary);const auto fail=[&](const char* text){result.error=text;return result;};if(!file)return fail("Cannot open traffic sidecar");
 Bytes header;header.size=24;if(!file.read(reinterpret_cast<char*>(header.data.data()),24))return fail("Invalid traffic header");
 const bool visual=header.data[5]=='2';auto magic=std::array<std::uint8_t,8>{'P','X','F','C','A',static_cast<std::uint8_t>(visual?'2':'1'),0,0};if(!std::equal(header.data.begin(),header.data.begin()+8,magic.begin()))return fail("Invalid traffic header");
 header.pos=8;result.qpc_frequency=header.get();result.begin_qpc=header.get();if(!result.qpc_frequency||result.qpc_frequency>1000000000000ULL||!result.begin_qpc||result.begin_qpc>INT64_MAX)return fail("Invalid traffic clock");
 TrafficModel model;model.configure(result.qpc_frequency);bool seeded=false;std::uint64_t previous=result.begin_qpc,ordinal{},total=24;
 TrafficVisualModel destination;std::vector<std::uint8_t> state_bytes;std::uint64_t state_size{};bool visual_seeded=false;
 for(;;){Bytes envelope;envelope.size=28;if(!file.read(reinterpret_cast<char*>(envelope.data.data()),28))return fail("Missing traffic terminal");const auto time=envelope.get(),type=envelope.get(4),size=envelope.get(4),order=envelope.get(),checksum=envelope.get(4);
  if(time<previous||time>INT64_MAX||size>capacity||order!=++ordinal||total+28+size>file_limit+capacity*2)return fail("Invalid traffic envelope");previous=time;total+=28+size;Bytes b;b.size=static_cast<std::size_t>(size);if(size&&!file.read(reinterpret_cast<char*>(b.data.data()),static_cast<std::streamsize>(size)))return fail("Truncated traffic record");if(checksum!=(crc(b.data.data(),b.size,crc(envelope.data.data(),24))^0xffffffffu))return fail("Traffic checksum mismatch");
  if(type==3||type==4){if(size||!seeded||(visual&&!visual_seeded)||file.peek()!=std::char_traits<char>::eof())return fail("Invalid traffic terminal");model.tick(static_cast<std::int64_t>(time));result.view=model.view();result.cutoff_qpc=time;result.complete=type==3;result.valid=true;if(visual){destination.tick(static_cast<std::int64_t>(time));result.visual=destination.view();result.visual_retained=true;}return result;}
  if(type==5){TrafficModel::Checkpoint c;checkpoint(b,c);if(seeded||time!=result.begin_qpc||!b.ok||b.pos!=b.size||!model.restore(c,static_cast<std::int64_t>(time)))return fail("Invalid traffic retained checkpoint");seeded=true;}
  else if(!seeded)return fail("Traffic starting checkpoint absent");
  else if(visual&&!visual_seeded){
   if(type==6){const auto offset=b.get(),count=b.get();if(!b.ok||time!=result.begin_qpc||offset!=state_bytes.size()||!count||count>1024*1024||(state_size&&state_size!=count)||b.size==b.pos||count-offset<b.size-b.pos)return fail("Invalid visual checkpoint chunk");state_size=count;state_bytes.insert(state_bytes.end(),b.data.begin()+b.pos,b.data.begin()+b.size);}
   else if(type==7){auto state=std::make_unique<TrafficVisualModel::State>();if(size||time!=result.begin_qpc||state_bytes.size()!=state_size||!traffic_visual_capture::decode(state_bytes,*state)||state->frequency!=result.qpc_frequency||!psx::motion::valid(state->view.owner))return fail("Invalid visual starting state");destination.configure(result.qpc_frequency,state->view.owner);if(!destination.restore(*state,static_cast<std::int64_t>(time)))return fail("Invalid visual checkpoint provenance");visual_seeded=true;state_bytes.clear();}
   else return fail("Visual starting checkpoint absent");
  }
  else if(type==1){const auto admission=b.get(4);TrafficEvent e;event(b,e);if(!b.ok||b.pos!=b.size||admission>2||e.received_qpc>static_cast<std::int64_t>(time)||model.event(e,static_cast<std::int64_t>(time))!=static_cast<TrafficAdmission>(admission))return fail("Invalid traffic event/admission");}
  else if(type==2){TrafficView v;view(b,v);model.tick(static_cast<std::int64_t>(time));if(!b.ok||b.pos!=b.size||!equal(v,model.view()))return fail("Traffic derived assertion mismatch");++result.assertions;}
  else if(visual&&(type==8||type==10)){
   const auto admission=type==8?b.get(4):0;std::vector<std::uint8_t> bytes(b.data.begin()+b.pos,b.data.begin()+b.size);TrafficVisualEvent e;
   if(!b.ok||!traffic_visual_capture::decode(bytes,e)||e.received_qpc>static_cast<std::int64_t>(time)||admission>2)return fail("Invalid visual event");
   if(type==8){if(e.kind==TrafficVisualEventKind::dispatch||destination.event(e,static_cast<std::int64_t>(time))!=static_cast<TrafficVisualAdmission>(admission))return fail("Visual event admission mismatch");}
   else {visual_wire::Frame actual;if(e.kind!=TrafficVisualEventKind::dispatch||!destination.next(actual,static_cast<std::int64_t>(time))||actual.size!=e.frame.size||!std::equal(actual.bytes.begin(),actual.bytes.begin()+actual.size,e.frame.bytes.begin()))return fail("Visual dispatch mismatch");}
  }
  else if(visual&&type==9){std::vector<std::uint8_t> bytes(b.data.begin(),b.data.begin()+b.size);TrafficVisualView expected;destination.tick(static_cast<std::int64_t>(time));if(!traffic_visual_capture::decode(bytes,expected)||!traffic_visual_capture::equal(expected,destination.view()))return fail("Visual derived assertion mismatch");++result.visual_assertions;}
  else return fail("Unknown traffic record");++result.records;
 }
}
}
