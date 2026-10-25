#include <du_client/failure_evidence.hpp>
#include <windows.h>
#include <sstream>
namespace du_client {
std::string failure_json(const FailureRecord&r){
    constexpr const char* names[]{"admission","decode","replay","presentation"};
    char utf8[2048]{};const auto length=int(wcsnlen_s(r.text.data(),r.text.size()));
    int n=WideCharToMultiByte(CP_UTF8,0,r.text.data(),length,utf8,sizeof utf8,nullptr,nullptr);
    std::string escaped;
    for(int i=0;i<n;++i){auto c=uint8_t(utf8[i]);if(c=='"'||c=='\\'){escaped+='\\';escaped+=char(c);}
        else if(c<32){constexpr char hex[]="0123456789abcdef";escaped+="\\u00";escaped+=hex[c>>4];escaped+=hex[c&15];}
        else escaped+=char(c);}
    std::ostringstream out;
    out<<"{\"type\":\"display_failure\",\"stage\":\""<<names[unsigned(r.stage)]
       <<"\",\"recovery\":"<<(r.recovery?"true":"false")<<",\"revision\":"<<r.revision
       <<",\"qpc\":"<<r.qpc<<",\"epoch\":"<<r.epoch<<",\"sequence\":"<<r.sequence
       <<",\"paint\":"<<r.paint<<",\"generation\":"<<r.generation<<",\"source_pid\":"<<r.source_pid
       <<",\"source_created\":"<<r.source_created<<",\"unit\":"<<r.unit
       <<",\"error\":"<<r.error<<",\"rejected\":"<<r.count<<",\"truncated\":"<<(r.truncated?"true":"false")
       <<",\"message\":\""<<escaped<<"\"}\n";
    return out.str();
}
}
