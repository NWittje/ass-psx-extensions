#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <jni.h>
#include <psx_exporter/terrain_runtime.hpp>
#include <psx_exporter/audio_runtime.hpp>
#include <psx/concurrency/triple_buffer.hpp>
#include <psx/windows/overlapped.hpp>
#include <array>
#include <atomic>
#include <cstring>
#include <cmath>

namespace {
using namespace psx;
// Three owned slots: neither thread reads a slot being written. Latest-only data lane.
using psx::concurrency::TripleBuffer;
struct Command {terrain::Control control{};std::int64_t deadline{};std::uint64_t generation{};terrain::Reason reason{};};
struct Plane {terrain::Surface surface{};std::uint64_t generation{};};
struct Support {terrain::SupportDetail detail{};std::uint64_t generation{};};
struct Constraint {terrain::ConstraintDetail detail{};std::uint64_t generation{};};
struct NativeFrame {terrain::FrameDetail detail{};std::uint64_t generation{};};
struct Applied {terrain::Ack ack{};std::uint64_t generation{};std::int64_t deadline{};};
struct Runtime {
    terrain::Hello hello{};std::wstring endpoint;
    std::atomic<std::uint32_t> capabilities{};std::atomic<bool> connected{};
    std::atomic<std::uint64_t> generation{},pose_epoch{},capture_generation{};
    std::atomic<std::int64_t> capture_start_qpc{};
    psx_exporter::SampleRing<Command,8> controls;
    psx_exporter::SampleRing<Applied,8> acknowledgments;
    TripleBuffer<Plane> surfaces;TripleBuffer<terrain::Consumed> consumed;TripleBuffer<Support> support_details;TripleBuffer<Constraint> constraint_details;TripleBuffer<NativeFrame> frame_details;
    Command polled{};bool polled_valid{};
} runtime;
std::int64_t ticks() noexcept {LARGE_INTEGER t{};QueryPerformanceCounter(&t);return t.QuadPart;}
using Operation=psx::windows::Overlapped;
bool send(HANDLE pipe,terrain::Frame& frame) noexcept {
    Operation op;if(!op.ov.hEvent)return false;std::size_t offset=0;const auto deadline=GetTickCount64()+1000;
    while(offset<frame.size){
        if(!op.begin(pipe,true,frame.bytes.data()+offset,static_cast<DWORD>(frame.size-offset)))return false;
        if(op.pending){const auto now=GetTickCount64();if(now>=deadline||WaitForSingleObject(op.ov.hEvent,static_cast<DWORD>(deadline-now))!=WAIT_OBJECT_0){op.cancel(pipe);return false;}if(!op.finish(pipe)){op.cancel(pipe);return false;}}
        if(!op.done)return false;offset+=op.done;
    }return true;
}
struct Peer {
    bool subscribed{},active{},intent_active{};std::uint32_t pid{};std::uint64_t owner{},operation{},epoch{},pose{},generation{},intent_epoch{},intent_pose{};
    std::int64_t deadline{};std::uint64_t last_query{};
    std::int64_t intent_deadline{},acquire_floor{};std::uint32_t sequence{};std::uint64_t intended_acquire{},applied_acquire{};
    std::uint64_t capture_generation{};bool capture{};unsigned detail_index{};
};
bool receive(void* context,const terrain::Header& h,const std::uint8_t* bytes,std::size_t n) {
    auto& p=*static_cast<Peer*>(context);const auto now=ticks();
    if(!h.sequence||h.sequence<=p.sequence)return false;p.sequence=h.sequence;
    if(!p.subscribed){
        terrain::Subscribe s;if(h.kind!=terrain::Kind::subscribe||!terrain::decode(bytes,n,s)||s.instance!=runtime.hello.instance||s.qpc_frequency!=runtime.hello.qpc_frequency||s.pid!=p.pid)return false;
        p.owner=s.owner;p.subscribed=true;runtime.connected.store(true,std::memory_order_release);return true;
    }
    if(h.kind==terrain::Kind::capture){
        terrain::Capture c;if(!terrain::decode(bytes,n,c)||c.owner!=p.owner||c.generation<p.capture_generation)return false;
        if(c.generation==p.capture_generation&&p.capture!=(c.enabled!=0))return false;
        if(c.generation!=p.capture_generation)runtime.capture_start_qpc.store(c.enabled?now:0,std::memory_order_release);
        p.capture_generation=c.generation;p.capture=c.enabled!=0;
        runtime.capture_generation.store(p.capture?c.generation:0,std::memory_order_release);return true;
    }
    if(h.kind==terrain::Kind::control){
        terrain::Control c;if(!terrain::decode(bytes,n,c)||c.producer!=runtime.hello.instance||c.owner!=p.owner)return false;
        auto reason=terrain::Reason::none;
        if(!(runtime.capabilities.load()&terrain::native_support))reason=terrain::Reason::unsupported;
        else if(c.operation<=p.operation||c.terrain_epoch<p.intent_epoch||c.pose_epoch!=runtime.pose_epoch.load())reason=terrain::Reason::epoch;
        else if(c.action!=terrain::Action::acquire&&(!p.intent_active||c.terrain_epoch!=p.intent_epoch||c.pose_epoch!=p.intent_pose))reason=terrain::Reason::owner;
        else if(c.action==terrain::Action::renew&&now>=p.intent_deadline)reason=terrain::Reason::expired;
        if(c.operation>p.operation)p.operation=c.operation;
        Command command{c,now+static_cast<std::int64_t>(runtime.hello.qpc_frequency),p.generation,reason};
        if(!runtime.controls.push(command))return false;
        if(reason==terrain::Reason::none){p.intent_epoch=c.terrain_epoch;p.intent_pose=c.pose_epoch;p.intent_active=c.action!=terrain::Action::release;p.intent_deadline=command.deadline;if(c.action!=terrain::Action::renew)runtime.surfaces.put({});if(c.action==terrain::Action::acquire){p.acquire_floor=now;p.intended_acquire=c.operation;}}
        return true;
    }
    if(h.kind==terrain::Kind::surface){
        terrain::Surface s;if(!terrain::decode(bytes,n,s))return false;
        const auto& q=s.query;
        if(!terrain::qualified_footprint(s))return true;
        if(!p.active||!p.intent_active||p.intended_acquire!=p.applied_acquire||s.query_start_qpc<p.acquire_floor||p.intent_epoch!=p.epoch||now>=p.deadline||!(s.flags&terrain::hit)||(s.flags&(terrain::wet|terrain::moving))||q.owner!=p.owner||q.reference.producer!=runtime.hello.instance||q.terrain_epoch!=p.epoch||q.reference.pose_epoch!=p.pose||q.reference.pose_epoch!=runtime.pose_epoch.load()||q.query_id<=p.last_query||q.reference.sample_qpc>now+1||q.reference.received_qpc>now+1||s.query_end_qpc>now+1||s.query_end_qpc<q.reference.sample_qpc||now-q.reference.sample_qpc>=static_cast<std::int64_t>(runtime.hello.qpc_frequency/10)||now-q.reference.received_qpc>=static_cast<std::int64_t>(runtime.hello.qpc_frequency/10)||now-s.query_end_qpc>=static_cast<std::int64_t>(runtime.hello.qpc_frequency/10))return true;
        p.last_query=q.query_id;runtime.surfaces.put({s,p.generation});return true;
    }
    return false;
}
bool service(HANDLE pipe) noexcept {
    Peer peer;ULONG server{};if(!GetNamedPipeServerProcessId(pipe,&server)||!server)return false;peer.pid=server;
    peer.generation=runtime.generation.fetch_add(1)+1;
    terrain::Frame output;std::uint32_t sequence=1;runtime.hello.capabilities=runtime.capabilities.load();
    if(!terrain::encode(runtime.hello,sequence++,output)||!send(pipe,output))return false;
    Operation read,write;std::array<std::uint8_t,2048> bytes{};terrain::Framer framer;
    if(!read.ov.hEvent||!write.ov.hEvent||!read.begin(pipe,false,bytes.data(),static_cast<DWORD>(bytes.size())))return false;
    bool okay=true,writing=false;std::size_t offset=0;auto progress=GetTickCount64();const auto started=progress;
    while(okay){
        if(runtime.hello.capabilities!=runtime.capabilities.load()){okay=false;break;}
        if(!read.pending||WaitForSingleObject(read.ov.hEvent,0)==WAIT_OBJECT_0){
            if(!read.finish(pipe)||!read.done||!framer.feed(bytes.data(),read.done,receive,&peer)){okay=false;break;}
            if(!read.begin(pipe,false,bytes.data(),static_cast<DWORD>(bytes.size()))){okay=false;break;}
            runtime.connected.store(peer.subscribed,std::memory_order_release);
        }
        if(!peer.subscribed&&GetTickCount64()-started>=1000){okay=false;break;}
        if(writing&&GetTickCount64()-progress>=1000){okay=false;break;}
        if(writing&&(!write.pending||WaitForSingleObject(write.ov.hEvent,0)==WAIT_OBJECT_0)){
            if(!write.finish(pipe)||!write.done){okay=false;break;}offset+=write.done;progress=GetTickCount64();
            if(offset<output.size){if(!write.begin(pipe,true,output.bytes.data()+offset,static_cast<DWORD>(output.size-offset))){okay=false;break;}}
            else writing=false;
        }
        if(!writing&&peer.subscribed){
            bool have=false;
            Applied applied;
            if(!have&&runtime.acknowledgments.pop(applied)&&applied.generation==peer.generation){
                const auto& c=applied.ack.control;
                if(applied.ack.reason==terrain::Reason::none){peer.epoch=c.terrain_epoch;peer.pose=c.pose_epoch;peer.active=c.action!=terrain::Action::release;peer.deadline=applied.deadline;if(c.action==terrain::Action::acquire)peer.applied_acquire=c.operation;}
                else if(c.operation==peer.operation&&c.action!=terrain::Action::release){peer.intent_active=peer.active=false;}
                have=terrain::encode(applied.ack,sequence++,output);
            }
            for(unsigned turn=0;!have&&turn<4;++turn){
                const auto lane=peer.detail_index++%4;
                if(lane==0){terrain::Consumed c;if(runtime.consumed.take(c))have=terrain::encode(c,sequence++,output);}
                else if(lane==1){Support d;if(runtime.support_details.take(d)&&peer.capture&&d.generation==peer.generation&&d.detail.capture_generation==peer.capture_generation)have=terrain::encode(d.detail,sequence++,output);}
                else if(lane==2){Constraint d;if(runtime.constraint_details.take(d)&&peer.capture&&d.generation==peer.generation&&d.detail.capture_generation==peer.capture_generation)have=terrain::encode(d.detail,sequence++,output);}
                else {NativeFrame d;if(runtime.frame_details.take(d)&&peer.capture&&d.generation==peer.generation&&d.detail.capture_generation==peer.capture_generation)have=terrain::encode(d.detail,sequence++,output);}
            }
            if(have){offset=0;writing=true;progress=GetTickCount64();if(!write.begin(pipe,true,output.bytes.data(),static_cast<DWORD>(output.size))){okay=false;break;}if(!write.pending)continue;}
        }
        HANDLE events[]={read.ov.hEvent,write.ov.hEvent};if(!writing)ResetEvent(write.ov.hEvent);
        if(WaitForMultipleObjects(2,events,FALSE,5)==WAIT_FAILED){okay=false;break;}
    }
    runtime.capture_generation.store(0,std::memory_order_release);runtime.connected.store(false,std::memory_order_release);runtime.generation.fetch_add(1);read.cancel(pipe);write.cancel(pipe);return okay;
}
DWORD WINAPI worker(void*) noexcept {
    for(;;){HANDLE pipe=CreateFileW(runtime.endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
        if(pipe!=INVALID_HANDLE_VALUE){try{service(pipe);}catch(...){runtime.capture_generation.store(0);runtime.connected.store(false);runtime.generation.fetch_add(1);}CloseHandle(pipe);}Sleep(250);
    }
}
template<class T> void store(void* data,std::size_t offset,T v) noexcept {std::memcpy(static_cast<std::uint8_t*>(data)+offset,&v,sizeof(v));}
template<class T> T load(const void* data,std::size_t offset) noexcept {T v{};std::memcpy(&v,static_cast<const std::uint8_t*>(data)+offset,sizeof(v));return v;}
}
namespace psx_exporter {
void start_terrain(const psx::terrain::Hello& hello,const std::wstring& endpoint) noexcept {
    try{runtime.hello=hello;runtime.endpoint=endpoint;HANDLE t=CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(t)CloseHandle(t);}catch(...){runtime.capabilities.store(0);}
}
}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_BridgeNative_terrainAbiVersion(JNIEnv*,jclass){return terrain::terrain_jni_abi;}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_BridgeNative_terrainCapabilities(JNIEnv*,jclass,jint capabilities){runtime.capabilities.store((static_cast<unsigned>(capabilities)&terrain::native_support)?terrain::native_support|terrain::support_details|terrain::constraint_details|terrain::frame_details:0u);}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_BridgeNative_terrainThreadId(JNIEnv*,jclass){return static_cast<jint>(GetCurrentThreadId());}
extern "C" JNIEXPORT jlong JNICALL Java_psx_exporter_BridgeNative_terrainCaptureGeneration(JNIEnv*,jclass){return static_cast<jlong>(runtime.capture_generation.load(std::memory_order_acquire));}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_BridgeNative_terrainDetailPublish(JNIEnv* env,jclass,jobject buffer){
    const auto connection=runtime.generation.load(std::memory_order_acquire),generation=runtime.capture_generation.load(std::memory_order_acquire);
    if(!generation||!runtime.connected.load(std::memory_order_acquire))return;
    const void* data=env->GetDirectBufferAddress(buffer);const auto capacity=env->GetDirectBufferCapacity(buffer);
    if(!data||capacity<8)return;const auto type=load<std::uint32_t>(data,0),size=load<std::uint32_t>(data,4);
    if((type==1&&size!=464)||(type==2&&size!=720)||(type==3&&size!=136)||type<1||type>3||capacity<size)return;
    const auto epoch=load<std::uint64_t>(data,8),capture=load<std::uint64_t>(data,type==3?24:48);
    const auto start=load<std::int64_t>(data,type==3?72:56);
    if(capture!=generation||start<runtime.capture_start_qpc.load(std::memory_order_acquire)||epoch!=runtime.pose_epoch.load(std::memory_order_acquire))return;
    const auto coherent=[&]{return runtime.connected.load(std::memory_order_acquire)&&connection==runtime.generation.load(std::memory_order_acquire)&&generation==runtime.capture_generation.load(std::memory_order_acquire)&&epoch==runtime.pose_epoch.load(std::memory_order_acquire);};
    // Copy fixed primitive records here; wire validation/serialization remain on the worker.
    if(type==1){
        terrain::SupportDetail d;d.producer=runtime.hello.instance;d.pose_epoch=epoch;d.terrain_epoch=load<std::uint64_t>(data,16);d.frame_sequence=load<std::uint64_t>(data,24);d.query_id=load<std::uint64_t>(data,32);d.owner=load<std::uint64_t>(data,40);d.capture_generation=generation;
        d.step_start_qpc=start;d.step_end_qpc=load<std::int64_t>(data,64);d.native_phase=static_cast<terrain::NativePhase>(load<std::uint32_t>(data,72));d.flags=load<std::uint32_t>(data,76);d.valid_mask=load<std::uint32_t>(data,80);d.count=load<std::uint32_t>(data,84);
        d.entry_elev_ft=load<double>(data,88);d.shadow_elev_ft=load<double>(data,96);d.cached_support_ft=load<double>(data,104);d.altitude_damper_ft=load<double>(data,112);d.begin_ticks=load<std::uint64_t>(data,120);d.step_ticks=load<std::uint64_t>(data,128);d.helper_ticks=load<std::uint64_t>(data,136);
        for(std::size_t i=0;i<d.sites.size();++i){auto& x=d.sites[i];const auto o=144+i*40;x.latitude_deg=load<double>(data,o);x.longitude_deg=load<double>(data,o+8);x.constraint_ft=load<double>(data,o+16);x.plane_ft=load<double>(data,o+24);x.ordinary_strut_ft=load<double>(data,o+32);}if(coherent())runtime.support_details.put({d,connection});
    }else if(type==2){
        terrain::ConstraintDetail d;d.producer=runtime.hello.instance;d.pose_epoch=epoch;d.terrain_epoch=load<std::uint64_t>(data,16);d.frame_sequence=load<std::uint64_t>(data,24);d.query_id=load<std::uint64_t>(data,32);d.owner=load<std::uint64_t>(data,40);d.capture_generation=generation;d.step_start_qpc=start;d.step_end_qpc=load<std::int64_t>(data,64);d.valid_mask=load<std::uint32_t>(data,72);d.count=load<std::uint32_t>(data,76);
        for(std::size_t i=0;i<d.sites.size();++i){auto& x=d.sites[i];const auto o=80+i*40;x.field=load<std::uint32_t>(data,o);x.flags=load<std::uint32_t>(data,o+4);x.cached_world_rad=load<double>(data,o+8);x.stored_basis_rad=load<double>(data,o+16);x.current_basis_rad=load<double>(data,o+24);x.returned_world_rad=load<double>(data,o+32);}if(coherent())runtime.constraint_details.put({d,connection});
    }else{
        terrain::FrameDetail d;d.producer=runtime.hello.instance;d.pose_epoch=epoch;d.frame_sequence=load<std::uint64_t>(data,16);d.capture_generation=generation;d.java_thread_id=load<std::uint64_t>(data,32);d.os_thread_id=load<std::uint32_t>(data,40);d.valid_mask=load<std::uint32_t>(data,44);d.flags=load<std::uint32_t>(data,48);d.delta_ms=load<double>(data,56);d.delta_acc_ms=load<double>(data,64);for(std::size_t i=0;i<8;++i)d.stage_qpc[i]=load<std::int64_t>(data,72+i*8);if(coherent())runtime.frame_details.put({d,connection});
    }
}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_BridgeNative_terrainConnected(JNIEnv*,jclass){return runtime.connected.load()?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jlong JNICALL Java_psx_exporter_BridgeNative_terrainConnection(JNIEnv*,jclass){return static_cast<jlong>(runtime.generation.load());}
extern "C" JNIEXPORT jlong JNICALL Java_psx_exporter_BridgeNative_terrainFrequency(JNIEnv*,jclass){return static_cast<jlong>(runtime.hello.qpc_frequency);}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_BridgeNative_terrainPoll(JNIEnv* env,jclass,jobject buffer,jlong lifecycle){
    runtime.pose_epoch.store(static_cast<std::uint64_t>(lifecycle));void* data=env->GetDirectBufferAddress(buffer);if(!data||env->GetDirectBufferCapacity(buffer)<160)return 0;
    const auto generation=runtime.generation.load();Command command;
    for(int i=0;i<8&&runtime.controls.pop(command);++i){
        if(command.generation!=generation||!runtime.connected.load())continue;
        runtime.polled=command;runtime.polled_valid=true;std::memset(data,0,160);
        store(data,0,std::int32_t{1});store(data,4,static_cast<std::uint32_t>(command.control.action));store(data,8,command.control.owner);store(data,16,command.control.operation);store(data,24,command.control.terrain_epoch);store(data,32,command.control.pose_epoch);store(data,56,command.deadline);store(data,104,runtime.hello.qpc_frequency);store(data,112,static_cast<std::uint32_t>(command.reason));return 1;
    }
    Plane plane;if(runtime.surfaces.take(plane)&&plane.generation==generation&&runtime.connected.load()){
        const auto& s=plane.surface;std::memset(data,0,160);store(data,0,std::int32_t{2});store(data,8,s.query.owner);store(data,24,s.query.terrain_epoch);store(data,32,s.query.reference.pose_epoch);store(data,40,s.query.query_id);store(data,48,s.query.reference.sample_qpc);
        store(data,64,s.height_msl_m);store(data,72,s.latitude_deg);store(data,80,s.longitude_deg);store(data,88,-s.normal_east/s.normal_up);store(data,96,-s.normal_north/s.normal_up);store(data,104,runtime.hello.qpc_frequency);store(data,120,s.query_end_qpc);return 2;
    }return 0;
}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_BridgeNative_terrainApplied(JNIEnv*,jclass,jlong operation,jint phase,jint reason){
    if(!runtime.polled_valid||runtime.polled.control.operation!=static_cast<std::uint64_t>(operation))return;
    Applied applied;applied.ack.control=runtime.polled.control;applied.ack.phase=static_cast<terrain::Phase>(phase);applied.ack.reason=static_cast<terrain::Reason>(reason);applied.generation=runtime.polled.generation;applied.deadline=runtime.polled.deadline;
    if(!runtime.acknowledgments.push(applied))runtime.connected.store(false);runtime.polled_valid=false;
}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_BridgeNative_terrainPublish(JNIEnv*,jclass,jlong pose,jlong epoch,jlong frame,jlong query,jlong owner,jlong at,jlong source,jint phase,jint reason,
    jdouble latitude,jdouble longitude,jdouble height,jdouble east,jdouble north,jdouble up,jdouble pitch,jdouble bank,jfloat strut,jfloat osc,jfloat compress,jint configuration,jint contacts,jlong applied,jlong rejected,jlong overhead,jlong sample){
    terrain::Consumed c;c.producer=runtime.hello.instance;c.pose_epoch=static_cast<std::uint64_t>(pose);c.terrain_epoch=static_cast<std::uint64_t>(epoch);c.frame_sequence=static_cast<std::uint64_t>(frame);c.query_id=static_cast<std::uint64_t>(query);c.owner=static_cast<std::uint64_t>(owner);c.consumed_qpc=at;c.surface_qpc=source;c.phase=static_cast<terrain::Phase>(phase);c.reason=static_cast<terrain::Reason>(reason);
    c.latitude_deg=latitude;c.longitude_deg=longitude;c.effective_height_msl_m=height;c.normal_east=east;c.normal_north=north;c.normal_up=up;c.support_pitch_rad=pitch;c.support_bank_left_rad=bank;c.strut_ft=strut;c.nose_osc_rad=osc;c.nose_compress_rad=compress;c.support_configuration=static_cast<std::uint32_t>(configuration);c.contact_flags=static_cast<std::uint32_t>(contacts);c.applied=static_cast<std::uint64_t>(applied);c.rejected=static_cast<std::uint64_t>(rejected);c.hook_ticks=static_cast<std::uint64_t>(overhead);c.source_sample_qpc=sample;runtime.consumed.put(c);
}
