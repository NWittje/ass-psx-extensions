#include <psx/display/diagnostics.hpp>
#include <fstream>
#include <iomanip>
#include <bcrypt.h>
#include <cstring>
namespace psx::du::transport {
Trace::Trace(std::filesystem::path output,unsigned producers):count_(producers),output_(std::move(output)) {
 if(!count_||count_>9)throw std::runtime_error("Diagnostic producer bound");
 queues_=std::make_unique<psx::norm::DiagnosticQueue[]>(count_);
 std::filesystem::create_directories(output_);worker_=std::thread(&Trace::run,this);
}
Trace::~Trace(){quit_=true;worker_.join();}
void Trace::record(unsigned producer,psx::norm::Stage stage,const psx::norm::Correlation&token,unsigned unit,uint64_t end,uint64_t value,uint32_t flags,uint64_t stamp) noexcept {
 event(producer,{token,stamp?stamp:psx::norm::diagnostic_qpc(),value,end,GetCurrentThreadId(),flags,uint16_t(unit),uint16_t(stage),0});
}
void Trace::event(unsigned producer,const psx::norm::DiagnosticRecord&r)noexcept {if(producer<count_)queues_[producer].push(r);}
void Trace::digest(unsigned producer,const psx::norm::Correlation&token,unsigned unit,std::span<const uint8_t>bytes) noexcept {
 if(token.frame%30)return;
 std::array<uint8_t,32> digest{};
 if(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,const_cast<PUCHAR>(bytes.data()),ULONG(bytes.size()),digest.data(),ULONG(digest.size()))<0){failed_=true;return;}
 for(unsigned word=0;word<4;++word){uint64_t value;std::memcpy(&value,digest.data()+8*word,8);record(producer,psx::norm::Stage::payload_digest,token,unit,0,value,word);}
}
uint64_t Trace::lost()const noexcept {uint64_t result{};for(unsigned i=0;i<count_;++i)result+=queues_[i].lost.load();return result;}
void Trace::run(){try{
 std::ofstream out(output_/L"stages.jsonl");if(!out)throw std::runtime_error("Diagnostic output");uint64_t records{},validated{};
 LARGE_INTEGER f{};QueryPerformanceFrequency(&f);
 for(;;){bool any=false;for(unsigned i=0;i<count_;++i){psx::norm::DiagnosticRecord r;unsigned bounded=0;
  while(bounded++<4096&&queues_[i].pop(r)){any=true;++records;if(r.stage==uint16_t(psx::norm::Stage::validated))++validated;
   out<<"{\"producer\":"<<i<<",\"session\":\"";for(auto b:r.token.session)out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(b);
   out<<std::dec<<"\",\"unit\":"<<r.unit<<",\"generation\":"<<r.token.generation<<",\"publication\":"<<r.token.publication<<",\"source_frame\":"<<r.token.frame<<",\"stage\":"<<r.stage<<",\"qpc\":"<<r.stamp<<",\"source_end\":"<<r.source_end<<",\"value\":"<<r.value<<",\"flags\":"<<r.flags<<",\"thread\":"<<r.thread<<",\"connection\":"<<r.reserved<<"}\n";
  }
 }if(!out)throw std::runtime_error("Diagnostic write");if(quit_&&!any)break;if(!any)Sleep(5);}
 out.close();if(!out)throw std::runtime_error("Diagnostic flush");
 std::ofstream summary(output_/L"stages-summary.json");summary<<"{\"schema\":2,\"record_bytes\":80,\"capacity_per_producer\":4096,\"producers\":"<<count_<<",\"allocated_queue_bytes\":"<<sizeof(psx::norm::DiagnosticQueue)*count_<<",\"records\":"<<records<<",\"validated_deliveries\":"<<validated<<",\"diagnostic_losses\":"<<lost()<<",\"qpc_frequency\":"<<f.QuadPart<<",\"sampling\":\"every emitted stage; overflow counted\"}\n";if(!summary)throw std::runtime_error("Diagnostic summary");
}catch(...){failed_=true;}}
}
