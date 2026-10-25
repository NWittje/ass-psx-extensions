#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <psx_exporter/launcher.hpp>
#include <psx_exporter/display_protocol.hpp>
#include <psx_exporter/recorder_protocol.hpp>
#include <psx_exporter/capture_writer.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <iomanip>
#include <sstream>

namespace psx::exporter::launcher {
namespace {
using namespace psx::display;
struct OwnedHandle {HANDLE h{};~OwnedHandle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}explicit OwnedHandle(HANDLE value):h(value){};};
void require_display(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::uint64_t ft(FILETIME f){return(static_cast<std::uint64_t>(f.dwHighDateTime)<<32)|f.dwLowDateTime;}
std::uint64_t qpc(){LARGE_INTEGER q{};QueryPerformanceCounter(&q);return static_cast<std::uint64_t>(q.QuadPart);}
std::uint64_t cpu(HANDLE p){FILETIME c{},e{},k{},u{};require_display(GetProcessTimes(p,&c,&e,&k,&u)!=FALSE,"display process times unavailable");return ft(k)+ft(u);}
std::wstring user_sid(){HANDLE h{};require_display(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&h)!=FALSE,"display token unavailable");OwnedHandle token(h);
    std::array<std::uint8_t,4096> data{};DWORD n{};require_display(GetTokenInformation(h,TokenUser,data.data(),static_cast<DWORD>(data.size()),&n)!=FALSE,"display SID unavailable");LPWSTR s{};
    require_display(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&s)!=FALSE,"display SID conversion");std::wstring text(s);LocalFree(s);return text;}
struct Windows {DWORD pid;std::vector<HWND> candidates;};
BOOL CALLBACK enumerate(HWND w,LPARAM p){auto& list=*reinterpret_cast<Windows*>(p);DWORD pid{};GetWindowThreadProcessId(w,&pid);wchar_t name[256]{};GetClassNameW(w,name,256);
    if(pid==list.pid&&GetWindow(w,GW_OWNER)==nullptr&&std::wstring(name)==L"SunAwtFrame")list.candidates.push_back(w);return TRUE;}
HWND select_window(const Options& o){
    if(o.display_hwnd_supplied){HWND w=reinterpret_cast<HWND>(static_cast<std::uintptr_t>(o.display_hwnd));if(!w)return nullptr;DWORD pid{};GetWindowThreadProcessId(w,&pid);
        require_display(IsWindow(w)&&pid==o.display_pid,"display window does not belong to requested process");return w;}
    Windows windows{o.display_pid,{}};EnumWindows(enumerate,reinterpret_cast<LPARAM>(&windows));
    if(windows.candidates.size()>1){std::cout<<"{\"schema\":1,\"window_candidates\":[";bool first=true;for(auto w:windows.candidates){if(!first)std::cout<<',';first=false;std::cout<<reinterpret_cast<std::uintptr_t>(w);}std::cout<<"]}\n";
        throw std::runtime_error("ambiguous PSX main window; supply --hwnd from window_candidates");}
    return windows.candidates.empty()?nullptr:windows.candidates.front();
}
struct CollectorIo {
    std::uint64_t calls{},max_ticks{},failure_ticks{};DWORD error{},requested{},completed{};const char* operation="none";
};
bool io_display(HANDLE pipe,bool write,void* data,DWORD n,CollectorIo* trace=nullptr,const char* operation="none"){DWORD offset{};const auto end=GetTickCount64()+1000,start=qpc();
    auto result=[&](DWORD error){if(trace){const auto elapsed=qpc()-start;trace->calls++;trace->max_ticks=std::max(trace->max_ticks,elapsed);if(error){trace->error=error;trace->operation=operation;trace->requested=n;trace->completed=offset;trace->failure_ticks=elapsed;}}return error==ERROR_SUCCESS;};
    while(offset<n){OwnedHandle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.h)return result(GetLastError());OVERLAPPED ov{};ov.hEvent=event.h;DWORD done{};
        const BOOL ok=write?WriteFile(pipe,static_cast<std::uint8_t*>(data)+offset,n-offset,&done,&ov):ReadFile(pipe,static_cast<std::uint8_t*>(data)+offset,n-offset,&done,&ov);
        if(!ok){const auto error=GetLastError();if(error!=ERROR_IO_PENDING)return result(error);const auto now=GetTickCount64();
            const auto waited=now>=end?WAIT_TIMEOUT:WaitForSingleObject(event.h,static_cast<DWORD>(end-now));
            if(waited!=WAIT_OBJECT_0){const auto waitError=waited==WAIT_FAILED?GetLastError():ERROR_TIMEOUT;CancelIoEx(pipe,&ov);GetOverlappedResult(pipe,&ov,&done,TRUE);return result(waitError);}
            if(!GetOverlappedResult(pipe,&ov,&done,FALSE))return result(GetLastError());}
        if(!done)return result(ERROR_HANDLE_EOF);offset+=done;
    }return result(ERROR_SUCCESS);}
void command(HANDLE pipe,std::uint32_t action,bool detail,std::uint32_t& sequence,CollectorIo* trace=nullptr){Header h;h.kind=2;h.bytes=8;h.sequence=++sequence;std::array<std::uint32_t,2> body{action,detail?1U:0U};
    require_display(io_display(pipe,true,&h,sizeof h,trace,"control_header")&&io_display(pipe,true,body.data(),8,trace,"control_payload"),"display control disconnected");}
