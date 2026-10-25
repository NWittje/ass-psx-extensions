#include "coordinator/terrain_replay.hpp"
#include <psx/terrain/protocol.hpp>
#include <psx/exporter/protocol.hpp>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
namespace coordinator {
namespace {
namespace t=psx::terrain;namespace e=psx::exporter;
std::uint64_t get(const std::uint8_t* bytes,std::size_t count){std::uint64_t value{};for(std::size_t n=0;n<count;++n)value|=std::uint64_t(bytes[n])<<(n*8);return value;}
bool role_allowed(unsigned type,unsigned role,unsigned kind,const std::uint8_t* bytes,std::size_t size){
    if(type==1){
        if(kind==1){t::Hello hello;return t::decode(bytes,size,hello)&&static_cast<unsigned>(hello.role)==role;}
        return kind==6||(role==1&&(kind==4||kind==8||kind==9||kind==11))||(role==2&&(kind==7||kind==10||kind==13||kind==14));
    }
    return kind==2||kind==5||kind==12||(role==1&&kind==3)||(role==2&&kind==4);
}
}
TerrainReplayResult inspect_terrain_capture(const std::wstring& path){
    TerrainReplayResult result;std::ifstream file(std::filesystem::path(path),std::ios::binary);
    const auto fail=[&](const char* error){result.error=error;return result;};
    if(!file)return fail("Cannot open terrain sidecar");
    std::array<std::uint8_t,24> header{};if(!file.read(reinterpret_cast<char*>(header.data()),header.size())||std::memcmp(header.data(),"PXTCA1\0\0",8))return fail("Invalid PXTCA1 header");
    result.qpc_frequency=get(header.data()+8,8);result.begin_qpc=get(header.data()+16,8);
    if(!result.qpc_frequency||result.qpc_frequency>1000000000000ULL)return fail("Invalid terrain QPC frequency");
    std::array<std::uint8_t,(std::max)(t::max_frame,e::max_frame)> frame{};
    for(;;){
        std::array<std::uint8_t,16> record{};if(!file.read(reinterpret_cast<char*>(record.data()),record.size()))return fail("Missing or truncated terrain terminal record");
        const auto type=record[8],role=record[9];const auto size=get(record.data()+12,4);
        if(record[10]||record[11])return fail("Nonzero terrain record reserved fields");
        if(type==3||type==4){
            if(size||role)return fail("Invalid terrain terminal record");
            result.cutoff_qpc=get(record.data(),8);if(result.cutoff_qpc<result.begin_qpc)return fail("Terrain cutoff precedes capture start");
            if(file.peek()!=std::char_traits<char>::eof())return fail("Data follows terrain terminal record");
            result.valid=true;result.complete=type==3;return result;
        }
        if((type!=1&&type!=2&&type!=5)||(role!=1&&role!=2)||size<16||size>frame.size())return fail("Invalid terrain record envelope");
        if(!file.read(reinterpret_cast<char*>(frame.data()),static_cast<std::streamsize>(size)))return fail("Truncated terrain payload");
        if(type==5){e::Header raw;e::Snapshot snapshot;
            if(role!=2||!e::read_header(frame.data(),16,raw)||raw.kind!=e::Kind::snapshot||16+raw.payload_size!=size||!e::decode(frame.data()+16,raw.payload_size,snapshot))return fail("Invalid raw PXSE terrain evidence");
            ++result.raw_snapshots;
        }else{
            t::Header h;std::uint16_t version{};
            if(!t::read_capture_header(frame.data(),16,h,version))return fail("Invalid versioned terrain header");
            const auto kind=static_cast<unsigned>(h.kind);
            if(h.payload_size+16!=size||size>t::max_frame||version>=result.versions.size()||kind>=result.kinds.size()||!t::validate_capture_payload(h,frame.data()+16,static_cast<std::size_t>(size-16),version)||!role_allowed(type,role,kind,frame.data()+16,static_cast<std::size_t>(size-16)))return fail("Invalid versioned terrain payload/role");
            ++result.versions[version];++result.kinds[kind];
        }
        ++result.records;
    }
}
}
