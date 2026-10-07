// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Config/GroovyMiSTerSettings.h"

namespace Config
{
// Dolphin.ini [GroovyMiSTer]
const Info<bool> GROOVY_MISTER_ENABLED{{System::Main, "GroovyMiSTer", "Enabled"}, false};
const Info<std::string> GROOVY_MISTER_HOST{{System::Main, "GroovyMiSTer", "Host"}, ""};

// Tri-sync arcade monitor (15/25/31 kHz). switchres's own default is generic_15, which
// refuses every 31 kHz mode and so would reject the 480p GameCube and Wii titles outright.
const Info<std::string> GROOVY_MISTER_MONITOR_PRESET{
    {System::Main, "GroovyMiSTer", "MonitorPreset"}, "arcade_15_25_31"};
const Info<std::string> GROOVY_MISTER_SWITCHRES_INI{
    {System::Main, "GroovyMiSTer", "SwitchresIni"}, ""};

const Info<GroovyMiSTer::Codec> GROOVY_MISTER_CODEC{{System::Main, "GroovyMiSTer", "Codec"},
                                                    GroovyMiSTer::Codec::NLC};
const Info<GroovyMiSTer::RgbMode> GROOVY_MISTER_RGB_MODE{
    {System::Main, "GroovyMiSTer", "RgbMode"}, GroovyMiSTer::RgbMode::RGB888};

// Rice beats Tiled on the photographic, dithered output a 3D console produces, and needs a
// core with the Rice decoder (the rbf_rice_r3 kit or newer).
const Info<GroovyMiSTer::NlcPack> GROOVY_MISTER_NLC_PACK{
    {System::Main, "GroovyMiSTer", "NlcPack"}, GroovyMiSTer::NlcPack::Rice};

// +-1 near-lossless. Analog-invisible, and it is what keeps a 640x480 GameCube frame
// inside the core's ingest ceiling at 60Hz.
const Info<int> GROOVY_MISTER_NLC_NEAR_LEVEL{{System::Main, "GroovyMiSTer", "NlcNearLevel"}, 1};

const Info<GroovyMiSTer::Interlace> GROOVY_MISTER_INTERLACE{
    {System::Main, "GroovyMiSTer", "Interlace"}, GroovyMiSTer::Interlace::ProgressiveFB};
const Info<GroovyMiSTer::Pacing> GROOVY_MISTER_PACING{{System::Main, "GroovyMiSTer", "Pacing"},
                                                      GroovyMiSTer::Pacing::DolphinMaster};

// 1500 is the safe default. 3800 needs jumbo frames enabled on the core and on every hop.
const Info<int> GROOVY_MISTER_MTU{{System::Main, "GroovyMiSTer", "Mtu"}, 1500};

const Info<bool> GROOVY_MISTER_AUDIO{{System::Main, "GroovyMiSTer", "Audio"}, true};
const Info<bool> GROOVY_MISTER_CRT_SAFETY_CAP{{System::Main, "GroovyMiSTer", "CrtSafetyCap"},
                                              true};

// 0 = errors and handshake only, 1 and 2 add per-frame client trace.
const Info<int> GROOVY_MISTER_LOG_VERBOSITY{{System::Main, "GroovyMiSTer", "LogVerbosity"}, 0};
}  // namespace Config
