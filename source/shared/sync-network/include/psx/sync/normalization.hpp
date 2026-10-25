#pragma once
#include <psx/sync/transport.hpp>
#include <psx/terrain/protocol.hpp>
#include <array>
#include <memory>
namespace psx::sync {
// Raw PXN1 payloads remain immutable in transport/Capture. This adapter creates
// a separate, explicitly clock-mapped copy for existing same-host reducers.
// It never replaces a source timestamp with receipt/processing time.
class Normalizer {
 struct QueryEntry {terrain::Query query;bool valid{};};
 std::array<QueryEntry,64> queries_{};unsigned query_cursor_{};
 std::uint32_t sequence_{};
 struct Memo;
 std::unique_ptr<Memo> memo_;
public:
 Normalizer();~Normalizer();
 void sent(Family,std::span<const std::uint8_t>,std::int64_t submitted_qpc=0)noexcept;
 bool received(const Received&,Endpoint&,Frame&)noexcept;
 bool received(const Received&,const LinkContext&,std::int64_t now,Frame&)noexcept;
 bool received(const Received&,const ClockMap&,std::int64_t now,std::uint64_t local_frequency,std::uint64_t remote_frequency,Frame&)noexcept;
 void reset()noexcept;
};
}
