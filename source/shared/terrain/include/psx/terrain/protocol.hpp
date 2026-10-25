#pragma once
#include <psx/motion/full_pose.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

// PXT3 is independent of wire6/PXSE2/CAP7. Native structs are never transmitted.
namespace psx::terrain {
inline constexpr std::uint16_t protocol_version=3;
inline constexpr std::uint32_t terrain_jni_abi=3;
inline constexpr std::size_t header_size=16,max_payload=1024,max_frame=header_size+max_payload;
inline constexpr std::uint32_t blend_ms=250,stale_ms=100,renew_ms=250,lease_ms=1000;
inline constexpr char plugin_pipe_prefix[]="\\\\.\\pipe\\psx-ios.terrain.plugin.v3."; // SID
inline constexpr char exporter_pipe_prefix[]="\\\\.\\pipe\\psx-ios.terrain.exporter.v3."; // SID.pid.creation
enum class Role : std::uint32_t {plugin=1,exporter=2};
enum Capability : std::uint32_t {surface_query=1,native_support=2,assembly_diagnostics=4,probe_details=8,support_details=16,render_details=32,footprint_surface=64,constraint_details=128,frame_details=256};
enum class Kind : std::uint16_t {hello=1,subscribe=2,query=3,surface=4,control=5,ack=6,consumed=7,diagnostics=8,probe_detail=9,support_detail=10,render_detail=11,capture=12,constraint_detail=13,frame_detail=14};
enum class Action : std::uint32_t {acquire=1,renew=2,release=3};
enum class Phase : std::uint32_t {unavailable=0,native=1,blending=2,active=3,held=4};
enum class Reason : std::uint32_t {none=0,unsupported=1,identity=2,epoch=3,stale=4,invalid_surface=5,owner=6,expired=7,stopped=8,disconnected=9,overflow=10,scenery=11,local_frame=12,loading=13};
enum SurfaceFlag : std::uint32_t {hit=1,wet=2,moving=4,geometry_qualified=8,footprint_plane=16};
inline constexpr std::uint32_t footprint_geometry_revision=1;
inline constexpr std::uint32_t no_assembly=0xffffffffu;
// First 128 bits of the qualified ACF SHA-256, high then low hex words.
inline constexpr motion::InstanceId sparky_footprint_geometry_id{0x19D499D63793C549ULL,0x2AC22158748C8E61ULL};
struct Header {Kind kind{Kind::hello};std::uint32_t payload_size{},sequence{};};
struct Hello {
    Role role{Role::plugin};motion::InstanceId instance{};
    std::uint32_t pid{},capabilities{};
    std::uint64_t process_creation_filetime{},qpc_frequency{};
};
struct Subscribe {motion::InstanceId instance{};std::uint64_t owner{},qpc_frequency{};std::uint32_t pid{};};
struct Reference {
    motion::InstanceId producer{};
    std::uint64_t pose_epoch{},sequence{},frame_sequence{};
    std::int64_t sample_qpc{},received_qpc{};
    // Native Aerodynamics realLat/realLon reference, explicitly not labelled CG.
    double latitude_deg{},longitude_deg{},altitude_msl_m{},heading_rad{};
    double pitch_rad{},bank_left_rad{};
    std::uint32_t flags{};
    // Exact native mainGearStrut term in the revision2 eye recipe, not Aerodynamics.strut.
    double main_strut_ft{};
    std::uint32_t reference_revision{motion::instant_reference_revision};
};
struct Query {std::uint64_t owner{},terrain_epoch{},query_id{};Reference reference{};};
// Nominal deployed assembly-centre station, not a measured PSX contact point/load.
// Stable index order nose/bodyR/bodyL/wingR/wingL.
struct SurfaceStation {
    double latitude_deg{},longitude_deg{},height_msl_m{};
    double east_m{},north_m{}; // Same geographic ENU basis at native Reference for the whole bundle.
    double normal_east{},normal_north{},normal_up{1},plane_residual_m{};
    std::int64_t query_start_qpc{},query_end_qpc{};
};
struct Surface {
    Query query{};motion::InstanceId provider{};
    std::uint64_t aircraft_generation{},scenery_generation{},local_frame_generation{};
    std::int64_t query_start_qpc{},query_end_qpc{};
    double latitude_deg{},longitude_deg{},height_msl_m{};
    // Unit normal in geographic east/north/up, not SDK local coordinates.
    double normal_east{},normal_north{},normal_up{1};
    std::uint32_t flags{};
    motion::InstanceId geometry_id{}; // Hash-qualified fixed profile geometry identity.
    std::uint32_t geometry_revision{},station_mask{};
    std::array<SurfaceStation,5> stations{};
};
struct Control {
    motion::InstanceId producer{};
    std::uint64_t owner{},operation{},terrain_epoch{},pose_epoch{};
    Action action{Action::acquire};std::uint32_t duration_ms{lease_ms};
};
struct Ack {Control control{};Phase phase{Phase::native};Reason reason{Reason::none};std::uint64_t accepted_query{};};
struct Consumed {
    motion::InstanceId producer{};std::uint64_t terrain_epoch{},pose_epoch{},frame_sequence{},query_id{},owner{};
    std::int64_t consumed_qpc{},surface_qpc{};
    Phase phase{Phase::native};Reason reason{Reason::none};
    double latitude_deg{},longitude_deg{},effective_height_msl_m{};
    double normal_east{},normal_north{},normal_up{1};
    double support_pitch_rad{},support_bank_left_rad{},strut_ft{},nose_osc_rad{},nose_compress_rad{};
    std::uint32_t support_configuration{},contact_flags{};
    std::uint64_t applied{},rejected{},hook_ticks{};
    std::int64_t source_sample_qpc{}; // Original physical query source; surface_qpc is probe completion.
};
struct Frame {std::array<std::uint8_t,max_frame> bytes{};std::size_t size{};};
// Index order nose/bodyR/bodyL/wingR/wingL. No contact force or invented strut state.
struct AssemblyClearance {
    std::int64_t query_start_qpc{},query_end_qpc{};
    double support_point_msl_m{},surface_msl_m{},clearance_m{};
    Reason reason{Reason::unsupported};
};
struct Diagnostics {
    Query query{};motion::InstanceId provider{};
    std::uint64_t aircraft_generation{},scenery_generation{},local_frame_generation{};
    std::uint32_t valid_mask{};std::array<AssemblyClearance,5> assemblies{};
};
// Diagnostics are independent of the support Surface and cannot grant ownership.
enum class ProbePurpose : std::uint32_t {primary=0,reference_check=1,camera_ground=2};
struct ProbeAttempt {
    std::int64_t query_start_qpc{},query_end_qpc{};
    std::uint32_t sdk_result{2},flags{}; // bit0 hit, bit1 qualified
    Reason reason{Reason::none};
    double seed_msl_m{};
    motion::Vec3 start_local{},hit_local{},normal_local{};
    double hit_msl_m{},height_residual_m{},horizontal_residual_m{};
};
struct ProbeDetail {
    Query query{};motion::InstanceId provider{};
    std::uint64_t aircraft_generation{},scenery_generation{},local_frame_generation{};
    std::uint64_t capture_generation{};
    ProbePurpose purpose{ProbePurpose::primary};std::uint32_t count{},valid_mask{};
    double latitude_deg{},longitude_deg{};
    std::array<ProbeAttempt,4> attempts{};
    motion::InstanceId geometry_id{};
    std::uint32_t geometry_revision{},assembly_index{no_assembly};
};
enum class NativePhase : std::uint32_t {unavailable=0,initialization=1,paused=2,ordinary=3};
struct SupportSite {
    double latitude_deg{},longitude_deg{},constraint_ft{},plane_ft{},ordinary_strut_ft{};
};
struct SupportDetail {
    motion::InstanceId producer{};
    std::uint64_t pose_epoch{},terrain_epoch{},frame_sequence{},query_id{},owner{};
    std::uint64_t capture_generation{};
    std::int64_t step_start_qpc{},step_end_qpc{};
    NativePhase native_phase{NativePhase::unavailable};
    std::uint32_t flags{},valid_mask{},count{}; // flags: bit0 coherent, bit1 external plane; slots: two paused, six ordinary
    double entry_elev_ft{},shadow_elev_ft{},cached_support_ft{},altitude_damper_ft{};
    std::uint64_t begin_ticks{},step_ticks{},helper_ticks{}; // step includes native dynamics; helpers are separately timed
    std::array<SupportSite,8> sites{};
};
struct RenderDetail {
    Query query{};motion::InstanceId provider{};
    std::uint64_t aircraft_generation{},scenery_generation{},local_frame_generation{};
    std::uint64_t capture_generation{};
    std::int64_t sample_qpc{};
    std::uint32_t valid_mask{}; // bit0 aircraft, bit1 camera; active camera only, not every projector
    motion::Vec3 aircraft_local{},aircraft_angles_deg{},camera_local{},camera_angles_deg{}; // angles pitch/heading/right bank
    double camera_latitude_deg{},camera_longitude_deg{},camera_altitude_msl_m{};
};
struct Capture {std::uint64_t owner{},generation{};std::uint32_t enabled{};};
// Capture-only actual reads of tagged cached support constraints; site IDs are
// pinned by the installed instruction inventory, never inferred from final pose.
struct ConstraintSite {
    std::uint32_t field{},flags{}; // field 0=down/1=up; flags bit0=plane basis available.
    double cached_world_rad{},stored_basis_rad{},current_basis_rad{},returned_world_rad{};
};
struct ConstraintDetail {
    motion::InstanceId producer{};
    std::uint64_t pose_epoch{},terrain_epoch{},frame_sequence{},query_id{},owner{},capture_generation{};
    std::int64_t step_start_qpc{},step_end_qpc{};
    std::uint32_t valid_mask{},count{};
    std::array<ConstraintSite,16> sites{};
};
// Stage order: frame entry, dynamics begin/end, publish begin/end, Boost begin/end, frame exit.
struct FrameDetail {
    motion::InstanceId producer{};
    std::uint64_t pose_epoch{},frame_sequence{},capture_generation{},java_thread_id{};
    std::uint32_t valid_mask{},flags{},os_thread_id{}; // bit0 exceptional frame exit.
    double delta_ms{},delta_acc_ms{};
    std::array<std::int64_t,8> stage_qpc{};
};
bool valid(const Reference&) noexcept;
bool valid(const Surface&) noexcept;
bool qualified_footprint(const Surface&) noexcept;
bool fit_footprint(Surface&) noexcept;
bool reference_eye(const Reference&,motion::GeoPose&) noexcept;
struct GeographicOffset {double east_m{},north_m{};};
// Native support-plane metre convention: radius6378137, anchor-latitude cosine,
// wrapped longitude. SDK station mapping is first converted to geographic coordinates.
bool geographic_offset(double anchor_latitude_deg,double anchor_longitude_deg,double latitude_deg,double longitude_deg,GeographicOffset&) noexcept;
bool read_header(const std::uint8_t*,std::size_t,Header&) noexcept;
// Historical envelope inspection only. Never use this for a live connection.
bool read_capture_header(const std::uint8_t*,std::size_t,Header&,std::uint16_t& version) noexcept;
bool validate_capture_payload(const Header&,const std::uint8_t*,std::size_t,std::uint16_t version) noexcept;
#define PSX_TERRAIN_CODEC(T) bool encode(const T&,std::uint32_t,Frame&) noexcept; bool decode(const std::uint8_t*,std::size_t,T&) noexcept;
PSX_TERRAIN_CODEC(Hello)
PSX_TERRAIN_CODEC(Subscribe)
PSX_TERRAIN_CODEC(Query)
PSX_TERRAIN_CODEC(Surface)
PSX_TERRAIN_CODEC(Control)
PSX_TERRAIN_CODEC(Ack)
PSX_TERRAIN_CODEC(Consumed)
PSX_TERRAIN_CODEC(Diagnostics)
PSX_TERRAIN_CODEC(ProbeDetail)
PSX_TERRAIN_CODEC(SupportDetail)
PSX_TERRAIN_CODEC(RenderDetail)
PSX_TERRAIN_CODEC(Capture)
PSX_TERRAIN_CODEC(ConstraintDetail)
PSX_TERRAIN_CODEC(FrameDetail)
#undef PSX_TERRAIN_CODEC
using FrameHandler=bool (*)(void*,const Header&,const std::uint8_t*,std::size_t);
class Framer {
public:
    bool feed(const void*,std::size_t,FrameHandler,void*) noexcept;
    void reset() noexcept {used_=0;needed_=header_size;failed_=false;}
    bool failed() const noexcept {return failed_;}
private:
    std::array<std::uint8_t,max_frame> bytes_{};
    std::size_t used_{},needed_{header_size};bool failed_{};
};
} // namespace psx::terrain
