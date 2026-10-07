// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"

class AbstractTexture;

// =====================================================================================
//  GroovyMiSTer - low-latency streaming output to a MiSTer FPGA
// =====================================================================================
//
// Streams the finished XFB and the mixed audio to a MiSTer running the GroovyNLC core over
// UDP. The MiSTer raster-chases its CRT, so a frame arrives microseconds before it is
// scanned out; switchres turns the game's video mode into a real CRT modeline rather than
// scaling it to a fixed one.
//
// This header is the whole seam into upstream Dolphin - everything else lives under
// VideoCommon/GroovyMiSTer/ - so add a function here rather than reaching into the module
// from upstream code. Call sites:
//
//   VideoCommon/VideoBackendBase.cpp -> Open() / Close()
//   VideoCommon/Present.cpp          -> IsActive() / OnXFB()
//   Core/HW/VideoInterface.cpp       -> OnFieldOutput()
//   AudioCommon/Mixer.cpp            -> IsAudioActive() / OnAudioChunk()
//   AudioCommon/MiSTerSoundStream    -> drives Mixer::Mix when the MiSTer is the audio device
//
namespace GroovyMiSTer
{
/// Bring the output up if Dolphin.ini [GroovyMiSTer] Enabled is set. Safe to call when
/// disabled and safe to call twice. Called on the video thread from
/// VideoBackendBase::InitializeShared, once g_gfx is live; a MiSTer that is off or asleep
/// must not stop a game from booting, so connection failures are non-fatal and retried in
/// the background.
void Open();

/// Stop the sender, tell the MiSTer we are going away so it returns to its
/// connection-search screen, and release GPU resources. Called from
/// VideoBackendBase::ShutdownShared, before g_gfx is torn down.
void Close();

/// True when streaming. The guard on every hot-path hook, so it stays a single relaxed
/// atomic load.
bool IsActive();

/// One VI field has been scanned out. Called on the CPU thread from
/// VideoInterfaceManager::OutputField, just before the XFB is handed to the video thread.
///
/// This exists because the two things switchres needs that the XFB itself does not carry -
/// the target refresh rate and which field this is - are only safe to read on the CPU
/// thread. Publishing them here keeps OnXFB() free of any VI access.
void OnFieldOutput(u8 field, u32 refresh_rate_numerator, u32 refresh_rate_denominator);

/// Hand over the finished frame. Called on the video thread from Presenter::ViSwap and
/// Presenter::ImmediateSwap, after presentation.
///
/// `texture`       the XFB texture.
/// `rect`          the valid region of `texture`, at the internal resolution scale.
/// `native_width`  the game's real framebuffer size, which is what switchres is asked for.
/// `native_height` Dolphin's internal resolution multiplier must not reach the CRT: a
///                 15kHz display wants 640x480, not 2560x1920 downscaled by the FPGA.
void OnXFB(const AbstractTexture* texture, const MathUtil::Rectangle<int>& rect,
           u32 native_width, u32 native_height);

/// True when audio should be mirrored to the MiSTer. Guard for OnAudioChunk().
bool IsAudioActive();

/// Hand over one chunk of mixed audio: `frames` interleaved stereo s16 samples, the same
/// buffer the host audio backend is about to play. Called on whichever thread is draining
/// the mixer.
void OnAudioChunk(const s16* samples, u32 frames);

/// The sample rate the MiSTer session was negotiated at. The rate is baked into CMD_INIT,
/// so the audio device has to match it rather than the other way round.
u32 GetAudioSampleRate();
}  // namespace GroovyMiSTer
