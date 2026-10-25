#pragma once
#include <psx/norm/diagnostics.hpp>
#include <filesystem>
#include <thread>
namespace psx::du::transport {
// Explicit capture only. Queues are allocated before attaching to producers.
class Trace {
 std::unique_ptr<psx::norm::DiagnosticQueue[]> queues_;unsigned count_;
 std::filesystem::path output_;std::thread worker_;std::atomic<bool> quit_{},failed_{};
 void run();
public:
 Trace(std::filesystem::path,unsigned producers);~Trace();
 Trace(const Trace&)=delete;
 void record(unsigned,psx::norm::Stage,const psx::norm::Correlation&,unsigned unit,uint64_t source_end=0,uint64_t value=0,uint32_t flags=0,uint64_t stamp=0) noexcept;
 void event(unsigned,const psx::norm::DiagnosticRecord&) noexcept;
 // Explicit one-in-thirty payload sample; hash on the ingress/preparation
 // worker, then queue four words. No payload or extra arena is retained.
 void digest(unsigned,const psx::norm::Correlation&,unsigned,std::span<const uint8_t>) noexcept;
 uint64_t lost()const noexcept;bool failed()const noexcept{return failed_;}
};
}
