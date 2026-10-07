// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/Config/ConfigInfo.h"

#include <string>

namespace GroovyMiSTer
{
// CMD_INIT's lz4Frames byte. Only Raw and NLC are useful here: LZ4 costs more bandwidth
// than it saves on 3D content (it is tuned for flat 2D), and NLC is what brings a Dolphin
// frame under the core's ingest ceiling.
enum class Codec : int
{
  Raw = 0,
  LZ4 = 1,
  LZ4HC = 3,
  NLC = 7,
};

// CMD_INIT's rgbMode byte.
enum class RgbMode : int
{
  RGB888 = 0,
  RGBA8888 = 1,
  RGB565 = 2,
};

// NLC's entropy front-end (CMD_INIT byte[1] bit 7). Rice needs a core with the Rice
// decoder; an older core misparses Rice bytes as Tiled and shows garbage.
enum class NlcPack : int
{
  Tiled = 1,
  Rice = 2,
};

// What goes in CMD_SWITCHRES's interlace byte when switchres returns an interlaced
// modeline.
//
// Dolphin hands VideoCommon one framebuffer per VI field, and with Force Progressive on
// (its default) that framebuffer holds both fields' lines at full height - so the core
// should do the field split itself, which is ProgressiveFB. Field is for the case where
// Force Progressive is off and each framebuffer really is one field.
enum class Interlace : int
{
  Progressive = 0,  // never ask switchres for an interlaced mode
  Field = 1,        // one true field per blit
  ProgressiveFB = 2,  // full-height framebuffer, core splits it into fields
};

// Who sets the pace.
enum class Pacing : int
{
  // Dolphin's own frame limiter runs the show; a frame that arrives too late is dropped.
  DolphinMaster = 0,
  // The CRT's raster is the clock: the capture blocks until the sender has room, which
  // pushes back through the GPU thread onto emulation.
  MisterMaster = 1,
};

// Largest blit the vendored client's fixed-size RIO-registered buffer can hold. The client
// derives the stream length from the modeline with no clamp in between, so an oversized
// mode walks off the end of that allocation - a property of the client, not of the
// display, so it is enforced whatever the safety cap says.
constexpr unsigned int MAX_BLIT_BYTES = 2048 * 1024;

// The CRT safety envelope. An arcade or consumer CRT driven far outside its designed
// range can be damaged - the horizontal output stage and flyback are what let go.
constexpr unsigned int MAX_SAFE_H_ACTIVE = 2048;
constexpr unsigned int MAX_SAFE_V_ACTIVE = 624;
}  // namespace GroovyMiSTer

namespace Config
{
// Dolphin.ini [GroovyMiSTer]
extern const Info<bool> GROOVY_MISTER_ENABLED;
extern const Info<std::string> GROOVY_MISTER_HOST;
extern const Info<std::string> GROOVY_MISTER_MONITOR_PRESET;
extern const Info<std::string> GROOVY_MISTER_SWITCHRES_INI;
extern const Info<GroovyMiSTer::Codec> GROOVY_MISTER_CODEC;
extern const Info<GroovyMiSTer::RgbMode> GROOVY_MISTER_RGB_MODE;
extern const Info<GroovyMiSTer::NlcPack> GROOVY_MISTER_NLC_PACK;
extern const Info<int> GROOVY_MISTER_NLC_NEAR_LEVEL;
extern const Info<GroovyMiSTer::Interlace> GROOVY_MISTER_INTERLACE;
extern const Info<GroovyMiSTer::Pacing> GROOVY_MISTER_PACING;
extern const Info<int> GROOVY_MISTER_MTU;
extern const Info<bool> GROOVY_MISTER_AUDIO;
extern const Info<bool> GROOVY_MISTER_CRT_SAFETY_CAP;
extern const Info<int> GROOVY_MISTER_LOG_VERBOSITY;
}  // namespace Config
