// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Config/GroovyMiSTerSettings.h"

namespace GroovyMiSTer
{
/// Bytes per pixel on the wire.
constexpr u32 BytesPerPixel(RgbMode mode)
{
  switch (mode)
  {
  case RgbMode::RGB565:
    return 2;
  case RgbMode::RGBA8888:
    return 4;
  default:
    return 3;
  }
}

/// Convert a Dolphin readback (RGBA8: byte 0 = R, 1 = G, 2 = B, 3 = A) into the MiSTer's
/// wire layout, which is not what the format names suggest:
///
///     RGB888   -> B, G, R
///     RGBA8888 -> B, G, R, A   (4th byte ignored by the core)
///     RGB565   -> little-endian u16, (r << 11) | (g << 5) | b
///
/// From the FPGA RTL (Groovy.sv, decode_pixel), which unpacks a pixel as
/// `{r,g,b} <= word64[0 +: 24]`: a Verilog concatenation, so `b` occupies the
/// least-significant byte, and DDR being little-endian makes stream byte 0 blue. The source
/// is RGBA, so every mode needs a channel swap and none of them is a memcpy.
///
/// `src_pitch` is in bytes and may exceed width * 4 - staging textures are usually padded.
void PackFrame(RgbMode mode, const u8* src, u32 src_pitch, u32 width, u32 height,
               std::vector<u8>& dst);
}  // namespace GroovyMiSTer
