#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <jni.h>
#include "du_timing.hpp"
#include <psx_exporter/recorder_protocol.hpp>
#include <psx/display/instrument_protocol.hpp>
#include <array>
#include <atomic>
#include <string>

namespace {
using namespace psx::recorder;
struct Runtime {
    std::atomic<bool> initialized{};std::atomic<unsigned> init_state{};
    std::atomic<std::uint64_t> head{},tail{},generation{},deadline{},rejected{},transport_lost{},drain_generation{},drain_deadline{};
    std::array<Batch,queue_capacity> batches{};
    std::wstring endpoint,sddl;std::string metadata;
    std::uint32_t pid{};std::uint64_t created{},frequency{};HANDLE wake{};
    bool instrument{};
} runtimes[7];
std::uint64_t ticks() noexcept {LARGE_INTEGER q{};QueryPerformanceCounter(&q);return static_cast<std::uint64_t>(q.QuadPart);}
std::uint64_t born(FILETIME t) noexcept {return (static_cast<std::uint64_t>(t.dwHighDateTime)<<32)|t.dwLowDateTime;}
bool identity(Runtime& runtime,unsigned unit){
    HANDLE token{};if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;
    std::array<std::uint8_t,65536> data{};DWORD n{};LPWSTR user{},logon{};
    bool ok=GetTokenInformation(token,TokenUser,data.data(),static_cast<DWORD>(data.size()),&n)!=FALSE;
    if(ok)ok=ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&user)!=FALSE;
    if(ok)ok=GetTokenInformation(token,TokenGroups,data.data(),static_cast<DWORD>(data.size()),&n)!=FALSE;
    if(ok){auto groups=reinterpret_cast<TOKEN_GROUPS*>(data.data());for(DWORD i=0;i<groups->GroupCount;i++)
        if((groups->Groups[i].Attributes&SE_GROUP_LOGON_ID)==SE_GROUP_LOGON_ID){ok=ConvertSidToStringSidW(groups->Groups[i].Sid,&logon)!=FALSE;break;}}
    CloseHandle(token);if(!ok||!user||!logon){if(user)LocalFree(user);if(logon)LocalFree(logon);return false;}
    FILETIME c{},e{},k{},u{};ok=GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u)!=FALSE;
    runtime.pid=GetCurrentProcessId();runtime.created=born(c);
    runtime.endpoint=(runtime.instrument?L"\\\\.\\pipe\\psx-ios.instrument-recorder.v1.":L"\\\\.\\pipe\\psx-ios.display-recorder.v1.")+std::wstring(user)+L"."+std::to_wstring(runtime.pid)+L"."+std::to_wstring(runtime.created);
    if(unit&&!runtime.instrument)runtime.endpoint+=L".du"+std::to_wstring(unit);
    runtime.sddl=L"D:P(A;;GA;;;"+std::wstring(logon)+L")(A;;GA;;;SY)";
    LocalFree(user);LocalFree(logon);LARGE_INTEGER f{};QueryPerformanceFrequency(&f);runtime.frequency=static_cast<std::uint64_t>(f.QuadPart);return ok&&f.QuadPart>0;
}
struct Operation {OVERLAPPED ov{};Operation(){ov.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);}~Operation(){if(ov.hEvent)CloseHandle(ov.hEvent);}};
bool io(HANDLE pipe,bool write,void* data,DWORD bytes){
    DWORD offset{};const auto limit=GetTickCount64()+250;
    while(offset<bytes){Operation op;if(!op.ov.hEvent)return false;DWORD done{};
        BOOL ok=write?WriteFile(pipe,static_cast<std::uint8_t*>(data)+offset,bytes-offset,&done,&op.ov):ReadFile(pipe,static_cast<std::uint8_t*>(data)+offset,bytes-offset,&done,&op.ov);
        if(!ok){if(GetLastError()!=ERROR_IO_PENDING)return false;const auto now=GetTickCount64();
            if(now>=limit||WaitForSingleObject(op.ov.hEvent,static_cast<DWORD>(limit-now))!=WAIT_OBJECT_0){CancelIoEx(pipe,&op.ov);GetOverlappedResult(pipe,&op.ov,&done,TRUE);return false;}
            if(!GetOverlappedResult(pipe,&op.ov,&done,FALSE))return false;
        }
        if(!done)return false;offset+=done;
    }return true;
}
bool send(Runtime& runtime,HANDLE pipe,std::uint16_t kind,void* data,std::uint32_t bytes,std::uint32_t& sequence){
    Header h;if(runtime.instrument)h.magic_value=psx::instrument::magic;h.kind=kind;h.bytes=bytes;h.sequence=++sequence;return io(pipe,true,&h,sizeof h)&&io(pipe,true,data,bytes);
}
bool take(Runtime& runtime,Batch& b){const auto r=runtime.tail.load(std::memory_order_relaxed);if(r==runtime.head.load(std::memory_order_acquire))return false;
    const auto& slot=runtime.batches[r%queue_capacity];b.size=slot.size;std::memcpy(b.bytes.data(),slot.bytes.data(),b.size);runtime.tail.store(r+1,std::memory_order_release);return true;
}
DWORD WINAPI worker(void* data){
    auto& runtime=*static_cast<Runtime*>(data);
    PSECURITY_DESCRIPTOR descriptor{};
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(runtime.sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)){runtime.init_state.store(3);return 0;}
    SECURITY_ATTRIBUTES security{sizeof security,descriptor,FALSE};
    HANDLE pipe=CreateNamedPipeW(runtime.endpoint.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,131072,4096,0,&security);
    LocalFree(descriptor);if(pipe==INVALID_HANDLE_VALUE){runtime.init_state.store(3);return 0;}
    runtime.initialized.store(true,std::memory_order_release);runtime.init_state.store(2);
    Batch batch;std::uint64_t nextGeneration=0;
    for(;;){
        Operation connect;BOOL linked=ConnectNamedPipe(pipe,&connect.ov);DWORD error=linked?0:GetLastError();
        if(!linked&&error!=ERROR_IO_PENDING&&error!=ERROR_PIPE_CONNECTED){DisconnectNamedPipe(pipe);Sleep(50);continue;}
        if(error==ERROR_IO_PENDING){while(WaitForSingleObject(connect.ov.hEvent,30)==WAIT_TIMEOUT){while(take(runtime,batch))runtime.transport_lost.fetch_add(1);}DWORD done{};
            if(!GetOverlappedResult(pipe,&connect.ov,&done,FALSE)){DisconnectNamedPipe(pipe);continue;}}
        std::array<std::uint8_t,4096> hello{};word(hello.data(),0,runtime.pid);word(hello.data(),8,runtime.created);word(hello.data(),16,runtime.frequency);
        word(hello.data(),24,1);std::memcpy(hello.data()+32,runtime.metadata.data(),runtime.metadata.size());std::uint32_t outSequence=0,inSequence=0;
        bool fullFormat=false;std::uint64_t skippedFrame=0,skippedGeneration=0;
        bool healthy=send(runtime,pipe,1,hello.data(),static_cast<std::uint32_t>(32+runtime.metadata.size()),outSequence);
        while(healthy){
            const auto now=ticks();if(runtime.generation.load()!=0&&now>=runtime.deadline.load()){runtime.generation.store(0);runtime.deadline.store(0);runtime.drain_generation.store(0);runtime.drain_deadline.store(0);}
            DWORD available{};if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr))break;
            if(available){Header h{};std::array<std::uint32_t,2> command{};
                if(!io(pipe,false,&h,sizeof h)||h.magic_value!=(runtime.instrument?psx::instrument::magic:magic)||h.version!=1||h.kind!=2||h.bytes!=8||h.sequence<=inSequence||!io(pipe,false,command.data(),8))break;
                inSequence=h.sequence;
                if(command[0]>=1&&command[0]<=3&&command[1]>1)break;
                if(command[0]==1){if(!fullFormat&&runtime.endpoint.find(L".du")!=std::wstring::npos)break;runtime.drain_generation.store(0);runtime.drain_deadline.store(0);runtime.deadline.store(command[1]?ticks()+runtime.frequency*2:0);runtime.generation.store(command[1]?++nextGeneration:0);}
                else if(command[0]==2){if(command[1]&&runtime.generation.load()!=0)runtime.deadline.store(ticks()+runtime.frequency*2);}
                else if(command[0]==3){runtime.drain_generation.store(runtime.generation.load());runtime.drain_deadline.store(ticks()+runtime.frequency);runtime.generation.store(0);runtime.deadline.store(0);}
                else if(command[0]==4&&command[1]==(runtime.instrument?1u:2u)&&inSequence==1){fullFormat=true;}
                else break;
            }
            if(healthy&&take(runtime,batch)){
                
                const auto kind=integer(batch.bytes.data(),0);
                if(!runtime.instrument&&!fullFormat&&runtime.endpoint.find(L".du")==std::wstring::npos){
                    const auto frame=word(batch.bytes.data(),8),generation=word(batch.bytes.data(),16);
                    if(kind==1&&integer(batch.bytes.data(),24)==0&&integer(batch.bytes.data(),28)>=256){
                        auto* header=batch.bytes.data()+32;const auto content=integer(header,12);
                        skippedFrame=skippedGeneration=0;
                        if(content>=4&&content<=6){
                            skippedFrame=frame;skippedGeneration=generation;
                            const std::uint32_t total=256,format=1,unsupported=3,zero=0,reason=4;
                            std::memcpy(batch.bytes.data()+4,&total,4);std::memcpy(batch.bytes.data()+28,&total,4);
                            std::memcpy(header+4,&format,4);std::memcpy(header+8,&total,4);std::memcpy(header+12,&unsupported,4);
                            std::memcpy(header+144,&reason,4);std::memcpy(header+148,&zero,4);std::memcpy(header+152,&zero,4);std::memcpy(header+240,&zero,4);
                            batch.size=288;
                        }
                    }else if((kind==1||kind==2)&&frame==skippedFrame&&generation==skippedGeneration)continue;
                }
                const auto timingUnit=unsigned(&runtime-runtimes);
                if(!runtime.instrument&&(kind==1||kind==2)){auto offset=integer(batch.bytes.data(),24);if(offset==0)psx::du::timing::event(12+timingUnit,36,timingUnit,word(batch.bytes.data(),16),word(batch.bytes.data(),8),0,kind);}
                healthy=send(runtime,pipe,3,batch.bytes.data(),batch.size,outSequence);
                if(!runtime.instrument&&healthy&&(kind==1||kind==2)&&integer(batch.bytes.data(),24)+integer(batch.bytes.data(),28)==integer(batch.bytes.data(),4))psx::du::timing::event(12+timingUnit,37,timingUnit,word(batch.bytes.data(),16),word(batch.bytes.data(),8),0,kind);
                if(!healthy)runtime.transport_lost.fetch_add(1);
            }
            if(runtime.head.load(std::memory_order_acquire)==runtime.tail.load(std::memory_order_acquire))WaitForSingleObject(runtime.wake,20);
        }
        runtime.generation.store(0);runtime.deadline.store(0);runtime.drain_generation.store(0);runtime.drain_deadline.store(0);DisconnectNamedPipe(pipe);
        while(take(runtime,batch))runtime.transport_lost.fetch_add(1);
    }
}
}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_BridgeNative_recorderAbiVersion(JNIEnv*,jclass){return 1;}
extern "C" JNIEXPORT jlong JNICALL Java_psx_exporter_BridgeNative_recorderFrequency(JNIEnv*,jclass){LARGE_INTEGER q{};QueryPerformanceFrequency(&q);return q.QuadPart;}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_BridgeNative_recorderThreadId(JNIEnv*,jclass){return static_cast<jint>(GetCurrentThreadId());}
jboolean initialize(JNIEnv* env,jstring metadata,Runtime& runtime,unsigned unit){
    try{
    if(!metadata)return JNI_FALSE;const auto length=env->GetStringUTFLength(metadata);if(length<2||length>4000)return JNI_FALSE;
    unsigned expected=0;if(!runtime.init_state.compare_exchange_strong(expected,1))return runtime.init_state.load()==2?JNI_TRUE:JNI_FALSE;
    runtime.instrument=unit==6;
    const char* text=env->GetStringUTFChars(metadata,nullptr);if(!text){runtime.init_state.store(3);return JNI_FALSE;}
    runtime.metadata.assign(text,static_cast<std::size_t>(length));env->ReleaseStringUTFChars(metadata,text);
    if(!identity(runtime,unit)){runtime.init_state.store(3);return JNI_FALSE;}runtime.wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    HANDLE thread=runtime.wake?CreateThread(nullptr,0,worker,&runtime,0,nullptr):nullptr;
    if(!thread){runtime.init_state.store(3);return JNI_FALSE;}CloseHandle(thread);
    // Worker-side initialization is reported by Publish/Poll; no source waits on pipe creation.
    return JNI_TRUE;
    }catch(...){runtime.init_state.store(3);return JNI_FALSE;}
}
void poll(JNIEnv* env,jobject destination,Runtime& runtime){
    if(!destination)return;
    auto p=env->GetDirectBufferAddress(destination);if(!p||env->GetDirectBufferCapacity(destination)<16)return;
    auto g=runtime.generation.load(),d=runtime.deadline.load();if(ticks()>=d)g=0;
    if(runtime.init_state.load()==3)g=UINT64_MAX;word(p,0,g);word(p,8,d);
    if(env->GetDirectBufferCapacity(destination)>=48){word(p,16,runtime.rejected.load());word(p,24,runtime.transport_lost.load());word(p,32,runtime.drain_generation.load());word(p,40,runtime.drain_deadline.load());}
}
jboolean publish(JNIEnv* env,jobject source,jint length,Runtime& runtime){
    if(!source)return JNI_FALSE;
    auto p=env->GetDirectBufferAddress(source);if(length<0||!p||env->GetDirectBufferCapacity(source)<length||!valid(p,static_cast<std::size_t>(length)))return JNI_FALSE;
    if(runtime.instrument&&integer(p,0)!=3&&(integer(p,0)!=1||integer(p,4)>psx::instrument::frame_limit))return JNI_FALSE;
    if(runtime.init_state.load()==3)return JNI_FALSE;
    if(integer(p,0)!=3){const auto now=ticks(),g=word(p,16);if(!((g==runtime.generation.load()&&g!=0&&now<runtime.deadline.load())||(g==runtime.drain_generation.load()&&g!=0&&now<runtime.drain_deadline.load())))return JNI_FALSE;}
    if(!runtime.initialized.load(std::memory_order_acquire))return JNI_FALSE;
    const auto h=runtime.head.load(std::memory_order_relaxed);if(h-runtime.tail.load(std::memory_order_acquire)>=queue_capacity){runtime.rejected.fetch_add(1);return JNI_FALSE;}
    auto& b=runtime.batches[h%queue_capacity];b.size=static_cast<std::uint32_t>(length);std::memcpy(b.bytes.data(),p,b.size);if(!runtime.instrument&&(integer(p,0)==1||integer(p,0)==2)){const auto unit=unsigned(&runtime-runtimes);auto offset=integer(p,24);auto paint=integer(p,0)==1&&offset==0&&integer(p,28)>=32?word(static_cast<const unsigned char*>(p)+32,24):0;if(offset==0)psx::du::timing::event(6+unit,32,unit,word(p,16),word(p,8),paint,integer(p,0));if(offset+integer(p,28)==integer(p,4))psx::du::timing::event(6+unit,33,unit,word(p,16),word(p,8),paint,integer(p,0));}runtime.head.store(h+1,std::memory_order_release);SetEvent(runtime.wake);return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_BridgeNative_recorderInitialize(JNIEnv* e,jclass,jstring m){return initialize(e,m,runtimes[0],0);}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_BridgeNative_recorderPoll(JNIEnv* e,jclass,jobject b){poll(e,b,runtimes[0]);}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_BridgeNative_recorderPublish(JNIEnv* e,jclass,jobject b,jint n){return publish(e,b,n,runtimes[0]);}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_BridgeNative_duRecorderAbiVersion(JNIEnv*,jclass){return 1;}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_BridgeNative_duRecorderInitialize(JNIEnv* e,jclass,jint u,jstring m){return u>=0&&u<6?initialize(e,m,runtimes[u],unsigned(u)):JNI_FALSE;}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_BridgeNative_duRecorderPoll(JNIEnv* e,jclass,jint u,jobject b){if(u>=0&&u<6)poll(e,b,runtimes[u]);}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_BridgeNative_duRecorderPublish(JNIEnv* e,jclass,jint u,jobject b,jint n){return u>=0&&u<6?publish(e,b,n,runtimes[u]):JNI_FALSE;}

extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_BridgeNative_instrumentRecorderAbiVersion(JNIEnv*,jclass){return 1;}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_BridgeNative_instrumentRecorderInitialize(JNIEnv* e,jclass,jstring m){return initialize(e,m,runtimes[6],6);}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_BridgeNative_instrumentRecorderPoll(JNIEnv* e,jclass,jobject b){poll(e,b,runtimes[6]);}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_BridgeNative_instrumentRecorderPublish(JNIEnv* e,jclass,jobject b,jint n){return publish(e,b,n,runtimes[6]);}
