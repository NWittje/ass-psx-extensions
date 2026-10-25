#include <du_client/runtime.hpp>
#include <du_client/config.hpp>
#include <psx/display/tcp.hpp>
#include <psx/network/io.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>
namespace du_client {
uint64_t qpc(){return psx::du::transport::qpc();}uint64_t frequency(){return psx::du::transport::frequency();}
std::vector<Source> discover(unsigned unit){return psx::du::transport::discover(unit);}
Runtime::Runtime(bool enabled, unsigned decode_delay_ms, unsigned unit,bool instrument_prepare_failure,std::filesystem::path health_output)
    : trim_(instrument_prepare_failure),enabled_(enabled), unit_(unit), decode_delay_ms_(decode_delay_ms),health_output_(std::move(health_output))
{
    if (unit >= 6)
        throw std::runtime_error("Display unit must be 0..5");
    for (auto &s : slots_)
        s.reserve(psx::recorder::frame_limit);
    control_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!control_)throw std::runtime_error("DU receiver control event");
    health_stop_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!health_stop_){CloseHandle(control_);throw std::runtime_error("DU health event");}
    health_wake_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!health_wake_){CloseHandle(control_);CloseHandle(health_stop_);throw std::runtime_error("DU diagnostics wake event");}
    try{reader_ = std::thread(&Runtime::reader, this);decoder_ = std::thread(&Runtime::decoder, this);health_=std::thread(&Runtime::health,this);}
    catch(...){shutdown();join();CloseHandle(control_);CloseHandle(health_stop_);CloseHandle(health_wake_);throw;}
}
Runtime::~Runtime()
{
    shutdown();
    join();
    CloseHandle(control_);
    CloseHandle(health_stop_);
    CloseHandle(health_wake_);
}
void Runtime::diagnostics(bool enabled) noexcept {
    if(health_enabled_.exchange(enabled)!=enabled){++health_generation_;SetEvent(health_wake_);}
}
void Runtime::select_trim(bool enabled)
{
    enabled=enabled&&(unit_.load()==4||unit_.load()==5);
    enabled=trim_.enable(enabled);
    if(trim_enabled_.exchange(enabled)==enabled)return;
    reconnect_=true;SetEvent(control_);
}
void Runtime::connect(std::wstring pipe)
{
    trim_.reset();
    {
        std::lock_guard l(mutex_);
        selected_ = std::move(pipe);
        view_.frame.reset();
        ++view_.connection_epoch;
        view_.last_received = 0;
        view_.source_ready = false;
    }
    enabled_ = true;
    reconnect_ = true;
    SetEvent(control_);
}
void Runtime::select_unit(unsigned unit)
{
    if (unit >= 6)
        throw std::runtime_error("Display unit must be 0..5");
    if (unit_.exchange(unit) != unit)
    {
        trim_.reset();
        if(unit!=4&&unit!=5){trim_enabled_=false;trim_.enable(false);}
        std::lock_guard l(mutex_);
        selected_.clear();
        view_.frame.reset();
        view_.last_received = 0;
        view_.source_ready = false;
        ++view_.connection_epoch;
        reconnect_ = true;
        SetEvent(control_);
    }
}
void Runtime::stop()
{
    enabled_ = false;
    reconnect_ = true;
    SetEvent(control_);
}
void Runtime::reconnect()
{
    enabled_ = true;
    reconnect_ = true;
    SetEvent(control_);
}
void Runtime::shutdown()
{
    quit_ = true;
    trim_.shutdown();
    enabled_ = false;
    ready_.notify_all();
    SetEvent(control_);
}

