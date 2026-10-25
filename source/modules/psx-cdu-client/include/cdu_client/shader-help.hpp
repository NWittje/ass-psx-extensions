#pragma once
#include <string_view>

namespace cdu_client
{
struct ShaderHelp
{
    std::string_view id;
    const wchar_t *text;
};
// Authored summaries of the pinned CRT-Royale user-settings.h sections GAMMA,
// LEVELS MANAGEMENT, HALATION/DIFFUSION/BLOOM, SCANLINES, ANTIALIASING,
// PHOSPHOR MASK and GEOMETRY. IDs/ranges/defaults remain the audited registry's.
// Upstream: libretro/slang-shaders@0b3ff0b240f82f4cb5e5d0117f8f2080e2764cf7.
inline constexpr ShaderHelp shader_help[]{
    {"crt_gamma", L"Gamma response of the simulated CRT. Controls how input levels become simulated light."},
    {"lcd_gamma", L"Gamma of your physical display, used to encode the final shader output."},
    {"levels_contrast", L"Multiplies the final image level. High values can clip bright text."},
    {"halation_weight", L"Amount of broad phosphor glow from simulated electron scattering under the CRT glass."},
    {"diffusion_weight", L"Amount of light spread caused by refraction through the simulated CRT glass."},
    {"bloom_underestimate_levels", L"Brightness estimate used for phosphor bloom. Lower values sharpen phosphors but can clip; higher values soften them."},
    {"bloom_excess", L"Adds extra colour blur to make phosphor bloom softer."},
    {"beam_min_sigma", L"Width of the darkest scanline beams, in scanline units. Larger values widen and soften them; must not exceed Max Sigma."},
    {"beam_max_sigma", L"Width of the brightest scanline beams, in scanline units. Larger values widen and soften them; must not be below Min Sigma."},
    {"beam_spot_power", L"Controls how beam width grows with brightness. Lower powers give smoother, less sharp spots."},
    {"beam_min_shape", L"Shape of dark scanline beams. 2 is Gaussian; larger values flatten the centre and sharpen the edges. Must not exceed Max Shape."},
    {"beam_max_shape", L"Shape of bright scanline beams. Larger values make wider plateaus and steeper edges, which may alias."},
    {"beam_shape_power", L"Controls how beam shape changes with brightness. Higher powers usually look softer; lower powers look sharper."},
    {"beam_horiz_filter", L"Horizontal scanline filter: 0 = fast Quilez, 1 = adjustable Gaussian, 2 = sharp Lanczos2."},
    {"beam_horiz_sigma", L"Width of the horizontal Gaussian filter. Used when Horiz Filter is 1; larger values soften the image."},
    {"beam_horiz_linear_rgb_weight", L"Horizontal colour mixing: 0 uses darker gamma-encoded RGB; 1 mixes light in linear RGB. Intermediate values blend both."},
    {"convergence_offset_x_r", L"Horizontal red-beam displacement in scanline units. Positive moves right; negative moves left."},
    {"convergence_offset_x_g", L"Horizontal green-beam displacement in scanline units. Positive moves right; negative moves left."},
    {"convergence_offset_x_b", L"Horizontal blue-beam displacement in scanline units. Positive moves right; negative moves left."},
    {"convergence_offset_y_r", L"Vertical red-beam displacement in scanline units. Positive moves down; negative moves up."},
    {"convergence_offset_y_g", L"Vertical green-beam displacement in scanline units. Positive moves down; negative moves up."},
    {"convergence_offset_y_b", L"Vertical blue-beam displacement in scanline units. Positive moves down; negative moves up."},
    {"mask_type", L"Phosphor pattern: 0 = aperture grille, 1 = slot mask, 2 = EDP shadow mask."},
    {"mask_sample_mode_desired", L"Mask sampling: 0 = manual sinc resize, 1 = hardware resize, 2 = unscaled 1:1 tiles. Manual mode needs at least a 512 by 512 fitted viewport; hardware mode is the CDU default."},
    {"mask_specify_num_triads", L"Choose phosphor pitch by triad size (0) or by the desired number of triads across the screen (1)."},
    {"mask_triad_size_desired", L"Desired phosphor triad width in output pixels when size mode is selected. The shader rounds and clamps it to the mask constraints."},
    {"mask_num_triads_desired", L"Desired number of phosphor triads across the screen when count mode is selected. The shader rounds and constrains the resulting pitch."},
    {"aa_subpixel_r_offset_x_runtime", L"Horizontal red subpixel antialiasing offset. Fixed by this compiled shader; blue uses the opposite offset."},
    {"aa_subpixel_r_offset_y_runtime", L"Vertical red subpixel antialiasing offset. Fixed by this compiled shader; blue uses the opposite offset."},
    {"aa_cubic_c", L"Sharpness of cubic antialiasing. 0 is a soft spline, about 0.333 is Mitchell, 0.5 is Catmull-Rom; higher values sharpen."},
    {"aa_gauss_sigma", L"Width of Gaussian antialiasing. Only affects builds using the Gaussian AA filter; this build uses the fixed cubic filter."},
    {"geom_mode_runtime", L"Screen mapping: 0 = flat, 1 = spherical, 2 = more bulbous spherical, 3 = cylindrical/Trinitron."},
    {"geom_radius", L"Screen curvature radius measured in viewport diagonals. Larger values make the simulated screen flatter."},
    {"geom_view_dist", L"Viewing distance in viewport diagonals. Changes the field of view of the simulated curved screen."},
    {"geom_tilt_angle_x", L"Screen tilt around the up vector, in radians; clockwise is positive."},
    {"geom_tilt_angle_y", L"Screen tilt around the right vector, in radians; clockwise is positive."},
    {"geom_aspect_ratio_x", L"Horizontal part of the simulated aspect ratio, also used to convert triad size and count. The X/Y ratio must not exceed 4:3; the CDU default is 372:315."},
    {"geom_aspect_ratio_y", L"Vertical part of the simulated aspect ratio. Together with Aspect Ratio X it affects geometry and phosphor pitch calculations."},
    {"geom_overscan_x", L"Horizontal overscan factor. 1 is neutral; increasing it crops more of the horizontal image."},
    {"geom_overscan_y", L"Vertical overscan factor. 1 is neutral; increasing it crops more of the vertical image."},
    {"border_size", L"Width of the simulated black screen border, as a fraction of the viewport. Very small values keep the full CDU visible."},
    {"border_darkness", L"Darkening strength at the simulated screen border."},
    {"border_compress", L"Controls how strongly border shading is compressed toward the edge."},
    {"interlace_detect_toggle", L"Interlace detection is compiled off for progressive CDU input; this value is fixed."},
    {"interlace_bff", L"Bottom-field-first interlace order. Fixed and inactive for progressive CDU input."},
    {"interlace_1080i", L"Treat 1080-line input as interlaced. Fixed and inactive for progressive CDU input."},
};
inline const wchar_t *parameter_help(std::string_view id)
{
    for (const auto &entry : shader_help)
        if (entry.id == id)
            return entry.text;
    return nullptr;
}
}
