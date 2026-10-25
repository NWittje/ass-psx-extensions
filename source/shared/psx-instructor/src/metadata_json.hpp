#pragma once
#include <string>
#include <string_view>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
namespace psx::instructor::metadata {
struct Json {
  std::string_view s; std::size_t at{};
  [[noreturn]] static void bad(const char *reason) { throw std::invalid_argument(reason); }
  void ws() { while (at < s.size() && (s[at]==' '||s[at]=='\t'||s[at]=='\r'||s[at]=='\n')) ++at; }
  bool take(char c) { ws(); if (at<s.size() && s[at]==c) { ++at; return true; } return false; }
  void expect(char c) { if (!take(c)) bad("malformed exporter metadata"); }
  unsigned hex() {
    unsigned v{};
    for (int k=0;k<4;++k) {
      if (at==s.size()) bad("truncated JSON escape");
      char c=s[at++]; unsigned d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;
      if (d==16) bad("invalid JSON escape"); v=v*16+d;
    } return v;
  }
  std::string str() {
    expect('"'); std::string out;
    while (at<s.size()) {
      unsigned char c=static_cast<unsigned char>(s[at++]);
      if (c=='"') return out;
      if (c<32) bad("invalid JSON string");
      if (c!='\\') { out+=static_cast<char>(c); continue; }
      if (at==s.size()) bad("truncated JSON escape");
      char e=s[at++];
      switch (e) {
        case '"': case '\\': case '/': out+=e; break;
        case 'b': out+='\b'; break; case 'f': out+='\f'; break;
        case 'n': out+='\n'; break; case 'r': out+='\r'; break; case 't': out+='\t'; break;
        case 'u': {
          unsigned v=hex();
          if (v>=0xd800 && v<=0xdbff) {
            if (at+2>s.size() || s.substr(at,2)!="\\u") bad("invalid surrogate pair");
            at+=2; unsigned low=hex(); if (low<0xdc00||low>0xdfff) bad("invalid surrogate pair");
            v=0x10000+((v-0xd800)<<10)+(low-0xdc00);
          } else if (v>=0xdc00&&v<=0xdfff) bad("invalid surrogate pair");
          if (v<0x80) out+=static_cast<char>(v);
          else if (v<0x800) { out+=static_cast<char>(0xc0|(v>>6)); out+=static_cast<char>(0x80|(v&63)); }
          else if (v<0x10000) { out+=static_cast<char>(0xe0|(v>>12)); out+=static_cast<char>(0x80|((v>>6)&63)); out+=static_cast<char>(0x80|(v&63)); }
          else { out+=static_cast<char>(0xf0|(v>>18)); out+=static_cast<char>(0x80|((v>>12)&63)); out+=static_cast<char>(0x80|((v>>6)&63)); out+=static_cast<char>(0x80|(v&63)); }
          break;
        }
        default: bad("invalid JSON escape");
      }
    } bad("unterminated JSON string");
  }
  std::uint64_t number() {
    ws(); auto begin=at; while (at<s.size()&&s[at]>='0'&&s[at]<='9') ++at;
    std::uint64_t result{}; auto r=std::from_chars(s.data()+begin,s.data()+at,result);
    if (begin==at || (at-begin>1&&s[begin]=='0') || r.ec!=std::errc{}) bad("invalid JSON integer");
    return result;
  }
  double real() {
    ws();const auto start=at;while(at<s.size()&&s[at]!=','&&s[at]!='}'&&s[at]!=' '&&s[at]!='\t'&&s[at]!='\r'&&s[at]!='\n')++at;
    auto token=s.substr(start,at-start);auto digits=token.starts_with('-')?token.substr(1):token;
    if(digits.empty()||(digits.size()>1&&digits[0]=='0'&&digits[1]!='.'&&digits[1]!='e'&&digits[1]!='E'))bad("invalid JSON number");
    double value{};auto result=std::from_chars(token.data(),token.data()+token.size(),value);
    if(result.ec!=std::errc{}||result.ptr!=token.data()+token.size()||!std::isfinite(value))bad("invalid JSON number");return value;
  }
};
}
