#include "coordinator/aircraft_evidence.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
namespace coordinator {
namespace w=aircraft_wire;
namespace {
std::uint64_t get(const std::uint8_t* p,std::size_t n){std::uint64_t v{};for(std::size_t i=0;i<n;i++)v|=std::uint64_t(p[i])<<(i*8);return v;}
bool payload(const w::Header& h,const std::uint8_t* p,unsigned type,unsigned role){
 const auto n=h.payload_size;
 switch(h.kind){
 case w::Kind::hello:{w::Hello v;return (type==1||type==5)&&w::decode(p,n,v)&&static_cast<unsigned>(v.role)==role;}
 case w::Kind::sample:{w::Sample v;return w::decode(p,n,v)&&((type==1&&role==2&&v.source==w::Source::native)||((type==2||type==5)&&role==1));}
 case w::Kind::control:{w::Control v;return type==2&&role==1&&w::decode(p,n,v);}
 case w::Kind::ack:{w::Ack v;return type==1&&role==1&&w::decode(p,n,v);}
 case w::Kind::applied:{w::Applied v;return type==1&&role==1&&w::decode(p,n,v);}
 case w::Kind::status:{w::Status v;return type==1&&w::decode(p,n,v);}
 case w::Kind::capture:{w::Capture v;return type==2&&w::decode(p,n,v);}
 default:return false;
 }
}
}
AircraftReplayResult inspect_aircraft_capture(const std::wstring& path){
 AircraftReplayResult r;std::ifstream f(std::filesystem::path(path),std::ios::binary);const auto fail=[&](const char* text){r.error=text;return r;};
 std::array<std::uint8_t,24> head{};if(!f.read(reinterpret_cast<char*>(head.data()),head.size())||std::memcmp(head.data(),"PXACA1\0\0",8))return fail("Invalid aircraft sidecar version1 header");
 r.qpc_frequency=get(head.data()+8,8);r.begin_qpc=get(head.data()+16,8);if(!r.qpc_frequency||r.qpc_frequency>1000000000000ULL)return fail("Invalid aircraft clock");
 std::array<std::uint8_t,w::max_frame> bytes{};
 for(;;){std::array<std::uint8_t,16> record{};if(!f.read(reinterpret_cast<char*>(record.data()),record.size()))return fail("Missing aircraft terminal");const auto time=get(record.data(),8),size=get(record.data()+12,4);const auto type=record[8],role=record[9];if(record[10]||record[11])return fail("Nonzero aircraft reserved fields");
 if(type==3||type==4){if(size||role||time<r.begin_qpc||f.peek()!=std::char_traits<char>::eof())return fail("Invalid aircraft cutoff");r.cutoff_qpc=time;r.valid=true;r.complete=type==3;return r;}
 if((type!=1&&type!=2&&type!=5)||(role!=1&&role!=2)||size<w::header_size||size>bytes.size()||!f.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size)))return fail("Invalid aircraft record envelope");
 w::Header h;if(!w::read_header(bytes.data(),w::header_size,h)||!h.sequence||h.payload_size+w::header_size!=size||!payload(h,bytes.data()+w::header_size,type,role))return fail("Invalid aircraft frame/role");
 ++r.records;++r.kinds[static_cast<std::size_t>(h.kind)];
 }
}
}
