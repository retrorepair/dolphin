// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Core/Config/GroovyMiSTerSettings.h"

namespace GroovyMiSTer
{
// A CRT modeline, in the shape gmw_switchres() wants.
struct Modeline
{
  double pclock = 0.0;  // pixel clock, MHz
  u16 h_active = 0, h_begin = 0, h_end = 0, h_total = 0;
  u16 v_active = 0, v_begin = 0, v_end = 0, v_total = 0;
  u8 interlace = 0;  // 0 progressive, 1 interlaced field, 2 progressive FB over interlaced

  bool operator==(const Modeline&) const = default;
};

/// Structural sanity: zero-sized, or blanking that does not enclose the active area. Such
/// a modeline drives the FPGA's PLL into an undefined state, so it is rejected whatever the
/// safety-cap setting says.
constexpr bool IsWellFormed(const Modeline& m)
{
  return m.pclock > 0.0 && m.h_active > 0 && m.v_active > 0 && m.h_begin >= m.h_active &&
         m.h_end >= m.h_begin && m.h_total > m.h_end && m.v_begin >= m.v_active &&
         m.v_end >= m.v_begin && m.v_total > m.v_end;
}

/// Bytes the client will stream for one blit of this modeline.
///
/// A true-field stream (interlace == 1) sends one half-height field per blit; the other two
/// modes send every line every time. `bytes_per_pixel` is the wire format's, from
/// BytesPerPixel() - a parameter so this header stays dependency-free.
constexpr u32 BlitBytes(const Modeline& m, u32 bytes_per_pixel)
{
  const u32 lines = (m.interlace == static_cast<u8>(Interlace::Field)) ?
                        (static_cast<u32>(m.v_active) / 2u) :
                        static_cast<u32>(m.v_active);
  return static_cast<u32>(m.h_active) * lines * bytes_per_pixel;
}

/// Does one blit fit the client's fixed-size buffer?
///
/// The vendored client allocates its blit buffers once at BUFFER_SIZE and derives the
/// stream length from the modeline it is given, with no clamp in between, so an oversized
/// mode walks off the end of a RIO-registered allocation. A property of the client rather
/// than of the display, so it is always enforced.
constexpr bool FitsBlitBuffer(const Modeline& m, u32 bytes_per_pixel)
{
  return BlitBytes(m, bytes_per_pixel) <= MAX_BLIT_BYTES;
}

/// The CRT safety cap.
///
/// An arcade or consumer CRT driven far outside its designed envelope can be damaged; the
/// horizontal output stage and flyback are what let go. Conservative and on by default
/// (Dolphin.ini [GroovyMiSTer] CrtSafetyCap). A multisync or genuine 31kHz display can turn
/// it off.
constexpr bool IsWithinCrtSafeEnvelope(const Modeline& m)
{
  return m.v_active <= MAX_SAFE_V_ACTIVE && m.h_active <= MAX_SAFE_H_ACTIVE;
}

/// Full gate applied before any modeline is sent to the FPGA.
///
/// Well-formedness and the blit-buffer budget are structural - an undefined PLL state and a
/// heap overrun respectively - and never optional. Only the CRT envelope cap follows the
/// CrtSafetyCap setting.
constexpr bool IsModelineAcceptable(const Modeline& m, u32 bytes_per_pixel, bool enforce_cap)
{
  return IsWellFormed(m) && FitsBlitBuffer(m, bytes_per_pixel) &&
         (!enforce_cap || IsWithinCrtSafeEnvelope(m));
}
}  // namespace GroovyMiSTer