void Runtime::join()
{
    if (reader_.joinable())
        reader_.join();
    if (decoder_.joinable())
        decoder_.join();
    SetEvent(health_stop_);
    if(health_.joinable())health_.join();
    trim_.join();
}
void Runtime::acknowledge(uint64_t epoch, uint64_t sequence)
{
    std::lock_guard l(mutex_);
    if (epoch == view_.connection_epoch)
        acknowledged_ = sequence;
    if(health_enabled_&&epoch==view_.connection_epoch)failures_.recover(failure_record(FailureStage::replay,epoch,sequence));
}
void Runtime::rejected(uint64_t epoch, uint64_t sequence,uint32_t error,std::wstring_view detail)
{
    std::lock_guard l(mutex_);
    if (epoch == view_.connection_epoch)
    {
        acknowledged_ = sequence;
        ++view_.replay_rejected;
        if(health_enabled_){auto r=failure_record(FailureStage::replay,epoch,sequence);r.error=error;failures_.reject(r,detail);}
    }
}
FailureRecord Runtime::failure_record(FailureStage stage,uint64_t epoch,uint64_t sequence)const noexcept {
    FailureRecord r;r.stage=stage;r.qpc=qpc();r.epoch=epoch;r.sequence=sequence;r.unit=unit_.load();
    if(epoch==view_.connection_epoch){r.source_pid=view_.source_pid;r.source_created=view_.source_creation;
        if(view_.frame&&view_.frame->header.sequence==sequence){r.paint=view_.frame->header.paint;r.generation=view_.frame->header.generation;}}
    return r;
}
void Runtime::failure(FailureStage stage,uint64_t epoch,uint64_t sequence,uint32_t error,std::wstring_view detail,uint64_t count,const psx::du::transport::Origin* source){
    std::lock_guard lock(mutex_);if(!health_enabled_)return;auto r=failure_record(stage,epoch,sequence);r.error=error;if(source){r.source_pid=source->pid;r.source_created=source->created;}failures_.reject(r,detail,count);
}
void Runtime::recovered(FailureStage stage,uint64_t epoch,uint64_t sequence,bool drawn){
    std::lock_guard lock(mutex_);if(epoch!=view_.connection_epoch)return;if(health_enabled_)failures_.recover(failure_record(stage,epoch,sequence));if(drawn)++view_.drawn;
}
View Runtime::view(bool adopt)
{
    std::lock_guard l(mutex_);
    auto v = view_;
    v.stopped=reader_finished_&&decoder_finished_;
    v.native=native_monitor_.snapshot();
    v.failures=failures_.snapshot();
    if(auto trace=trace_.load()){v.diagnostic_lost=trace->lost();v.diagnostics_failed=trace->failed();}
    if (adopt && v.frame && acknowledged_!=v.frame->header.sequence) {
        acknowledged_ = v.frame->header.sequence;
        if(auto trace=trace_.load())trace->event(2,{v.correlation,qpc(),0,v.frame->header.end,GetCurrentThreadId(),0,uint16_t(unit_.load()),uint16_t(psx::norm::Stage::ui_adopt),v.connection});
    }
    v.active = enabled_;
    if (!v.connected && v.active && v.frame && v.last_received && qpc()-v.last_received>=2*frequency())
        v.status += L" — last display retained (stale)";
    else if (!v.connected && v.active && v.frame &&
        (v.status == L"Source disconnected — last display retained" ||
         v.status == L"Waiting for local PSX grabber"))
    {
        v.status = v.last_received && qpc() - v.last_received < 2 * frequency()
                       ? L""
                       : L"Source unavailable — last display retained (stale)";
    }
    if (v.connected && v.last_received && qpc() - v.last_received > 2 * frequency())
        v.status = L"Source traffic missing — last display retained (stale)";
    return v;
}
void Runtime::status(std::wstring message, bool connected)
{
    std::lock_guard l(mutex_);
    view_.status = std::move(message);
    view_.connected = connected;
    if(!connected)view_.source_ready=false;
}
void Runtime::configure_source(bool network,std::string host,std::wstring guid,uint16_t port){
 trim_.reset();
 if(!port)throw std::runtime_error("Invalid DU host port");
 if(network)host=network_host(host);
 {std::lock_guard lock(mutex_);network_=network;tcp_port_=port;host_=std::move(host);adapter_guid_=std::move(guid);selected_.clear();view_.frame.reset();view_.last_received=0;++view_.connection_epoch;}
 reconnect_=true;
 SetEvent(control_);
}
psx::du::transport::tcp::Disposition Runtime::publish(std::span<const uint8_t>b,const psx::du::transport::Origin&origin,uint64_t generation,uint64_t sequence,uint64_t epoch,bool same_clock,const psx::norm::Correlation&correlation,unsigned connection){
 if(b.size()>psx::recorder::frame_limit)throw std::runtime_error("Raw frame bound");
 auto d=psx::recorder::decode(b);if(d.unit!=unit_.load())return psx::du::transport::tcp::Disposition::budget;if(d.sequence!=sequence||d.generation!=generation)throw std::runtime_error("Frame transport mismatch");
 auto validated=qpc();auto token=correlation;if(!token.frame){token.frame=d.sequence;token.generation=d.generation;}
 if(auto trace=trace_.load())trace->event(0,{token,validated,b.size(),d.end,GetCurrentThreadId(),uint32_t(same_clock&&d.frequency==frequency()),uint16_t(d.unit),uint16_t(psx::norm::Stage::validated),connection});
 if(auto trace=trace_.load())trace->digest(0,token,d.unit,b);
 {std::lock_guard lock(mutex_);++view_.validated;
  if(same_clock&&d.frequency==frequency()&&validated>=d.end){auto ms=double(validated-d.end)*1000/frequency();view_.delivery_max_ms=std::max(view_.delivery_max_ms,ms);if(ms>50)++view_.delivery_warnings;}}
 std::lock_guard lock(queue_mutex_);int slot=assembling_>=0?assembling_:0;if(assembling_<0)while(slot==pending_||slot==working_)++slot;if(slot>=3)throw std::runtime_error("Raw slots exhausted");
 if(auto trace=trace_.load())trace->event(0,{token,qpc(),b.size(),d.end,GetCurrentThreadId(),0,uint16_t(d.unit),uint16_t(psx::norm::Stage::raw_begin),connection});
 if(b.data()!=slots_[slot].data())slots_[slot].assign(b.begin(),b.end());auto received=qpc();
 const bool replaced=pending_>=0;assembling_=-1;
 {std::lock_guard v(mutex_);if(epoch!=view_.connection_epoch||!enabled_||quit_)return psx::du::transport::tcp::Disposition::budget;view_.last_received=received;view_.source_ready=true;if(view_.source_pid&&(view_.source_pid!=origin.pid||view_.source_creation!=origin.created))trim_.reset();view_.source_pid=origin.pid;view_.source_creation=origin.created;if(!view_.identity||*view_.identity!=origin.identity)view_.identity=std::make_shared<const std::string>(origin.identity);if(pending_>=0)++view_.raw_replaced;}
 if(pending_>=0)if(auto trace=trace_.load())trace->event(0,{slot_correlations_[pending_],qpc(),0,0,GetCurrentThreadId(),0,uint16_t(slot_units_[pending_]),uint16_t(psx::norm::Stage::replaced),slot_connections_[pending_]});
 pending_=slot;slot_epochs_[slot]=epoch;slot_generations_[slot]=generation;slot_sequences_[slot]=sequence;slot_received_[slot]=validated;slot_units_[slot]=d.unit;slot_same_clock_[slot]=same_clock&&d.frequency==frequency();slot_correlations_[slot]=token;slot_connections_[slot]=connection;ready_.notify_one();
 if(auto trace=trace_.load())trace->event(0,{token,received,b.size(),d.end,GetCurrentThreadId(),0,uint16_t(d.unit),uint16_t(psx::norm::Stage::raw_commit),connection});
 return replaced?psx::du::transport::tcp::Disposition::replaced:psx::du::transport::tcp::Disposition::accepted;
}
void Runtime::reader(){
 try{while(!quit_){
  if(!enabled_){status(L"Reception stopped — last display retained");WaitForSingleObject(control_,INFINITE);continue;}
  reconnect_=false;bool network;std::string host;std::wstring guid,selected;uint16_t port;uint64_t epoch;unsigned unit=unit_;
  {std::lock_guard lock(mutex_);network=network_;port=tcp_port_;host=host_;guid=adapter_guid_;selected=selected_;epoch=++view_.connection_epoch;}
  if(!network){
   uint64_t local_epoch{},local_invalid{},local_received{};
   std::unique_ptr<psx::du::transport::LocalSource> instrument;uint64_t instrument_discovery_due{};
   psx::du::transport::LocalSource input(unit,selected,[&](auto bytes,const auto& v){if(local_epoch!=v.epoch){std::lock_guard lock(mutex_);epoch=++view_.connection_epoch;view_.frame.reset();view_.last_received=0;local_epoch=v.epoch;}publish(bytes,v.origin,psx::display::word(bytes.data(),32),psx::display::word(bytes.data(),16),epoch);});
   while(!quit_&&enabled_&&!reconnect_){auto v=input.view();
    if(v.invalid>local_invalid){failure(FailureStage::admission,epoch,v.rejected_sequence,0,v.rejection.data(),v.invalid-local_invalid,&v.origin);local_invalid=v.invalid;}
    if(v.received>local_received&&v.received>v.rejected_qpc){recovered(FailureStage::admission,epoch,0);local_received=v.received;}
{std::lock_guard lock(mutex_);view_.source_lost=v.lost;view_.invalid=std::max(view_.invalid,v.invalid);}status(v.status,v.connected);
    if(trim_enabled_&&v.connected&&v.origin.pid&&!instrument&&qpc()>=instrument_discovery_due){instrument_discovery_due=qpc()+frequency();auto choices=psx::du::transport::discover_instruments();auto chosen=std::find_if(choices.begin(),choices.end(),[&](auto&s){return s.pid==v.origin.pid&&s.created==v.origin.created;});if(chosen!=choices.end())instrument=std::make_unique<psx::du::transport::LocalSource>(psx::instrument::captain_stab_trim,chosen->pipe,psx::du::transport::OwnedSink{[this](auto bytes,const auto&source){trim_.offer(std::move(bytes),source);},nullptr,0},psx::du::transport::InstrumentSourceTag{});}
    if(instrument&&(!trim_enabled_||!v.connected)){instrument->request_stop();instrument.reset();}
    if(instrument){auto state=instrument->view();trim_.status(state.status,state.connected);}else if(trim_enabled_)trim_.status(L"STAB TRIM source unavailable",false,false);
    Sleep(10);
   }if(instrument)instrument->request_stop();input.request_stop();instrument.reset();input.stop();
  }else{
   try{
    {std::lock_guard lock(mutex_);++view_.network_attempts;}
    using namespace psx::du::transport;
    auto endpoints=psx::network::resolve_ipv4(host,control_);
    uint64_t attempt{};{std::lock_guard lock(mutex_);attempt=view_.network_attempts;}
    const auto resolved=endpoints[(attempt-1)%endpoints.size()].host();
    tcp::ClientOptions o;o.host=resolved;o.port=port;o.unit=unit;o.protocol=2;o.instrument=trim_enabled_;o.local="0.0.0.0";
    bool same_clock=resolved.starts_with("127.");
    if(!same_clock)for(auto&a:psx::network::enumerate(psx::network::InventoryPolicy::display))
        for(auto&ip:a.addresses)if(ip.host()==resolved)same_clock=true;
    std::array<uint8_t,16>session{};uint64_t stream_generation{};
    unsigned connection{};tcp::ClientCallbacks callbacks;callbacks.connected=[&](const auto&id,int tx,int rx){std::memcpy(&connection,id.data(),4);std::lock_guard l(mutex_);view_.send_buffer=tx;view_.receive_buffer=rx;};
    callbacks.instrument_acquire=[&](size_t bytes){return trim_.acquire(bytes);};
    callbacks.instrument_abort=[&]{trim_.abort();};
    callbacks.instrument_complete=[&](const InstrumentEnvelope&e,uint64_t,uint64_t completed){return trim_.publish(e,completed,connection);};
    callbacks.instrument_capability=[&](bool available){if(trim_enabled_&&!available)trim_.status(L"STAB TRIM unavailable: host supports only DUT1",false,false);};
    callbacks.instrument_status=[&](const InstrumentEnvelope&e){trim_.status(e.state==State::waiting?L"":e.state==State::busy?L"STAB TRIM source busy":L"STAB TRIM source unavailable",e.state==State::waiting,e.state==State::waiting||e.state==State::busy);};
    callbacks.endpoints=[&](std::string_view local,std::string_view remote){
        std::lock_guard lock(mutex_);view_.local_ipv4={};view_.remote_ipv4={};
        std::memcpy(view_.local_ipv4.data(),local.data(),std::min(local.size(),size_t(15)));
        std::memcpy(view_.remote_ipv4.data(),remote.data(),std::min(remote.size(),size_t(15)));
    };
    callbacks.stop=[&]{return quit_||!enabled_||reconnect_||unit!=unit_.load();};
    callbacks.acquire=[&](size_t bytes){std::lock_guard l(queue_mutex_);if(bytes>psx::recorder::frame_limit)throw std::runtime_error("TCP raw payload limit");int slot=0;while(slot==pending_||slot==working_)++slot;if(slot>=3||assembling_>=0)throw std::runtime_error("TCP raw ownership exhausted");slots_[slot].resize(bytes);assembling_=slot;return std::span<uint8_t>(slots_[slot]);};
    callbacks.abort=[&]{std::lock_guard l(queue_mutex_);assembling_=-1;};
    callbacks.complete=[&](const Envelope&e,uint64_t,uint64_t){
     if(session!=e.session||stream_generation!=e.generation){std::lock_guard l(mutex_);epoch=++view_.connection_epoch;if(view_.source_pid&&(view_.source_pid!=e.source.pid||view_.source_creation!=e.source.created)){view_.frame.reset();view_.last_received=0;}session=e.session;stream_generation=e.generation;}
     auto result=publish(e.frame,e.source,psx::display::word(e.frame.data(),32),psx::display::word(e.frame.data(),16),epoch,same_clock,{e.session,e.generation,e.sequence,psx::display::word(e.frame.data(),16)},connection);reconnect_backoff_=250;status(L"",true);return result;
    };
    callbacks.status=[&](const Envelope&e){if(e.state!=State::waiting){std::lock_guard lock(mutex_);view_.source_ready=false;}if(e.state!=State::waiting)status(e.state==State::busy?L"Source busy":e.state==State::disabled?L"Host publication disabled":L"Source unavailable — last display retained",true);};
    status(L"Connecting to TCP DU host");tcp::receive(o,control_,callbacks,trace_.load(),&native_monitor_);
   }catch(const std::exception&e){{std::lock_guard lock(mutex_);++view_.network_failures;if(std::string_view(e.what())=="TCP host protocol progress timeout (5 seconds)")++view_.protocol_timeouts;}std::wstring message=L"Network source unavailable: ";for(auto p=e.what();*p;++p)message+=wchar_t(uint8_t(*p));status(message);if(trim_enabled_)trim_.status(L"STAB TRIM network source disconnected",false);if(!quit_&&enabled_&&!reconnect_)WaitForSingleObject(control_,reconnect_backoff_);reconnect_backoff_=std::min(2000u,reconnect_backoff_*2);}
  }
 }}catch(...){status(L"Source worker failed");enabled_=false;}
 {std::lock_guard lock(queue_mutex_);decoder_done_=true;}ready_.notify_all();
 reader_finished_=true;
}
void Runtime::decoder()
{
    for (;;)
    {
        int slot;
        uint64_t epoch;
        {
            std::unique_lock l(queue_mutex_);
            ready_.wait(l, [&] { return pending_ >= 0 || decoder_done_ || quit_; });
            if (pending_ < 0)
            {
                if (decoder_done_ || quit_)
                    break;
                continue;
            }
            slot = pending_;
            pending_ = -1;
            working_ = slot;
            epoch = slot_epochs_[slot];
        }
        try
        {
            if(auto trace=trace_.load())trace->event(1,{slot_correlations_[slot],qpc(),0,0,GetCurrentThreadId(),0,uint16_t(slot_units_[slot]),uint16_t(psx::norm::Stage::decode_enter),slot_connections_[slot]});
            if (decode_delay_ms_)
                Sleep(decode_delay_ms_);
            constexpr size_t maximum=8*1024*1024;
            if(!psx::du::transport::tcp::reserve_model(maximum)){
                std::lock_guard l(mutex_);++view_.cpu_budget_rejected;throw std::runtime_error("Aggregate client CPU payload budget exceeded");
            }
            struct Reservation{size_t bytes;std::shared_ptr<const psx::du::Frame> frame;~Reservation(){frame.reset();psx::du::transport::tcp::release_model(bytes);}};
            std::shared_ptr<Reservation> owner;
            try{owner=std::make_shared<Reservation>();}catch(...){psx::du::transport::tcp::release_model(maximum);throw;}
            owner->bytes=maximum;owner->frame=psx::du::decode(slots_[slot]);auto actual=owner->frame->bytes();
            if(actual>maximum)throw std::runtime_error("Native frame CPU bound exceeded");
            psx::du::transport::tcp::release_model(maximum-actual);owner->bytes=actual;
            auto f=std::shared_ptr<const psx::du::Frame>(owner,owner->frame.get());
            if (psx::recorder::integer(slots_[slot].data(), 72) != slot_units_[slot] ||
                f->header.sequence != slot_sequences_[slot] ||
                f->header.generation != slot_generations_[slot])
                throw std::runtime_error("Frame transport/source identity mismatch");
            std::lock_guard l(mutex_);
            if (epoch == view_.connection_epoch && enabled_ && !quit_)
            {
                if (view_.frame && view_.frame->header.sequence != acknowledged_)
                    ++view_.decoded_replaced;
                view_.frame = std::move(f);
                view_.frame_received = slot_received_[slot];
                view_.last_decoded = qpc();
                view_.same_clock=slot_same_clock_[slot];
                view_.correlation=slot_correlations_[slot];view_.connection=slot_connections_[slot];
                ++view_.accepted;
                failures_.recover(failure_record(FailureStage::decode,epoch,slot_sequences_[slot]));
                view_.status =
                    view_.frame->header.kind == 3
                        ? ((view_.frame->flags & 2)
                               ? L"Unsupported source scale/clip profile — DU cleared"
                               : L"Unsupported native assignment " +
                                     std::to_wstring(view_.frame->header.assignment) + L" — DU cleared")
                        : L"";
                if(auto trace=trace_.load())trace->event(1,{view_.correlation,qpc(),view_.frame->bytes(),view_.frame->header.end,GetCurrentThreadId(),0,uint16_t(slot_units_[slot]),uint16_t(psx::norm::Stage::decoded),view_.connection});
            }
        }
        catch (const std::exception &e)
        {
            std::lock_guard l(mutex_);
            ++view_.invalid;
            view_.status = L"Rejected drawing frame: ";
            for (const char *p = e.what(); *p; p++)
                view_.status.push_back(wchar_t(uint8_t(*p)));
            auto evidence=failure_record(FailureStage::decode,epoch,slot_sequences_[slot]);evidence.generation=slot_generations_[slot];if(slots_[slot].size()>=256)evidence.paint=psx::display::word(slots_[slot].data(),24);failures_.reject(evidence,view_.status);
        }
        {
            std::lock_guard l(queue_mutex_);
            working_ = -1;
        }
    }
    std::lock_guard l(mutex_);
    view_.stopped = true;
    decoder_finished_=true;
}
void Runtime::health(){
 // The health worker alone owns files. Disabled collection waits on control
 // events and creates no directory/file; stream and live FPS remain independent.
 HANDLE wake[]{health_stop_,health_wake_};
 for(;;){
  if(!health_enabled_){if(WaitForSingleObject(health_stop_,0)==WAIT_OBJECT_0)break;WaitForMultipleObjects(2,wake,FALSE,INFINITE);continue;}
  const auto generation=health_generation_.load();
 try{
  auto path=health_output_;
  if(path.empty()){
   wchar_t directory[32768]{};auto length=GetEnvironmentVariableW(L"LOCALAPPDATA",directory,32768);
   if(!length||length>=32768)return;
   path=std::filesystem::path(directory)/L"PSX"/L"DuClient"/(L"delivery-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(qpc())+L".jsonl");
  }
  std::filesystem::create_directories(path.parent_path());std::ofstream file(path);
  if(!file)throw std::runtime_error("DU health journal open");
  constexpr size_t limit=1024*1024,reserve=4096;
  size_t bytes{};uint64_t omitted{};bool limited{};
  auto write=[&](const std::string& line){if(bytes+line.size()<=limit-reserve){file<<line;bytes+=line.size();}else{limited=true;++omitted;}};
  write("{\"type\":\"clock\",\"frequency\":"+std::to_string(frequency())+"}\n");
  {std::lock_guard lock(mutex_);view_.diagnostics_enabled=true;}
  uint64_t warnings{},lost{},rejections{};bool failed{};std::string previous;
  for(;;){
   auto result=WaitForMultipleObjects(2,wake,FALSE,1000);
   auto stop=result==WAIT_OBJECT_0||!health_enabled_||generation!=health_generation_;
   if(generation!=health_generation_&&health_enabled_)break;
   auto v=view(false);std::array<FailureRecord,8> records;size_t count{};
   {std::lock_guard lock(mutex_);count=failures_.drain(records);}
   for(size_t n=0;n<count;++n)write(failure_json(records[n]));
   const bool native_warning=v.native.call&&qpc()>=v.native.entered&&qpc()-v.native.entered>frequency();
   uint64_t rejected{};for(auto&s:v.failures.stages)rejected+=s.rejected;
   const bool warning=native_warning||v.delivery_warnings>warnings||v.diagnostic_lost>lost||rejected>rejections||(!failed&&v.diagnostics_failed);
   std::string line="{\"validated\":"+std::to_string(v.validated)+",\"drawn\":"+std::to_string(v.drawn)+",\"delivery_over_50ms\":"+std::to_string(v.delivery_warnings)+",\"delivery_max_ms\":"+std::to_string(v.delivery_max_ms)+",\"raw_replaced\":"+std::to_string(v.raw_replaced)+",\"invalid\":"+std::to_string(v.invalid)+",\"replay_rejected\":"+std::to_string(v.replay_rejected)+",\"diagnostic_lost\":"+std::to_string(v.diagnostic_lost)+",\"failure_event_lost\":"+std::to_string(v.failures.lost)+",\"diagnostic_failed\":"+(v.diagnostics_failed?"true":"false")+",\"warning\":"+(warning?"true":"false");
   line+=",\"native_call\":"+std::to_string(v.native.call)+",\"native_entered\":"+std::to_string(v.native.entered)+",\"native_completed_calls\":"+std::to_string(v.native.completed);
   line+=",\"connected\":"+std::string(v.connected?"true":"false")+",\"source_ready\":"+(v.source_ready?"true":"false")+",\"accepted\":"+std::to_string(v.accepted)+",\"connection_epoch\":"+std::to_string(v.connection_epoch)+",\"network_attempts\":"+std::to_string(v.network_attempts)+",\"network_failures\":"+std::to_string(v.network_failures)+",\"protocol_timeouts\":"+std::to_string(v.protocol_timeouts);
   line+=",\"local_ipv4\":\""+std::string(v.local_ipv4.data())+"\",\"remote_ipv4\":\""+std::string(v.remote_ipv4.data())+"\",\"failures\":[";
   for(unsigned n=0;n<4;++n){if(n)line+=",";const auto&s=v.failures.stages[n];line+="{\"rejected\":"+std::to_string(s.rejected)+",\"recoveries\":"+std::to_string(s.recoveries)+",\"active\":"+(s.active?"true":"false")+",\"last_sequence\":"+std::to_string(s.last.sequence)+"}";}
   line+="]}\n";
   if(line!=previous){write(line);previous=line;}
   if(warning)OutputDebugStringA(("PSX DU delivery warning (stream continues): "+line).c_str());
   warnings=v.delivery_warnings;lost=v.diagnostic_lost;failed=v.diagnostics_failed;rejections=rejected;
   file.flush();if(!file)throw std::runtime_error("DU health journal write");
   if(stop)break;
  }
  file<<"{\"type\":\"journal_end\",\"limited\":"<<(limited?"true":"false")<<",\"omitted_records\":"<<omitted<<"}\n";
 }catch(...){OutputDebugStringA("PSX DU counter journal unavailable; reception continues\n");
    if(health_enabled_&&generation==health_generation_)WaitForMultipleObjects(2,wake,FALSE,INFINITE);
 }
 {std::lock_guard lock(mutex_);view_.diagnostics_enabled=false;}
 if(WaitForSingleObject(health_stop_,0)==WAIT_OBJECT_0)break;
 }
}
} // namespace du_client
