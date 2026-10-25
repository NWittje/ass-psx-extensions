#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <bcrypt.h>
#include <jni.h>
#include <psx/exporter/protocol.hpp>
#include <psx_exporter/audio_runtime.hpp>
#include <psx_exporter/terrain_runtime.hpp>
#include <psx_exporter/aircraft_runtime.hpp>
#include <psx_exporter/environment_runtime.hpp>
#include <psx_exporter/status_runtime.hpp>
#include <psx_exporter/traffic_runtime.hpp>
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include "../../../shared/windows-runtime/single_instance.hpp"

namespace {
using namespace psx;
constexpr std::size_t capacity=128;
struct Runtime {
    std::atomic<bool> initialized{},active{},wake_posted{};
    HANDLE wake{};
    exporter::Hello hello{};
    std::wstring endpoint;
    alignas(64) std::atomic<std::uint64_t> write{};
    alignas(64) std::atomic<std::uint64_t> read{};
    std::array<motion::RawPose,capacity> slots{};
    std::atomic<std::uint64_t> sequence{},enqueued{},sent{},queue_dropped{},transport_dropped{};
    std::atomic<std::uint64_t> attempted{},rejected{},pose_epoch{};
    std::atomic<std::uint32_t> flags{},audio_groups{},suppression_groups{};
    std::array<psx_exporter::SampleRing<audio::Snapshot,16>,8> audio_slots{};
    std::atomic<std::uint64_t> audio_enqueued{},audio_sent{},audio_dropped{},configuration{};
    std::atomic<bool> audio_paused{};
    std::atomic<std::int64_t> audio_state_time{};
    psx_exporter::OutputLease output_lease;

};
Runtime runtime;
psx::windows::SingleInstance runtime_instance;
std::atomic<int> instance_state{};
bool claim_runtime() noexcept {
    int expected=0;
    if(!instance_state.compare_exchange_strong(expected,1,std::memory_order_acq_rel))return expected==2;
    const bool primary=runtime_instance.acquire(L"Local\\PSX-IOS.ExporterRuntime")==psx::windows::InstanceResult::primary;
    instance_state.store(primary?2:3,std::memory_order_release);
    return primary;
}
std::int64_t ticks() noexcept {LARGE_INTEGER v{};QueryPerformanceCounter(&v);return v.QuadPart;}
std::uint64_t filetime(FILETIME t) noexcept {return (static_cast<std::uint64_t>(t.dwHighDateTime)<<32)|t.dwLowDateTime;}
bool identify() {
    HANDLE token{};if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;
    DWORD needed{};GetTokenInformation(token,TokenUser,nullptr,0,&needed);
    std::array<unsigned char,4096> storage{};
    if(needed>storage.size()||!GetTokenInformation(token,TokenUser,storage.data(),static_cast<DWORD>(storage.size()),&needed)){CloseHandle(token);return false;}
    CloseHandle(token);LPWSTR sid{};if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid,&sid))return false;
    FILETIME created{},exit{},kernel{},user{};const auto times=GetProcessTimes(GetCurrentProcess(),&created,&exit,&kernel,&user);
    if(!times){LocalFree(sid);return false;}
    runtime.hello.pid=GetCurrentProcessId();runtime.hello.process_creation_filetime=filetime(created);
    runtime.endpoint=L"\\\\.\\pipe\\psx-ios.exporter.v1."+std::wstring(sid)+L"."+std::to_wstring(runtime.hello.pid)+L"."+std::to_wstring(runtime.hello.process_creation_filetime);LocalFree(sid);
    LARGE_INTEGER frequency{};if(!QueryPerformanceFrequency(&frequency)||frequency.QuadPart<=0)return false;
    runtime.hello.qpc_frequency=static_cast<std::uint64_t>(frequency.QuadPart);
    if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&runtime.hello.producer),sizeof(runtime.hello.producer),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0||!motion::valid(runtime.hello.producer))return false;
    constexpr char build[]="psx-exporter-native-" PSX_EXPORTER_VERSION;static_assert(sizeof(build)<=runtime.hello.build.size());std::memcpy(runtime.hello.build.data(),build,sizeof(build));return true;
}
struct Operation {
    OVERLAPPED overlapped{};bool pending{};DWORD completed{};
    Operation(){overlapped.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);}
    ~Operation(){if(overlapped.hEvent)CloseHandle(overlapped.hEvent);}
    Operation(const Operation&)=delete;Operation& operator=(const Operation&)=delete;
    bool begin(HANDLE pipe,bool write,void* buffer,DWORD size) noexcept {
        ResetEvent(overlapped.hEvent);completed=0;
        const auto result=write?WriteFile(pipe,buffer,size,&completed,&overlapped):ReadFile(pipe,buffer,size,&completed,&overlapped);
        pending=!result&&GetLastError()==ERROR_IO_PENDING;
        return result||pending;
    }
    bool finish(HANDLE pipe) noexcept {
        if(!pending)return true;
        if(!GetOverlappedResult(pipe,&overlapped,&completed,FALSE))return false;
        pending=false;return true;
    }
    bool cancel(HANDLE pipe) noexcept {
        if(pending){CancelIoEx(pipe,&overlapped);const bool done=GetOverlappedResult(pipe,&overlapped,&completed,TRUE)!=FALSE;pending=false;return done;}
        return true;
    }
};
bool write_frame(HANDLE pipe,exporter::Frame& frame) noexcept {
    Operation op;if(!op.overlapped.hEvent)return false;
    std::size_t offset=0;const auto deadline=GetTickCount64()+5000;
    while(offset<frame.size){
        if(!op.begin(pipe,true,frame.bytes.data()+offset,static_cast<DWORD>(frame.size-offset)))return false;
        if(op.pending){const auto now=GetTickCount64();if(now>=deadline||WaitForSingleObject(op.overlapped.hEvent,static_cast<DWORD>(deadline-now))!=WAIT_OBJECT_0){op.cancel(pipe);return false;}if(!op.finish(pipe)){op.cancel(pipe);return false;}}
        if(!op.completed)return false;offset+=op.completed;
    }return true;
}
struct Subscription {bool received{};std::uint64_t owner{};};
bool subscription_frame(void* context,const exporter::Header& header,const std::uint8_t* data,std::size_t size) {
    auto& state=*static_cast<Subscription*>(context);exporter::Subscribe s;
    if(state.received||header.kind!=exporter::Kind::subscribe||!exporter::decode(data,size,s)||s.producer!=runtime.hello.producer||s.qpc_frequency!=runtime.hello.qpc_frequency)return false;
    state.received=true;state.owner=s.coordinator_instance;return true;
}
bool subscribe(HANDLE pipe,Subscription& state) noexcept {
    exporter::Framer framer;std::array<std::uint8_t,512> buffer{};Operation op;
    if(!op.overlapped.hEvent)return false;const auto deadline=GetTickCount64()+5000;
    while(!state.received){
        if(!op.begin(pipe,false,buffer.data(),static_cast<DWORD>(buffer.size())))return false;
        if(op.pending){const auto now=GetTickCount64();if(now>=deadline||WaitForSingleObject(op.overlapped.hEvent,static_cast<DWORD>(deadline-now))!=WAIT_OBJECT_0){op.cancel(pipe);return false;}if(!op.finish(pipe)){op.cancel(pipe);return false;}}
        if(!op.completed||!framer.feed(buffer.data(),op.completed,subscription_frame,&state))return false;
    }
    return framer.buffered()==0;
}
bool pop(motion::RawPose& out) noexcept {
    const auto r=runtime.read.load(std::memory_order_relaxed),w=runtime.write.load(std::memory_order_acquire);
    if(r==w)return false;out=runtime.slots[r%capacity];runtime.read.store(r+1,std::memory_order_release);return true;
}
void discard_queue() noexcept {
    motion::RawPose ignored;while(pop(ignored))runtime.transport_dropped.fetch_add(1,std::memory_order_relaxed);
}
struct AudioPeer {
    std::uint64_t owner{},last_operation{},epoch{};
    std::array<audio::Lease,16> pending{};std::size_t head{},count{};
    audio::LeaseAck overflow{};bool overflow_pending{};
};
bool audio_command(void* context,const exporter::Header& header,const std::uint8_t* bytes,std::size_t size) {
    auto& peer=*static_cast<AudioPeer*>(context);audio::Lease lease;
    if(header.version!=2||header.kind!=exporter::Kind::audio_lease||!exporter::decode(bytes,size,lease)||lease.owner!=peer.owner)return false;
    if(peer.count==peer.pending.size()){
        runtime.output_lease.set(0,0);peer.head=peer.count=0;
        peer.overflow={lease,0,audio::Reason::overflow};peer.overflow_pending=true;
    }else {peer.pending[(peer.head+peer.count)%peer.pending.size()]=lease;++peer.count;}
    return true;
}
bool service(HANDLE pipe) noexcept {
    exporter::Frame output;std::uint32_t sequence=1;Subscription subscription;
    runtime.hello.audio_groups=runtime.audio_groups.load();runtime.hello.suppression_groups=runtime.suppression_groups.load();
    if(!exporter::encode(runtime.hello,sequence++,output)||!write_frame(pipe,output)||!subscribe(pipe,subscription))return false;
    discard_queue();const auto subscribed_at=ticks();runtime.active.store(true,std::memory_order_release);
    std::array<audio::Snapshot,8> last_audio{};
    for(auto& ring:runtime.audio_slots){audio::Snapshot ignored;while(ring.pop(ignored))runtime.audio_dropped.fetch_add(1);}
    Operation read,write;std::array<std::uint8_t,1024> incoming{};
    if(!read.overlapped.hEvent||!write.overlapped.hEvent){runtime.active.store(false);return false;}
    if(!read.begin(pipe,false,incoming.data(),static_cast<DWORD>(incoming.size()))){runtime.active.store(false);return false;}
    AudioPeer peer;peer.owner=subscription.owner;exporter::Framer command_framer;
    bool okay=true,writing=false,snapshot=false,audio_snapshot=false,waiting_ack=false;
    audio::LeaseAck ack;std::uint64_t awaited_ticket{};
    std::size_t offset=0,lane=0,held_group=0;ULONGLONG health_at=0,write_progress=0,held_at=0;
    while(okay){
        runtime.wake_posted.store(false,std::memory_order_release);
        if(!read.pending||WaitForSingleObject(read.overlapped.hEvent,0)==WAIT_OBJECT_0){
            if(!read.finish(pipe)||!read.completed||!command_framer.feed(incoming.data(),read.completed,audio_command,&peer)){okay=false;break;}
            if(!read.begin(pipe,false,incoming.data(),static_cast<DWORD>(incoming.size()))){okay=false;break;}
        }
        if(writing&&GetTickCount64()-write_progress>=5000){okay=false;break;}
        if(writing&&(!write.pending||WaitForSingleObject(write.overlapped.hEvent,0)==WAIT_OBJECT_0)){
            if(!write.finish(pipe)||!write.completed){okay=false;break;}
            offset+=write.completed;write_progress=GetTickCount64();
            if(offset<output.size){if(!write.begin(pipe,true,output.bytes.data()+offset,static_cast<DWORD>(output.size-offset))){okay=false;break;}}
            else {writing=false;if(snapshot)runtime.sent.fetch_add(1);if(audio_snapshot)runtime.audio_sent.fetch_add(1);}
        }
        if(!writing){
            bool have=false;snapshot=false;audio_snapshot=false;const auto now=GetTickCount64();const auto qpc=ticks();
            if(peer.overflow_pending){have=exporter::encode(peer.overflow,sequence++,output);peer.overflow_pending=false;waiting_ack=false;}
            if(!have&&!waiting_ack&&peer.count){
                ack={};ack.lease=peer.pending[peer.head];peer.head=(peer.head+1)%peer.pending.size();--peer.count;
                if(ack.lease.operation<=peer.last_operation||ack.lease.audio_epoch<peer.epoch)ack.reason=audio::Reason::epoch;
                else if(ack.lease.groups&&(ack.lease.expires_qpc<=qpc||ack.lease.expires_qpc-qpc>static_cast<std::int64_t>(runtime.hello.qpc_frequency)))ack.reason=audio::Reason::expired;
                else {
                    peer.last_operation=ack.lease.operation;peer.epoch=ack.lease.audio_epoch;
                    ack.accepted_groups=ack.lease.groups&runtime.suppression_groups.load()&runtime.audio_groups.load();
                    runtime.output_lease.set(ack.accepted_groups,ack.lease.expires_qpc);
                    awaited_ticket=runtime.output_lease.token(qpc,runtime.suppression_groups.load());waiting_ack=true;
                }
                if(!waiting_ack)have=exporter::encode(ack,sequence++,output);
            }
            if(!have&&waiting_ack){
                const auto token=runtime.output_lease.token(qpc,runtime.suppression_groups.load());
                if(runtime.output_lease.applied.load()==awaited_ticket&&token==awaited_ticket){have=exporter::encode(ack,sequence++,output);waiting_ack=false;}
                else if((ack.lease.groups&&qpc>=ack.lease.expires_qpc)||token!=awaited_ticket){ack.accepted_groups=0;ack.reason=audio::Reason::expired;have=exporter::encode(ack,sequence++,output);waiting_ack=false;}
            }
            if(!have&&now>=health_at){
                exporter::Health health;health.rejected=runtime.rejected.load();health.attempted=runtime.attempted.load();health.pose_epoch=runtime.pose_epoch.load();health.flags=runtime.flags.load();
                health.sent=runtime.sent.load();health.enqueued=runtime.enqueued.load();health.queue_dropped=runtime.queue_dropped.load();health.transport_dropped=runtime.transport_dropped.load();
                health.audio_enqueued=runtime.audio_enqueued.load();health.audio_sent=runtime.audio_sent.load();health.audio_dropped=runtime.audio_dropped.load();health.audio_groups=runtime.audio_groups.load();health.suppression_groups=runtime.suppression_groups.load();
                have=exporter::encode(health,sequence++,output);health_at=now+1000;
            }
            // Alternating pose and group lanes prevent either producer family starving the other.
            for(std::size_t attempts=0;!have&&attempts<16;++attempts){
                const auto selected=(lane++)%16;
                if((selected&1)==0){motion::RawPose value;if(pop(value)){
                    if(value.sample_end_qpc>=subscribed_at&&qpc-value.sample_end_qpc<=static_cast<std::int64_t>(runtime.hello.qpc_frequency/2)){have=exporter::encode(value,sequence++,output);snapshot=have;}
                    if(!have)runtime.transport_dropped.fetch_add(1);
                }} else {
                    auto g=selected/2;audio::Snapshot value;
                    if(runtime.audio_slots[g].pop(value)){
                        if(value.sample_end_qpc>=subscribed_at&&qpc-value.sample_end_qpc<=static_cast<std::int64_t>(runtime.hello.qpc_frequency)){
                            have=exporter::encode(value,sequence++,output);audio_snapshot=have;if(have)last_audio[g]=value;
                        }
                        if(!have)runtime.audio_dropped.fetch_add(1);
                    }
                }
            }
            // Paused duplicates retain their original sample identity; receiver renews liveness only.
            if(!have&&now>=held_at&&runtime.audio_paused.load()&&qpc-runtime.audio_state_time.load()<static_cast<std::int64_t>(runtime.hello.qpc_frequency)){
                auto g=(held_group++)%8;
                if(last_audio[g].sequence&&(runtime.audio_groups.load()&(1u<<g))){
                    auto held=last_audio[g];held.flags|=audio::held;
                    have=exporter::encode(held,sequence++,output);
                    // A held duplicate is not a newly enqueued producer sample.
                    audio_snapshot=false;
                }
                held_at=now+25;
            }
            if(have){offset=0;writing=true;write_progress=GetTickCount64();if(!write.begin(pipe,true,output.bytes.data(),static_cast<DWORD>(output.size))){okay=false;break;}if(!write.pending)continue;}
        }
        HANDLE events[]={read.overlapped.hEvent,write.overlapped.hEvent,runtime.wake};
        if(!writing)ResetEvent(write.overlapped.hEvent);
        if(WaitForMultipleObjects(3,events,FALSE,25)==WAIT_FAILED){okay=false;break;}
    }
    runtime.active.store(false);runtime.output_lease.set(0,0);read.cancel(pipe);
    const bool completed_write=write.cancel(pipe);
    if(writing&&snapshot){if(completed_write&&offset+write.completed==output.size)runtime.sent.fetch_add(1);else runtime.transport_dropped.fetch_add(1);}
    if(writing&&audio_snapshot&&!completed_write)runtime.audio_dropped.fetch_add(1);
    discard_queue();return okay;
}
DWORD WINAPI worker(void*) noexcept {
    unsigned retry=1;
    for(;;){
        HANDLE pipe=CreateFileW(runtime.endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
        if(pipe!=INVALID_HANDLE_VALUE){
            try{service(pipe);}catch(...){runtime.active.store(false,std::memory_order_release);}
            CloseHandle(pipe);retry=1;
        }
        Sleep(retry*1000);if(retry<8)retry*=2;
    }
}
}

extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_BridgeNative_initialize(JNIEnv*,jclass) {
    if(!claim_runtime())return JNI_FALSE;
    bool expected=false;if(!runtime.initialized.compare_exchange_strong(expected,true))return JNI_TRUE;
    try{
        if(!identify()){runtime.initialized.store(false);return JNI_FALSE;}
        terrain::Hello terrain_hello;terrain_hello.role=terrain::Role::exporter;terrain_hello.instance=runtime.hello.producer;terrain_hello.pid=runtime.hello.pid;terrain_hello.process_creation_filetime=runtime.hello.process_creation_filetime;terrain_hello.qpc_frequency=runtime.hello.qpc_frequency;
        auto terrain_endpoint=runtime.endpoint;terrain_endpoint.replace(terrain_endpoint.find(L"psx-ios.exporter.v1."),std::wstring(L"psx-ios.exporter.v1.").size(),L"psx-ios.terrain.exporter.v3.");psx_exporter::start_terrain(terrain_hello,terrain_endpoint);
        try{psx::aircraft_sync::Hello aircraft_hello;aircraft_hello.role=psx::aircraft_sync::Role::exporter;aircraft_hello.instance=runtime.hello.producer;aircraft_hello.pid=runtime.hello.pid;aircraft_hello.process_creation_filetime=runtime.hello.process_creation_filetime;aircraft_hello.qpc_frequency=runtime.hello.qpc_frequency;
        auto aircraft_endpoint=runtime.endpoint;aircraft_endpoint.replace(aircraft_endpoint.find(L"psx-ios.exporter.v1."),std::wstring(L"psx-ios.exporter.v1.").size(),L"psx-ios.aircraft.exporter.v1.");psx_exporter::start_aircraft(aircraft_hello,aircraft_endpoint);}catch(...){}
        try{psx::environment::Hello environment_hello;environment_hello.role=psx::environment::Role::exporter;environment_hello.instance=runtime.hello.producer;environment_hello.pid=runtime.hello.pid;environment_hello.process_creation_filetime=runtime.hello.process_creation_filetime;environment_hello.qpc_frequency=runtime.hello.qpc_frequency;
        auto environment_endpoint=runtime.endpoint;environment_endpoint.replace(environment_endpoint.find(L"psx-ios.exporter.v1."),std::wstring(L"psx-ios.exporter.v1.").size(),L"psx-ios.environment.exporter.v1.");psx_exporter::start_environment(environment_hello,environment_endpoint);}catch(...){}
        try{psx::status::Hello status_hello;status_hello.role=psx::status::Role::exporter;status_hello.instance=runtime.hello.producer;status_hello.pid=runtime.hello.pid;status_hello.process_creation_filetime=runtime.hello.process_creation_filetime;status_hello.qpc_frequency=runtime.hello.qpc_frequency;
        auto status_endpoint=runtime.endpoint;status_endpoint.replace(status_endpoint.find(L"psx-ios.exporter.v1."),std::wstring(L"psx-ios.exporter.v1.").size(),L"psx-ios.status.exporter.v1.");psx_exporter::start_status(status_hello,status_endpoint);}catch(...){}
        try{psx::traffic_source::Hello traffic_hello;traffic_hello.instance=runtime.hello.producer;traffic_hello.pid=runtime.hello.pid;traffic_hello.process_creation_filetime=runtime.hello.process_creation_filetime;traffic_hello.qpc_frequency=runtime.hello.qpc_frequency;
        auto traffic_endpoint=runtime.endpoint;traffic_endpoint.replace(traffic_endpoint.find(L"psx-ios.exporter.v1."),std::wstring(L"psx-ios.exporter.v1.").size(),L"psx-ios.traffic.exporter.v1.");psx_exporter::start_traffic(traffic_hello,traffic_endpoint);}catch(...){}
        runtime.wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!runtime.wake){runtime.initialized.store(false);return JNI_FALSE;}
        HANDLE thread=CreateThread(nullptr,0,worker,nullptr,0,nullptr);
        if(!thread){CloseHandle(runtime.wake);runtime.wake=nullptr;runtime.initialized.store(false);return JNI_FALSE;}
        CloseHandle(thread);return JNI_TRUE;
    }catch(...){runtime.initialized.store(false);return JNI_FALSE;}
}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_BridgeNative_active(JNIEnv*,jclass) {return runtime.active.load(std::memory_order_acquire)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_BridgeNative_abiVersion(JNIEnv*,jclass) {return 2;}
// Probing this support must not claim ownership, start workers or open IPC.
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_BridgeNative_runtimeInstanceSupport(JNIEnv*,jclass) {return 1;}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_BridgeNative_claimRuntime(JNIEnv*,jclass) {return claim_runtime()?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jlong JNICALL Java_psx_exporter_BridgeNative_qpc(JNIEnv*,jclass) {return ticks();}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_BridgeNative_state(JNIEnv*,jclass,jlong attempted,jlong rejected,jlong epoch,jint flags) {
    runtime.attempted.store(static_cast<std::uint64_t>(attempted));runtime.rejected.store(static_cast<std::uint64_t>(rejected));runtime.pose_epoch.store(static_cast<std::uint64_t>(epoch));runtime.flags.store(static_cast<std::uint32_t>(flags));
}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_BridgeNative_publish(JNIEnv*,jclass,
    jlong sample_start,jlong frame_sequence,jlong pose_epoch,jlong scheduler_wall_ms,
    jdouble delta_ms,jdouble delta_acc_ms,jdouble simulation_rate,jint flags,
    jdouble latitude_rad,jdouble longitude_rad,jdouble heading_rad,
    jfloat total_pitch_rad,jfloat bank_rad,jfloat altitude_ft,jfloat main_strut_ft,jfloat base_pitch_rad,
    jfloat nose_osc_rad,jfloat nose_compress_rad,jfloat cached_sin_pitch,jfloat stock_eye_altitude_ft,
    jfloat longitudinal_heading_rad,jfloat fpv_rad,jfloat slope_fade,jdouble stock_eye_latitude_rad,jdouble stock_eye_longitude_rad) {
    const auto end=ticks();if(!runtime.active.load(std::memory_order_acquire))return;
    motion::RawPose value;value.sequence=runtime.sequence.fetch_add(1,std::memory_order_relaxed)+1;
    value.frame_sequence=static_cast<std::uint64_t>(frame_sequence);value.pose_epoch=static_cast<std::uint64_t>(pose_epoch);
    value.sample_start_qpc=sample_start;value.sample_end_qpc=end;value.scheduler_wall_ms=scheduler_wall_ms;
    value.delta_ms=delta_ms;value.delta_acc_ms=delta_acc_ms;value.simulation_rate=simulation_rate;value.flags=static_cast<std::uint32_t>(flags);
    value.latitude_rad=latitude_rad;value.longitude_rad=longitude_rad;value.heading_rad=heading_rad;
    value.total_pitch_rad=total_pitch_rad;value.bank_rad=bank_rad;value.altitude_ft=altitude_ft;value.main_strut_ft=main_strut_ft;value.base_pitch_rad=base_pitch_rad;
    value.nose_osc_rad=nose_osc_rad;value.nose_compress_rad=nose_compress_rad;value.cached_sin_pitch=cached_sin_pitch;value.stock_eye_altitude_ft=stock_eye_altitude_ft;
    value.longitudinal_heading_rad=longitudinal_heading_rad;value.fpv_rad=fpv_rad;value.slope_fade=slope_fade;value.stock_eye_latitude_rad=stock_eye_latitude_rad;value.stock_eye_longitude_rad=stock_eye_longitude_rad;
    const auto w=runtime.write.load(std::memory_order_relaxed),r=runtime.read.load(std::memory_order_acquire);
    if(w-r>=capacity){runtime.queue_dropped.fetch_add(1,std::memory_order_relaxed);return;}
    runtime.slots[w%capacity]=value;runtime.enqueued.fetch_add(1,std::memory_order_relaxed);runtime.write.store(w+1,std::memory_order_release);
    if(!runtime.wake_posted.exchange(true,std::memory_order_acq_rel))SetEvent(runtime.wake);
}

extern "C" JNIEXPORT void JNICALL Java_psx_exporter_BridgeNative_audioCapabilities(JNIEnv*,jclass,jint groups,jint suppression) {
    runtime.audio_groups.store(static_cast<std::uint32_t>(groups)&255u);runtime.suppression_groups.store(static_cast<std::uint32_t>(suppression)&static_cast<std::uint32_t>(groups)&255u);
}
extern "C" JNIEXPORT jlong JNICALL Java_psx_exporter_BridgeNative_audioControl(JNIEnv*,jclass) {return static_cast<jlong>(runtime.output_lease.token(ticks(),runtime.suppression_groups.load()));}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_BridgeNative_audioApplied(JNIEnv*,jclass,jlong control) {runtime.output_lease.applied.store(static_cast<std::uint64_t>(control));}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_BridgeNative_audioPaused(JNIEnv*,jclass,jboolean paused) {runtime.audio_paused.store(paused!=0);runtime.audio_state_time.store(ticks());}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_BridgeNative_audioPublish(JNIEnv*,jclass,jint group,jint family,jint variant,jint flags,jint valid,
    jlong sequence,jlong lifecycle,jlong start,
    jfloat v0,jfloat v1,jfloat v2,jfloat v3,jfloat v4,jfloat v5,jfloat v6,jfloat v7,
    jfloat v8,jfloat v9,jfloat v10,jfloat v11,jfloat v12,jfloat v13,jfloat v14,jfloat v15,
    jfloat v16,jfloat v17,jfloat v18,jfloat v19,jfloat v20,jfloat v21,jfloat v22,jfloat v23,
    jfloat v24,jfloat v25,jfloat v26,jfloat v27,jfloat v28,jfloat v29,jfloat v30,jfloat v31) {
    const auto end=ticks();if(!runtime.active.load()||group<0||group>=8||family<1||family>3||variant<0||variant>255)return;
    const auto key=static_cast<std::uint64_t>((family<<8)|variant);
    auto configuration=runtime.configuration.load();
    if((configuration&65535u)!=key){if(group!=0)return;auto updated=(((configuration>>16)+1)<<16)|key;if(!runtime.configuration.compare_exchange_strong(configuration,updated)){runtime.audio_dropped.fetch_add(1);return;}configuration=updated;}
    audio::Snapshot s;s.group=static_cast<audio::Group>(group);s.source=audio::Source::exporter;s.family=static_cast<audio::Family>(family);s.variant=static_cast<std::uint8_t>(variant);
    s.sequence=static_cast<std::uint64_t>(sequence);s.lifecycle_epoch=static_cast<std::uint64_t>(lifecycle);s.configuration_epoch=configuration>>16;
    s.sample_start_qpc=start;s.sample_end_qpc=end;s.valid_fields=static_cast<std::uint32_t>(valid);s.flags=static_cast<std::uint32_t>(flags);
    s.values={v0,v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13,v14,v15,v16,v17,v18,v19,v20,v21,v22,v23,v24,v25,v26,v27,v28,v29,v30,v31};
    if(!audio::valid(s)||!runtime.audio_slots[static_cast<std::size_t>(group)].push(s)){runtime.audio_dropped.fetch_add(1);return;}
    runtime.audio_enqueued.fetch_add(1);if(!runtime.wake_posted.exchange(true))SetEvent(runtime.wake);
}