Header receive(HANDLE pipe,std::array<std::uint8_t,maximum>& buffer,std::uint32_t& sequence,CollectorIo* trace=nullptr){Header h;
    require_display(io_display(pipe,false,&h,sizeof h,trace,"packet_header")&&h.magic_value==magic&&h.version==1&&h.bytes<=maximum&&h.sequence>sequence,"invalid display diagnostics header");
    require_display(io_display(pipe,false,buffer.data(),h.bytes,trace,"packet_payload"),"display diagnostics disconnected");sequence=h.sequence;return h;}
double percentile(std::vector<double> values,double quantile){if(values.empty())return 0;std::sort(values.begin(),values.end());return values[static_cast<std::size_t>(std::ceil(quantile*static_cast<double>(values.size()-1)))];}
// Batch explicit capture writes; expand individual artifacts after the timed interval.
class CaptureJournal {
    std::vector<char> buffer_=std::vector<char>(1024*1024);std::ofstream stream_;std::filesystem::path path_;
public:
    explicit CaptureJournal(const std::filesystem::path& directory):path_(directory/L"capture.dpj"){
        stream_.rdbuf()->pubsetbuf(buffer_.data(),static_cast<std::streamsize>(buffer_.size()));stream_.open(path_,std::ios::binary);
        const std::array<std::uint32_t,4> header{0x314a5044U,1,16,0};stream_.write(reinterpret_cast<const char*>(header.data()),sizeof header);require_display(static_cast<bool>(stream_),"DPJ1 output");
    }
    void append(std::uint32_t kind,std::uint64_t sequence,const std::vector<std::uint8_t>& data){
        std::array<std::uint8_t,16> header{};const auto size=static_cast<std::uint32_t>(data.size());std::memcpy(header.data(),&kind,4);std::memcpy(header.data()+4,&size,4);std::memcpy(header.data()+8,&sequence,8);
        stream_.write(reinterpret_cast<const char*>(header.data()),header.size());stream_.write(reinterpret_cast<const char*>(data.data()),size);require_display(static_cast<bool>(stream_),"DPJ1 capture write");
    }
    void expand(){stream_.close();require_display(!stream_.fail(),"DPJ1 capture flush");std::ifstream input(path_,std::ios::binary);std::array<std::uint32_t,4> journalHeader{};input.read(reinterpret_cast<char*>(journalHeader.data()),sizeof journalHeader);require_display(input&&journalHeader[0]==0x314a5044U&&journalHeader[1]==1&&journalHeader[2]==16,"DPJ1 header");
        std::vector<std::uint8_t> data;std::array<std::uint8_t,16> header{};while(input.peek()!=std::char_traits<char>::eof()){
            input.read(reinterpret_cast<char*>(header.data()),header.size());require_display(static_cast<bool>(input),"DPJ1 incomplete record");const auto kind=psx::recorder::integer(header.data(),0),size=psx::recorder::integer(header.data(),4);const auto sequence=word(header.data(),8);
            require_display((kind==1||kind==2)&&size>0&&size<=(kind==1?psx::recorder::frame_limit:4*1024*1024),"DPJ1 record budget");data.resize(size);input.read(reinterpret_cast<char*>(data.data()),size);require_display(static_cast<bool>(input),"DPJ1 incomplete payload");
            std::ostringstream name;name<<std::setw(12)<<std::setfill('0')<<sequence;auto destination=path_.parent_path()/(from_utf8(name.str())+(kind==1?L".dpf":L"-reference.png"));std::ofstream file(destination,std::ios::binary);file.write(reinterpret_cast<const char*>(data.data()),size);file.close();require_display(!file.fail(),"DPJ1 artifact expansion");
        }
    }
};
class CaptureStorage {
    CaptureJournal journal_;std::ofstream samples_,index_;
#if defined(PSX_CAPTURE_STORAGE_FIXTURE_STALL_MS) || defined(PSX_CAPTURE_STORAGE_FIXTURE_FAILURE)
    bool fixture_started_{};
#endif
public:
    std::uint64_t journal_calls{},max_journal_ticks{},slow_journal_calls{};const std::uint64_t frequency;
    CaptureStorage(const std::filesystem::path& directory,std::uint64_t hz):journal_(directory),samples_(directory/L"counter-samples.jsonl"),index_(directory/L"frames.jsonl"),frequency(hz){require_display(samples_&&index_,"DPF1 storage output");}
    void write(const psx::recorder::CaptureWrite& item){
#if defined(PSX_CAPTURE_STORAGE_FIXTURE_STALL_MS) || defined(PSX_CAPTURE_STORAGE_FIXTURE_FAILURE)
        if(!fixture_started_){fixture_started_=true;
#ifdef PSX_CAPTURE_STORAGE_FIXTURE_STALL_MS
            Sleep(PSX_CAPTURE_STORAGE_FIXTURE_STALL_MS);
#endif
#ifdef PSX_CAPTURE_STORAGE_FIXTURE_FAILURE
            throw std::runtime_error("authored collector disk write failure");
#endif
        }
#endif
        if(item.kind<=2){const auto start=qpc();journal_.append(item.kind,item.sequence,item.bytes);const auto ticks=qpc()-start;journal_calls++;max_journal_ticks=std::max(max_journal_ticks,ticks);slow_journal_calls+=ticks>frequency/4;}
        else {auto& stream=item.kind==3?samples_:index_;stream.write(reinterpret_cast<const char*>(item.bytes.data()),static_cast<std::streamsize>(item.bytes.size()));require_display(static_cast<bool>(stream),"DPF1 storage text write");}
    }
    void expand(){samples_.close();index_.close();require_display(!samples_.fail()&&!index_.fail(),"DPF1 storage text flush");journal_.expand();}
};
}
int decode_recorder(const Options& o){
    namespace pf=psx::recorder;std::filesystem::path input(o.recorder_input);std::vector<std::filesystem::path> files;
    if(std::filesystem::is_directory(input)){for(const auto& item:std::filesystem::directory_iterator(input))if(item.path().extension()==L".dpf")files.push_back(item.path());std::sort(files.begin(),files.end());}else files.push_back(input);
    std::uint64_t pfd=0,black=0,unavailable=0,operations=0,resources=0,bytes=0;
    for(const auto& file:files){auto size=std::filesystem::file_size(file);require_display(size>=pf::frame_header&&size<=pf::frame_limit,"DPF1 file budget");std::vector<std::uint8_t> data(static_cast<std::size_t>(size));std::ifstream stream(file,std::ios::binary);stream.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size()));require_display(static_cast<bool>(stream),"DPF1 read");auto d=pf::decode(data);pfd+=d.kind==1;black+=d.kind==2;unavailable+=d.kind==3;operations+=d.operations;resources+=d.resources;bytes+=size;}
    std::ostringstream text;text<<"{\"schema\":1,\"native_decoder\":true,\"jvm_required\":false,\"frames\":"<<files.size()<<",\"pfd\":"<<pfd<<",\"black\":"<<black<<",\"unsupported\":"<<unavailable<<",\"operations\":"<<operations<<",\"resources\":"<<resources<<",\"bytes\":"<<bytes<<"}";
    if(!o.display_output.empty()){std::ofstream output(o.display_output);require_display(static_cast<bool>(output),"DPF1 decoder report");output<<text.str()<<'\n';}std::cout<<text.str()<<'\n';return 0;
}
int collect_recorder(const Options& o){
    namespace pf=psx::recorder;OwnedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,o.display_pid));require_display(process.h!=nullptr,"recorder process unavailable");FILETIME c{},e{},k{},u{};require_display(GetProcessTimes(process.h,&c,&e,&k,&u)!=FALSE,"recorder process identity");const auto created=ft(c);auto window=select_window(o);
    const auto path=std::filesystem::absolute(o.display_output).lexically_normal();require_display(!std::filesystem::exists(path),"fresh recorder output required");
    auto endpoint=L"\\\\.\\pipe\\psx-ios.display-recorder.v1."+user_sid()+L"."+std::to_wstring(o.display_pid)+L"."+std::to_wstring(created);HANDLE raw=INVALID_HANDLE_VALUE;auto connectEnd=GetTickCount64()+5000;
    while(GetTickCount64()<connectEnd){raw=CreateFileW(endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);if(raw!=INVALID_HANDLE_VALUE)break;Sleep(20);}require_display(raw!=INVALID_HANDLE_VALUE,"DPF1 unavailable; next ordinary startup must load recorder");OwnedHandle pipe(raw);ULONG server{};require_display(GetNamedPipeServerProcessId(raw,&server)&&server==o.display_pid,"DPF1 pipe identity");
    std::array<std::uint8_t,pf::maximum> buffer{};std::uint32_t incoming=0,outgoing=0;CollectorIo ioTrace;
    auto receiveFrame=[&](){pf::Header h{};require_display(io_display(raw,false,&h,sizeof h,&ioTrace,"packet_header")&&h.magic_value==pf::magic&&h.version==1&&h.bytes<=buffer.size()&&h.sequence>incoming,"DPF1 packet header");require_display(io_display(raw,false,buffer.data(),h.bytes,&ioTrace,"packet_payload"),"DPF1 disconnected");incoming=h.sequence;return h;};
    // One byte-stream write keeps a scheduling pause from separating the control header and body.
    auto control=[&](std::uint32_t action,bool active){pf::Header h;h.kind=2;h.bytes=8;h.sequence=++outgoing;std::array<std::uint32_t,2> body{action,active?1U:0U};std::array<std::uint8_t,sizeof h+sizeof body> packet{};std::memcpy(packet.data(),&h,sizeof h);std::memcpy(packet.data()+sizeof h,body.data(),sizeof body);require_display(io_display(raw,true,packet.data(),static_cast<DWORD>(packet.size()),&ioTrace,"control"),"DPF1 control");};
    auto hello=receiveFrame();require_display(hello.kind==1&&hello.bytes>=34&&hello.bytes<=4096&&word(buffer.data(),0)==o.display_pid&&word(buffer.data(),8)==created&&word(buffer.data(),24)==1,"DPF1 source identity/ABI");const auto hz=word(buffer.data(),16);require_display(hz>0,"DPF1 QPC frequency");std::string source(reinterpret_cast<char*>(buffer.data()+32),hello.bytes-32);require_display(source.front()=='{'&&source.back()=='}',"DPF1 source metadata");
    std::filesystem::create_directories(path);std::ofstream metadata(path/L"metadata.json");require_display(static_cast<bool>(metadata),"DPF1 output");
    metadata<<"{\"schema\":1,\"pid\":"<<o.display_pid<<",\"process_creation_filetime\":"<<created<<",\"qpc_frequency\":"<<hz<<",\"hwnd\":"<<reinterpret_cast<std::uintptr_t>(window)<<",\"state_label\":"<<json_escape(utf8(o.display_state))<<",\"detail\":"<<json_escape(utf8(o.display_detail))<<",\"covered_confirmation\":"<<(o.display_state==L"covered"?"\"user label only\"":"null")<<",\"source\":"<<source<<"}\n";metadata.close();
    CaptureStorage storage(path,hz);pf::CaptureWriter<CaptureStorage> writer(storage,hz);bool active=o.display_detail==L"timing";control(1,active);auto start=qpc(),end=start+hz*o.display_duration,cpuStart=cpu(process.h),renew=start;std::uint64_t cpuEnd=0,finish=0,stopTime=0,generation=0,lastSequence=0,pfd=0,black=0,unavailable=0,png=0,outside=0,stats=0;bool stopped=false,drained=false,windowVerified=window!=nullptr;
    std::array<std::uint64_t,17> first{},last{};std::vector<double> durations,gaps,allocations,sizes;std::vector<std::uint8_t> assembly;std::uint32_t assemblingKind=0,total=0,offset=0;std::uint64_t assemblingSequence=0,assemblingGeneration=0,lastStart=0;std::string error;
    std::uint64_t lastLoop=start,maxLoopTicks=0,maxEnqueueTicks=0,slowLoopCalls=0;
    auto enqueue=[&](std::uint32_t kind,std::uint64_t sequence,const std::vector<std::uint8_t>& bytes){const auto before=qpc();require_display(writer.enqueue(pf::CaptureWrite{kind,sequence,bytes}),writer.failed()?"DPF1 capture storage worker failed":"DPF1 capture storage queue overflow");maxEnqueueTicks=std::max(maxEnqueueTicks,qpc()-before);};
    auto enqueueText=[&](std::uint32_t kind,const std::string& text){const auto before=qpc();require_display(writer.enqueue(pf::CaptureWrite{kind,0,std::vector<std::uint8_t>(text.begin(),text.end())}),writer.failed()?"DPF1 capture storage worker failed":"DPF1 capture storage queue overflow");maxEnqueueTicks=std::max(maxEnqueueTicks,qpc()-before);};
    try{while(!stopped||(!drained&&qpc()<stopTime+hz)){
        auto now=qpc();require_display(!writer.failed(),"DPF1 capture storage worker failed");const auto loopTicks=now-lastLoop;lastLoop=now;maxLoopTicks=std::max(maxLoopTicks,loopTicks);slowLoopCalls+=loopTicks>hz/4;if(!stopped&&now>=end){finish=now;cpuEnd=cpu(process.h);control(3,false);stopped=true;stopTime=now;}if(!stopped&&now>=renew){control(2,active);renew=now+hz/2;}
        DWORD available{};const auto peeked=PeekNamedPipe(raw,nullptr,0,nullptr,&available,nullptr);if(!peeked){ioTrace.error=GetLastError();ioTrace.operation="reader_peek";ioTrace.requested=ioTrace.completed=0;ioTrace.failure_ticks=0;}require_display(peeked!=FALSE,"DPF1 reader lost");if(!available){Sleep(2);continue;}auto h=receiveFrame();require_display(h.kind==3&&pf::valid(buffer.data(),h.bytes),"DPF1 chunk validation");auto kind=pf::integer(buffer.data(),0);
        if(kind==3){for(std::size_t i=0;i<17;i++)last[i]=word(buffer.data(),16+i*8);if(stats++==0)first=last;const bool exists=window&&IsWindow(window),iconic=exists&&IsIconic(window);windowVerified=windowVerified&&exists&&(o.display_state==L"minimized"?iconic:!iconic);
            std::ostringstream sample;sample<<"{\"qpc\":"<<now<<",\"cpu_100ns\":"<<cpu(process.h)<<",\"iconic\":"<<(iconic?"true":"false")<<",\"window_available\":"<<(exists?"true":"false")<<",\"generation\":"<<word(buffer.data(),8)<<",\"counters\":[";for(std::size_t i=0;i<17;i++){if(i)sample<<',';sample<<last[i];}sample<<"]}\n";enqueueText(3,sample.str());drained=stopped&&word(buffer.data(),8)==0&&last[7]==0&&offset==0;continue;}
        auto frameSequence=word(buffer.data(),8),frameGeneration=word(buffer.data(),16);auto fragmentOffset=pf::integer(buffer.data(),24),part=pf::integer(buffer.data(),28),size=pf::integer(buffer.data(),4);
        if(!generation)generation=frameGeneration;require_display(frameGeneration==generation,"DPF1 capture generation changed");
        if(offset==0){require_display(fragmentOffset==0,"DPF1 initial fragment");assemblingKind=kind;assemblingSequence=frameSequence;assemblingGeneration=frameGeneration;total=size;assembly.resize(size);}
        require_display(kind==assemblingKind&&frameSequence==assemblingSequence&&frameGeneration==assemblingGeneration&&size==total&&fragmentOffset==offset,"DPF1 chunk ordering");std::memcpy(assembly.data()+offset,buffer.data()+32,part);offset+=part;if(offset!=total)continue;
        if(kind==1){auto d=pf::decode(assembly);require_display(d.sequence==frameSequence&&d.generation==frameGeneration&&d.sequence>lastSequence&&d.frequency==hz,"DPF1 assembled frame identity");lastSequence=d.sequence;
            if(d.start>=start&&d.start<=end){pfd+=d.kind==1;black+=d.kind==2;unavailable+=d.kind==3;if(d.kind==1){if(lastStart)gaps.push_back(static_cast<double>(d.start-lastStart)*1000/static_cast<double>(hz));lastStart=d.start;durations.push_back(static_cast<double>(d.capture_ticks)*1000/static_cast<double>(hz));allocations.push_back(static_cast<double>(d.allocated));sizes.push_back(static_cast<double>(assembly.size()));}}else outside++;
            enqueue(1,frameSequence,assembly);
            std::ostringstream index;double scale{};std::memcpy(&scale,assembly.data()+192,8);index<<"{\"sequence\":"<<d.sequence<<",\"paint\":"<<d.paint<<",\"start_qpc\":"<<d.start<<",\"end_qpc\":"<<d.end<<",\"kind\":"<<d.kind<<",\"assignment\":"<<d.assignment<<",\"epoch\":"<<d.epoch<<",\"bytes\":"<<assembly.size()<<",\"operations\":"<<d.operations<<",\"resources\":"<<d.resources<<",\"source_scale\":"<<scale<<",\"capture_ticks\":"<<d.capture_ticks<<",\"source_allocated_bytes\":"<<d.allocated<<",\"native_drawing_primitives\":"<<pf::integer(assembly.data(),156)<<"}\n";enqueueText(4,index.str());
        }else {require_display(assembly.size()>=8&&assembly[0]==137&&assembly[1]=='P'&&assembly[2]=='N'&&assembly[3]=='G',"DPF1 PNG");enqueue(2,frameSequence,assembly);png++;}
        offset=0;require_display(WaitForSingleObject(process.h,0)==WAIT_TIMEOUT,"DPF1 process exited");
    }require_display(drained,"DPF1 final drain incomplete");}catch(const std::exception& failure){error=failure.what();}
    if(!finish){finish=qpc();cpuEnd=cpu(process.h);}CloseHandle(pipe.h);pipe.h=INVALID_HANDLE_VALUE;const auto flushStart=qpc();writer.finish();const auto flushTicks=qpc()-flushStart;const auto& storageStats=writer.stats();const bool ownershipDrained=writer.owned_bytes()==0&&writer.owned_entries()==0&&storageStats.submitted==storageStats.written+storageStats.discarded;if(writer.failed()&&error.empty())error=writer.error();if(error.empty()&&(!ownershipDrained||storageStats.discarded||storageStats.overflows))error="DPF1 capture storage drain/accounting failure";try{storage.expand();}catch(const std::exception& failure){if(error.empty())error=failure.what();}double elapsed=static_cast<double>(finish-start)/static_cast<double>(hz);std::ofstream summary(path/L"summary.json");require_display(static_cast<bool>(summary),"DPF1 summary");
    summary<<"{\"schema\":1,\"status\":"<<json_escape(error.empty()?"collected":"incomplete")<<",\"error\":"<<json_escape(error)<<",\"elapsed_seconds\":"<<elapsed<<",\"nominal_interval_seconds\":"<<o.display_duration<<",\"collector_stop_delay_ms\":"<<static_cast<double>(finish>end?finish-end:0)*1000/static_cast<double>(hz)<<",\"counter_interval_includes_drain\":true,\"window_state_verified\":"<<(windowVerified?"true":"false")<<",\"completed_pfd_frames\":"<<pfd<<",\"black_frames\":"<<black<<",\"unsupported_frames\":"<<unavailable<<",\"outside_interval_frames\":"<<outside<<",\"reference_images\":"<<png<<",\"final_drain\":"<<(drained?"true":"false")<<",\"pfd_hz\":"<<static_cast<double>(pfd)/static_cast<double>(o.display_duration)<<",\"cpu_percent_one_core\":"<<static_cast<double>(cpuEnd-cpuStart)*0.00001/elapsed<<",\"capture_ms\":{\"p50\":"<<percentile(durations,.5)<<",\"p95\":"<<percentile(durations,.95)<<",\"p99\":"<<percentile(durations,.99)<<",\"max\":"<<percentile(durations,1)<<"},\"frame_gap_ms\":{\"p50\":"<<percentile(gaps,.5)<<",\"p99\":"<<percentile(gaps,.99)<<",\"max\":"<<percentile(gaps,1)<<"},\"allocated_bytes_per_captured_native_call\":{\"p50\":"<<percentile(allocations,.5)<<",\"p99\":"<<percentile(allocations,.99)<<"},\"frame_bytes\":{\"p50\":"<<percentile(sizes,.5)<<",\"max\":"<<percentile(sizes,1)<<"},\"counter_deltas\":[";for(std::size_t i=0;i<17;i++){if(i)summary<<',';summary<<last[i]-first[i];}summary<<"],\"partial_chunk_bytes\":"<<offset<<",\"reference_replay_qualified\":false}\n";
    std::ofstream diagnostics(path/L"collector-diagnostics.json");require_display(static_cast<bool>(diagnostics),"DPF1 collector diagnostics");
    diagnostics<<"{\"schema\":1,\"storage_records_submitted\":"<<storageStats.submitted<<",\"storage_ownership_drained\":"<<(ownershipDrained?"true":"false")<<",\"storage_final_owned_capacity_bytes\":"<<writer.owned_bytes()<<",\"storage_final_owned_entries\":"<<writer.owned_entries()<<",";
    diagnostics<<"\"qpc_frequency\":"<<hz<<",\"io_calls\":"<<ioTrace.calls<<",\"io_max_ms\":"<<static_cast<double>(ioTrace.max_ticks)*1000/hz<<",\"failure_operation\":"<<json_escape(ioTrace.operation)<<",\"failure_win32_error\":"<<ioTrace.error<<",\"failure_requested_bytes\":"<<ioTrace.requested<<",\"failure_completed_bytes\":"<<ioTrace.completed<<",\"failure_byte_count_excludes_failed_or_cancelled_io\":true,\"failure_ms\":"<<static_cast<double>(ioTrace.failure_ticks)*1000/hz<<",\"journal_calls\":"<<storage.journal_calls<<",\"journal_append_max_ms\":"<<static_cast<double>(storage.max_journal_ticks)*1000/hz<<",\"journal_appends_over_250ms\":"<<storage.slow_journal_calls<<",\"loop_gap_max_ms\":"<<static_cast<double>(maxLoopTicks)*1000/hz<<",\"loop_gaps_over_250ms\":"<<slowLoopCalls<<",\"control_single_write\":true,\"journal_worker\":true,\"storage_queue_byte_limit\":"<<pf::CaptureWriter<CaptureStorage>::byte_limit<<",\"storage_queue_entry_limit\":"<<pf::CaptureWriter<CaptureStorage>::entry_limit<<",\"storage_peak_owned_capacity_bytes\":"<<storageStats.peak_bytes<<",\"storage_peak_entries\":"<<storageStats.peak_entries<<",\"storage_queue_overflows\":"<<storageStats.overflows<<",\"storage_records_written\":"<<storageStats.written<<",\"storage_records_discarded\":"<<storageStats.discarded<<",\"storage_error\":"<<json_escape(writer.error())<<",\"storage_enqueue_max_ms\":"<<static_cast<double>(maxEnqueueTicks)*1000/hz<<",\"storage_write_max_ms\":"<<static_cast<double>(storageStats.max_write_ticks)*1000/hz<<",\"storage_writes_over_250ms\":"<<storageStats.writes_over_250ms<<",\"storage_finish_after_source_disconnect_ms\":"<<static_cast<double>(flushTicks)*1000/hz<<"}\n";
    std::cout<<"{\"operation\":\"display-record\",\"status\":"<<json_escape(error.empty()?"collected":"incomplete")<<",\"pfd_frames\":"<<pfd<<",\"unsupported_frames\":"<<unavailable<<",\"output\":"<<json_escape(utf8(path.wstring()))<<"}\n";return error.empty()?0:2;
}
int collect_display(const Options& o){
    OwnedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,o.display_pid));require_display(process.h!=nullptr,"display process unavailable");
    FILETIME c{},e{},k{},u{};require_display(GetProcessTimes(process.h,&c,&e,&k,&u)!=FALSE,"display process identity unavailable");const auto created=ft(c);
    const auto window=select_window(o);const auto path=std::filesystem::absolute(o.display_output).lexically_normal();
    require_display(!std::filesystem::exists(path),"display output already exists; choose a fresh directory");
    const auto endpoint=L"\\\\.\\pipe\\psx-ios.display-observer.v1."+user_sid()+L"."+std::to_wstring(o.display_pid)+L"."+std::to_wstring(created);
    HANDLE raw=INVALID_HANDLE_VALUE;const auto connectionEnd=GetTickCount64()+5000;
    while(GetTickCount64()<connectionEnd){raw=CreateFileW(endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);if(raw!=INVALID_HANDLE_VALUE)break;Sleep(50);}
    require_display(raw!=INVALID_HANDLE_VALUE,"display observer unavailable; ordinary startup with a qualified exporter is required");OwnedHandle pipe(raw);
    ULONG server{};require_display(GetNamedPipeServerProcessId(raw,&server)!=FALSE&&server==o.display_pid,"display pipe process identity disagreement");
    std::array<std::uint8_t,maximum> buffer{};std::uint32_t incoming=0,outgoing=0;CollectorIo ioTrace;auto hello=receive(raw,buffer,incoming,&ioTrace);
    require_display(hello.kind==1&&hello.bytes>=34&&hello.bytes<=4096&&word(buffer.data(),0)==o.display_pid&&word(buffer.data(),8)==created&&word(buffer.data(),24)==1,"display observer hello identity/ABI");
    const auto hz=word(buffer.data(),16);require_display(hz>0,"display QPC frequency");
    const auto connected=qpc();std::uint64_t anchored=connected;double connectionDelay=0;
    if(o.display_startup_created){require_display(o.display_startup_created==created,"startup process creation identity changed");FILETIME wall{};GetSystemTimePreciseAsFileTime(&wall);const auto wallNow=ft(wall);require_display(wallNow>=created,"startup wall clock moved backwards");connectionDelay=static_cast<double>(wallNow-created)/10000000.0;const auto elapsed=static_cast<std::uint64_t>(connectionDelay*static_cast<double>(hz));require_display(connected>=elapsed,"startup QPC anchor range");anchored=connected-elapsed;}
    std::string source(reinterpret_cast<const char*>(buffer.data()+32),hello.bytes-32);
    require_display(source.front()=='{'&&source.back()=='}',"display metadata shape");
    std::filesystem::create_directories(path);std::ofstream metadata(path/L"metadata.json"),samples(path/L"counter-samples.jsonl"),records(path/L"paint-records.bin",std::ios::binary);
    require_display(metadata&&samples&&records,"display output creation failed");
    metadata<<"{\"schema\":1,\"pid\":"<<o.display_pid<<",\"process_creation_filetime\":"<<created<<",\"qpc_frequency\":"<<hz<<",\"hwnd\":"<<reinterpret_cast<std::uintptr_t>(window)
        <<",\"state_label\":"<<json_escape(utf8(o.display_state))<<",\"covered_confirmation\":"<<(o.display_state==L"covered"?"\"user_label\"":"null")<<",\"detail\":"<<json_escape(utf8(o.display_detail))
        <<",\"duration_seconds\":"<<o.display_duration<<",\"startup_anchored\":"<<(o.display_startup_created?"true":"false")<<",\"creation_qpc_estimate\":"<<anchored<<",\"connection_delay_seconds\":"<<connectionDelay<<",\"missing_early_detail_seconds\":"<<connectionDelay<<",\"record_bytes\":1024,\"snapshot_bytes\":2048,\"source\":"<<source<<"}\n";metadata.close();
    const bool detail=o.display_detail==L"timing";command(raw,1,detail,outgoing,&ioTrace);
    const auto cpuBegin=qpc(),start=o.display_startup_created?anchored:cpuBegin,cpuStart=cpu(process.h),end=start+hz*o.display_duration;auto nextRenew=qpc();
    std::array<std::uint64_t,128> first{},last{};bool haveFirst=false,windowVerified=window!=nullptr;std::uint64_t accepted=0,stale=0,expectedGeneration=0,lastSequence=0,firstPaint=0,lastPaint=0,sourceFailure=0,firstReject=0,lastReject=0,firstLost=0,lastLost=0;
    std::array<std::uint64_t,6> unitFrames{},unitBodies{};std::vector<double> durations,gaps;std::string error;std::uint64_t nativeSnapshots=0,outsideWindow=0,finish=0,cpuFinish=0,drainDeadline=0,activeAbandoned=0;
    bool stopped=false,drained=false;
    std::uint64_t lastLoop=cpuBegin,maxLoopTicks=0,maxStorageTicks=0,stopControlTicks=0,drainFinished=0;
    try{
        while(!stopped||(!drained&&qpc()<drainDeadline)){
            const auto now=qpc();maxLoopTicks=std::max(maxLoopTicks,now-lastLoop);lastLoop=now;
            if(!stopped&&now>=end){finish=now;cpuFinish=cpu(process.h);activeAbandoned=last[23]-first[23];command(raw,3,false,outgoing,&ioTrace);stopControlTicks=qpc()-now;stopped=true;drainDeadline=now+hz;}
            if(!stopped&&now>=nextRenew){command(raw,2,detail,outgoing,&ioTrace);nextRenew=now+hz/2;}
            DWORD available{};require_display(PeekNamedPipe(raw,nullptr,0,nullptr,&available,nullptr)!=FALSE,"display peer lost");
            if(available){auto h=receive(raw,buffer,incoming,&ioTrace);require_display(h.kind==3&&valid(buffer.data(),h.bytes),"invalid display snapshot");nativeSnapshots++;const auto received=qpc();
                const auto capture=word(buffer.data(),48);if(detail&&capture){if(!expectedGeneration)expectedGeneration=capture;require_display(expectedGeneration==capture,"display capture generation changed");}
                sourceFailure=word(buffer.data(),40);lastReject=word(buffer.data(),112);lastLost=word(buffer.data(),136);
                for(std::size_t i=0;i<128;i++)last[i]=word(buffer.data(),512+i*8);
                if(!haveFirst){first=last;firstReject=lastReject;firstLost=lastLost;haveFirst=true;}
                const bool availableWindow=window&&IsWindow(window);const bool iconic=availableWindow&&IsIconic(window);const bool stateMatch=availableWindow&&(o.display_state==L"minimized"?iconic:!iconic);windowVerified=windowVerified&&stateMatch;
                RECT rect{};if(availableWindow)GetWindowRect(window,&rect);
                const auto storageStart=qpc();
                samples<<"{\"collector_qpc\":"<<now<<",\"receipt_qpc\":"<<received<<",\"source_qpc\":"<<word(buffer.data(),104)<<",\"cpu_100ns\":"<<cpu(process.h)<<",\"window_available\":"<<(availableWindow?"true":"false")
                    <<",\"iconic\":"<<(iconic?"true":"false")<<",\"window_rect\":["<<rect.left<<','<<rect.top<<','<<rect.right<<','<<rect.bottom<<"],\"generation\":"<<capture<<",\"source_failure\":"<<sourceFailure
                    <<",\"unfinished_stage\":"<<static_cast<std::int64_t>(word(buffer.data(),88))<<",\"unfinished_stage_qpc\":"<<word(buffer.data(),96)<<",\"queue_write\":"<<word(buffer.data(),16)<<",\"queue_read\":"<<word(buffer.data(),24)<<",\"batch_records\":"<<word(buffer.data(),128)<<",\"queue_overflow\":"<<word(buffer.data(),32)<<",\"native_batch_rejections\":"<<lastReject<<",\"transport_records_lost\":"<<lastLost<<",\"counters\":[";
                for(std::size_t i=0;i<128;i++){if(i)samples<<',';samples<<last[i];}samples<<"]}\n";
                const auto count=word(buffer.data(),128);
                drained=stopped&&capture==0&&word(buffer.data(),16)==word(buffer.data(),24)&&count==0;
                for(std::uint64_t i=0;i<count;i++){const auto p=buffer.data()+snapshot_bytes+static_cast<std::size_t>(i)*record_bytes;
                    if(!detail||!expectedGeneration||word(p,0)!=expectedGeneration){stale++;continue;}
                    const auto sequence=word(p,8),a=word(p,32),b=word(p,40);require_display(sequence>lastSequence&&b>=a&&a>0,"display paint record order/time");lastSequence=sequence;
                    if(a<start||a>end){outsideWindow++;continue;}
                    if(lastPaint)gaps.push_back(static_cast<double>(a-lastPaint)*1000/static_cast<double>(hz));else firstPaint=a;
                    lastPaint=a;durations.push_back(static_cast<double>(b-a)*1000/static_cast<double>(hz));accepted++;
                    for(std::size_t unit=0;unit<6;unit++){const auto base=32+unit*14;if(word(p,(base+4)*8))unitFrames[unit]++;if(word(p,(base+6)*8))unitBodies[unit]++;}
                    records.write(reinterpret_cast<const char*>(p),record_bytes);
                }
                require_display(samples&&records,"display output write failed");maxStorageTicks=std::max(maxStorageTicks,qpc()-storageStart);
                if(drained)drainFinished=qpc();
            }else Sleep(10);
            require_display(WaitForSingleObject(process.h,0)==WAIT_TIMEOUT,"display process exited");
        }
        require_display(drained,"display final diagnostic drain incomplete");
    }catch(const std::exception& failure){error=failure.what();}
    if(!finish){finish=qpc();cpuFinish=cpu(process.h);activeAbandoned=last[23]-first[23];}
    const double elapsed=static_cast<double>(finish-start)/static_cast<double>(hz);
    std::ofstream summary(path/L"summary.json");require_display(static_cast<bool>(summary),"display summary creation failed");
    summary<<"{\"schema\":1,\"status\":"<<json_escape(error.empty()?"collected":"incomplete")<<",\"error\":"<<json_escape(error)<<",\"elapsed_seconds\":"<<elapsed
        <<",\"window_state_verified\":"<<(windowVerified?"true":"false")<<",\"covered_state_evidence\":"<<(o.display_state==L"covered"?"\"user label only\"":"null")<<",\"snapshots\":"<<nativeSnapshots<<",\"paint_records\":"<<accepted<<",\"stale_records\":"<<stale
        <<",\"outside_collection_window_records\":"<<outsideWindow<<",\"final_drain_completed\":"<<(drained?"true":"false")<<",\"detail_abandoned_during_collection_delta\":"<<activeAbandoned
        <<",\"pipe_io_max_ms\":"<<static_cast<double>(ioTrace.max_ticks)*1000/hz<<",\"loop_gap_max_ms\":"<<static_cast<double>(maxLoopTicks)*1000/hz<<",\"storage_batch_max_ms\":"<<static_cast<double>(maxStorageTicks)*1000/hz<<",\"stop_control_ms\":"<<static_cast<double>(stopControlTicks)*1000/hz<<",\"stop_to_drain_ms\":"<<(drainFinished?static_cast<double>(drainFinished-finish)*1000/hz:0)
        <<",\"source_failure\":"<<sourceFailure<<",\"queue_overflow_delta\":"<<(last[21]-first[21])<<",\"native_batch_rejections_delta\":"<<(lastReject-firstReject)<<",\"transport_records_lost_delta\":"<<(lastLost-firstLost)
        <<",\"cpu_percent_one_core\":"<<(static_cast<double>(cpuFinish-cpuStart)*0.00001/(static_cast<double>(finish-cpuBegin)/static_cast<double>(hz)))<<",\"paint_hz\":"<<(accepted>1&&lastPaint>firstPaint?static_cast<double>(accepted-1)*static_cast<double>(hz)/static_cast<double>(lastPaint-firstPaint):0)
        <<",\"paint_duration_ms\":{\"p50\":"<<percentile(durations,.5)<<",\"p95\":"<<percentile(durations,.95)<<",\"p99\":"<<percentile(durations,.99)<<",\"max\":"<<percentile(durations,1)<<"},\"paint_start_gap_ms\":{\"p50\":"<<percentile(gaps,.5)<<",\"p95\":"<<percentile(gaps,.95)<<",\"p99\":"<<percentile(gaps,.99)<<",\"max\":"<<percentile(gaps,1)<<"},\"physical_units\":[";
    const char* names[]={"captain_outboard","captain_inboard","first_officer_inboard","first_officer_outboard","eicas_upper","eicas_lower"};
    for(std::size_t unit=0;unit<6;unit++){if(unit)summary<<',';summary<<"{\"id\":"<<unit<<",\"name\":\""<<names[unit]<<"\",\"paints_with_renderer_entry\":"<<unitFrames[unit]<<",\"paints_reaching_body\":"<<unitBodies[unit]<<",\"body_dispatch_hz_over_collection\":"<<static_cast<double>(unitBodies[unit])/elapsed<<'}';}
    summary<<"],\"counter_deltas\":[";for(std::size_t i=0;i<128;i++){if(i)summary<<',';summary<<last[i]-first[i];}
    summary<<"],\"fresh_pixels_proven\":false,\"complete_frames_proven\":false}\n";summary.close();samples.close();records.close();
    std::cout<<"{\"schema\":1,\"operation\":\"display-probe\",\"status\":"<<json_escape(error.empty()?"collected":"incomplete")<<",\"output\":"<<json_escape(utf8(path.wstring()))<<",\"paint_records\":"<<accepted<<"}\n";
    return error.empty()&&haveFirst&&sourceFailure==0&&last[19]==first[19]?0:2;
}
}
