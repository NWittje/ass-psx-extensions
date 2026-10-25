#pragma once
#include "flightillusion.hpp"

namespace hwio::flightillusion {
struct ExporterRequest {
    std::string host;
    std::uint16_t port{};
    std::uint64_t epoch{};
    bool active{};
    bool operator==(const ExporterRequest&) const = default;
};
struct ExporterSample {
    std::optional<double> knots;
    std::uint64_t sampled_unix_ms{};
    std::string notice;
};
// Main-associated metadata is read off the hardware service thread. A changed
// endpoint, connection/load epoch or enable state invalidates in-flight reads.
class ExporterSource {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    using Reader=std::function<ExporterSample(const ExporterRequest&,std::uint64_t unix_ms)>;
    explicit ExporterSource(Reader={},bool threaded=true);
    ~ExporterSource();
    void configure(ExporterRequest);
    void apply(Sources&,std::uint64_t unix_ms,std::uint64_t monotonic_ms) const;
    // Deterministic fixture seam; use only with threaded=false.
    void pump(std::uint64_t unix_ms,std::uint64_t monotonic_ms);
};
}
