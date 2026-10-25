#include "coordinator/relocation.hpp"
#include <algorithm>
namespace coordinator::relocation {
bool same_reference(const psx::terrain::Reference& a,const psx::terrain::Reference& b) noexcept {
 return a.producer==b.producer&&a.pose_epoch==b.pose_epoch&&a.sequence==b.sequence&&a.frame_sequence==b.frame_sequence&&
 a.sample_qpc==b.sample_qpc&&a.received_qpc==b.received_qpc&&a.latitude_deg==b.latitude_deg&&a.longitude_deg==b.longitude_deg&&
 a.altitude_msl_m==b.altitude_msl_m&&a.heading_rad==b.heading_rad&&a.pitch_rad==b.pitch_rad&&a.bank_left_rad==b.bank_left_rad&&
 a.flags==b.flags&&a.main_strut_ft==b.main_strut_ft&&a.reference_revision==b.reference_revision;
}
bool Model::resume(const psx::terrain::Reference& ref,std::uint64_t now,Resume& out) noexcept {
 if(stage_!=Stage::bound||exhausted_||release_||!follow_||!serial(now)||!psx::terrain::valid(ref)||ref.producer!=binding_.writer.domain.producer||
 ref.pose_epoch!=resolution_.epoch||ref.sequence<=binding_.source_sequence_floor||ref.sample_qpc<=0||
 static_cast<std::uint64_t>(ref.sample_qpc)<binding_.source_qpc_floor||static_cast<std::uint64_t>(ref.sample_qpc)>now||
 ref.received_qpc<ref.sample_qpc||static_cast<std::uint64_t>(ref.received_qpc)>now)return false;
 const auto elapsed=now-static_cast<std::uint64_t>(ref.sample_qpc);
 if(elapsed>frequency_/10||(elapsed==frequency_/10&&frequency_%10==0))return false;
 pending_={prepare_.admission,binding_.serial,intent_,true,binding_.writer.domain,ref,binding_.control_floor,binding_.query_floor,binding_.transport_floor};
 out=pending_;stage_=Stage::resuming;return true;
}
bool Model::acknowledgment(const ResumeAck& a,std::uint64_t now) noexcept {
 const auto& r=a.request;
 if(!a.accepted||r.admission!=pending_.admission||r.binding!=pending_.binding||r.current_intent!=pending_.current_intent||
 r.current_follow!=pending_.current_follow||r.domain!=pending_.domain||!same_reference(r.reference,pending_.reference)||
 r.control_floor!=pending_.control_floor||r.query_floor!=pending_.query_floor||r.transport_floor!=pending_.transport_floor||
 !serial(a.observation_sequence)||a.observation_sequence<=binding_.observation_sequence||!serial(a.observed_qpc)||
 a.observed_qpc<(std::max)({binding_.observed_qpc,static_cast<std::uint64_t>(pending_.reference.sample_qpc),
 static_cast<std::uint64_t>(pending_.reference.received_qpc)})||a.observed_qpc>now)return false;
 if(stage_!=Stage::resuming||!follow_||intent_!=r.current_intent){if(!release_)release_request_=r;release_=true;return false;}
 stage_=Stage::active;return true; // Native acceptance is still distinct from actual Consumed.
}
}
