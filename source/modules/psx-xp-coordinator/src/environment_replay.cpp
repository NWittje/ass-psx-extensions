#include "coordinator/environment_evidence.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
namespace coordinator {
namespace w=environment_wire;
namespace v2=presentation_wire;
namespace {
std::uint64_t get(const std::uint8_t* p,std::size_t n){std::uint64_t v{};for(std::size_t i=0;i<n;i++)v|=std::uint64_t(p[i])<<(i*8);return v;}
bool payload(const w::Header& h,const std::uint8_t* p,unsigned type,unsigned role){
 const auto n=h.payload_size;
 switch(h.kind){
 case w::Kind::hello:{w::Hello v;return (type==1||type==5)&&w::decode(p,n,v)&&static_cast<unsigned>(v.role)==role;}
 case w::Kind::sample:{w::Sample v;return w::decode(p,n,v)&&((type==1&&role==2)||((type==2||type==5)&&role==1));}
 case w::Kind::control:{w::Control v;return type==2&&role==1&&w::decode(p,n,v);}
 case w::Kind::ack:{w::Ack v;return type==1&&role==1&&w::decode(p,n,v);}
 case w::Kind::applied:{w::Applied v;return (type==1||type==5)&&role==1&&w::decode(p,n,v);}
 case w::Kind::status:{w::Status v;return type==1&&w::decode(p,n,v);}
 case w::Kind::capture:{w::Capture v;return type==2&&w::decode(p,n,v);}
 default:return false;
 }
}
}
EnvironmentReplayResult inspect_environment_capture(const std::wstring& path){
 EnvironmentReplayResult r;std::ifstream f(std::filesystem::path(path),std::ios::binary);const auto fail=[&](const char* text){r.error=text;return r;};
 std::array<std::uint8_t,24> head{};if(!f.read(reinterpret_cast<char*>(head.data()),head.size())||std::memcmp(head.data(),"PXWCA1\0\0",8))return fail("Invalid environment sidecar version1 header");
 r.qpc_frequency=get(head.data()+8,8);r.begin_qpc=get(head.data()+16,8);if(!r.qpc_frequency||r.qpc_frequency>1000000000000ULL)return fail("Invalid environment clock");
 std::array<std::uint8_t,w::max_frame> bytes{};std::uint64_t latest_record_qpc=r.begin_qpc;
 for(;;){std::array<std::uint8_t,16> record{};if(!f.read(reinterpret_cast<char*>(record.data()),record.size()))return fail("Missing environment terminal");const auto time=get(record.data(),8),size=get(record.data()+12,4);const auto type=record[8],role=record[9];if(record[10]||record[11])return fail("Nonzero environment reserved fields");
 if(type==3||type==4){if(size||role||time<latest_record_qpc||f.peek()!=std::char_traits<char>::eof())return fail("Invalid environment cutoff");r.cutoff_qpc=time;r.valid=true;r.complete=type==3;return r;}
 if((type!=1&&type!=2&&type!=5)||(role!=1&&role!=2)||size<w::header_size||size>bytes.size()||!f.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size)))return fail("Invalid environment record envelope");
 w::Header h;v2::Header h2;bool qualified=false;const bool extended=v2::read_header(bytes.data(),w::header_size,h2);
 if(extended){h.kind=static_cast<w::Kind>(h2.kind);h.sequence=h2.sequence;h.payload_size=h2.payload_size;const auto whole=std::span<const std::uint8_t>(bytes.data()+w::header_size,static_cast<std::size_t>(size)-w::header_size);
  switch(h2.kind){
  case v2::Kind::hello:{v2::Hello v;qualified=(type==1||type==5)&&v2::decode(whole,v)&&static_cast<unsigned>(v.base.role)==role;break;}
  case v2::Kind::source:{v2::Source v;qualified=(type==1||type==5)&&role==2&&v2::decode(whole,v);break;}
  case v2::Kind::presentation:{v2::Presentation v;qualified=(type==2||type==5)&&role==1&&v2::decode(whole,v);break;}
  case v2::Kind::applied_presentation:{v2::AppliedPresentation v;qualified=(type==1||type==5)&&role==1&&v2::decode(whole,v);break;}
  case v2::Kind::sample:case v2::Kind::control:case v2::Kind::ack:case v2::Kind::applied:case v2::Kind::status:case v2::Kind::capture:qualified=payload(h,bytes.data()+w::header_size,type,role);break;
  default:break;
  }
 }else qualified=w::read_header(bytes.data(),w::header_size,h)&&payload(h,bytes.data()+w::header_size,type,role);
 if(!qualified||!h.sequence||h.payload_size+w::header_size!=size||static_cast<std::size_t>(h.kind)>=r.kinds.size())return fail("Invalid environment frame/role");
 if(time<r.begin_qpc)return fail("Environment record precedes Capture");if(time>latest_record_qpc)latest_record_qpc=time;++r.records;++r.kinds[static_cast<std::size_t>(h.kind)];
 }
}
}
