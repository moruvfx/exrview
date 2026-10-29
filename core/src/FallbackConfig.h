// Config OCIO tối giản nhúng sẵn — chỉ dùng khi thư viện OCIO < 2.2 (không có config ACES
// dựng sẵn) và người dùng chưa chọn config nào. Dựa trên BuiltinTransform của OCIO 2.1.
#pragma once

namespace exrv {

static const char* kFallbackConfig = R"OCIO(ocio_profile_version: 2.1

name: exrview-fallback
description: Config tối giản của ExrView (ACES 1.0 output, sRGB / Rec.709 / P3)

roles:
  aces_interchange: ACES2065-1
  cie_xyz_d65_interchange: CIE-XYZ-D65
  color_picking: sRGB Encoded Rec.709 (sRGB)
  color_timing: ACEScg
  compositing_log: ACEScg
  data: Raw
  default: ACEScg
  matte_paint: sRGB Encoded Rec.709 (sRGB)
  scene_linear: ACEScg
  texture_paint: sRGB Encoded Rec.709 (sRGB)

file_rules:
  - !<Rule> {name: Default, colorspace: ACEScg}

displays:
  sRGB - Display:
    - !<View> {name: ACES 1.0 - SDR Video, view_transform: ACES 1.0 - SDR Video, display_colorspace: sRGB - Display}
    - !<View> {name: Un-tone-mapped, view_transform: Un-tone-mapped, display_colorspace: sRGB - Display}
    - !<View> {name: Raw, colorspace: Raw}
  Rec.1886 Rec.709 - Display:
    - !<View> {name: ACES 1.0 - SDR Video, view_transform: ACES 1.0 - SDR Video, display_colorspace: Rec.1886 Rec.709 - Display}
    - !<View> {name: Un-tone-mapped, view_transform: Un-tone-mapped, display_colorspace: Rec.1886 Rec.709 - Display}
    - !<View> {name: Raw, colorspace: Raw}
  P3-D65 - Display:
    - !<View> {name: ACES 1.0 - SDR Video, view_transform: ACES 1.0 - SDR Video P3, display_colorspace: P3-D65 - Display}
    - !<View> {name: Un-tone-mapped, view_transform: Un-tone-mapped, display_colorspace: P3-D65 - Display}
    - !<View> {name: Raw, colorspace: Raw}

active_displays: []
active_views: []

view_transforms:
  - !<ViewTransform>
    name: ACES 1.0 - SDR Video
    from_scene_reference: !<BuiltinTransform> {style: ACES-OUTPUT - ACES2065-1_to_CIE-XYZ-D65 - SDR-VIDEO_1.0}
  - !<ViewTransform>
    name: ACES 1.0 - SDR Video P3
    from_scene_reference: !<BuiltinTransform> {style: ACES-OUTPUT - ACES2065-1_to_CIE-XYZ-D65 - SDR-VIDEO-P3lim_1.1}
  - !<ViewTransform>
    name: Un-tone-mapped
    from_scene_reference: !<BuiltinTransform> {style: UTILITY - ACES-AP0_to_CIE-XYZ-D65_BFD}

display_colorspaces:
  - !<ColorSpace>
    name: CIE-XYZ-D65
    family: ""
    isdata: false
  - !<ColorSpace>
    name: sRGB - Display
    isdata: false
    from_display_reference: !<BuiltinTransform> {style: DISPLAY - CIE-XYZ-D65_to_sRGB}
  - !<ColorSpace>
    name: Rec.1886 Rec.709 - Display
    isdata: false
    from_display_reference: !<BuiltinTransform> {style: DISPLAY - CIE-XYZ-D65_to_REC.1886-REC.709}
  - !<ColorSpace>
    name: P3-D65 - Display
    isdata: false
    from_display_reference: !<BuiltinTransform> {style: DISPLAY - CIE-XYZ-D65_to_G2.6-P3-D65}

colorspaces:
  - !<ColorSpace>
    name: ACES2065-1
    aliases: [aces2065_1, ACES - ACES2065-1, lin_ap0]
    family: ACES
    isdata: false

  - !<ColorSpace>
    name: ACEScg
    aliases: [ACES - ACEScg, lin_ap1]
    family: ACES
    isdata: false
    to_scene_reference: !<BuiltinTransform> {style: ACEScg_to_ACES2065-1}

  - !<ColorSpace>
    name: ACEScct
    aliases: [ACES - ACEScct, acescct_ap1]
    family: ACES
    isdata: false
    to_scene_reference: !<BuiltinTransform> {style: ACEScct_to_ACES2065-1}

  - !<ColorSpace>
    name: Linear Rec.709 (sRGB)
    aliases: [lin_rec709_srgb, lin_rec709, lin_srgb, Utility - Linear - sRGB, linear]
    family: Utility
    isdata: false
    to_scene_reference: !<GroupTransform>
      children:
        - !<MatrixTransform> {matrix: [0.4123908, 0.3575843, 0.1804808, 0, 0.2126390, 0.7151687, 0.0721923, 0, 0.0193308, 0.1191948, 0.9505322, 0, 0, 0, 0, 1]}
        - !<BuiltinTransform> {style: UTILITY - ACES-AP0_to_CIE-XYZ-D65_BFD, direction: inverse}

  - !<ColorSpace>
    name: Linear P3-D65
    aliases: [lin_p3d65, Utility - Linear - P3-D65]
    family: Utility
    isdata: false
    to_scene_reference: !<GroupTransform>
      children:
        - !<MatrixTransform> {matrix: [0.4865709, 0.2656677, 0.1982173, 0, 0.2289746, 0.6917385, 0.0792869, 0, 0.0, 0.0451134, 1.0439444, 0, 0, 0, 0, 1]}
        - !<BuiltinTransform> {style: UTILITY - ACES-AP0_to_CIE-XYZ-D65_BFD, direction: inverse}

  - !<ColorSpace>
    name: Linear Rec.2020
    aliases: [lin_rec2020, Utility - Linear - Rec.2020]
    family: Utility
    isdata: false
    to_scene_reference: !<GroupTransform>
      children:
        - !<MatrixTransform> {matrix: [0.6369580, 0.1446169, 0.1688810, 0, 0.2627002, 0.6779981, 0.0593017, 0, 0.0, 0.0280727, 1.0609851, 0, 0, 0, 0, 1]}
        - !<BuiltinTransform> {style: UTILITY - ACES-AP0_to_CIE-XYZ-D65_BFD, direction: inverse}

  - !<ColorSpace>
    name: sRGB Encoded Rec.709 (sRGB)
    aliases: [srgb_encoded_rec709_srgb, srgb_tx, srgb_texture, Utility - sRGB - Texture, sRGB - Texture]
    family: Utility
    isdata: false
    to_scene_reference: !<GroupTransform>
      children:
        - !<ExponentWithLinearTransform> {gamma: 2.4, offset: 0.055, direction: inverse}
        - !<MatrixTransform> {matrix: [0.4123908, 0.3575843, 0.1804808, 0, 0.2126390, 0.7151687, 0.0721923, 0, 0.0193308, 0.1191948, 0.9505322, 0, 0, 0, 0, 1]}
        - !<BuiltinTransform> {style: UTILITY - ACES-AP0_to_CIE-XYZ-D65_BFD, direction: inverse}

  - !<ColorSpace>
    name: Raw
    aliases: [Utility - Raw, raw]
    family: Utility
    isdata: true
)OCIO";

} // namespace exrv
