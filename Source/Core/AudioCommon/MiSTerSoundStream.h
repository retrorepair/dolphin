// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <thread>

#include "AudioCommon/SoundStream.h"
#include "Common/Flag.h"

/// Sends the mixed audio to the MiSTer instead of to a host device.
///
/// Dolphin's other backends are pulled by an audio device; there is no device here, so this
/// one drains the mixer on a thread of its own at the rate the Groovy session was negotiated
/// at and hands each chunk to GroovyMiSTer::OnAudioChunk. The MiSTer core puts it out of the
/// FPGA's own DAC, in step with the video it belongs to.
///
/// Picking this backend is how you get audio out of the MiSTer and nothing out of the PC,
/// which is the point when the PC is in another room. Any other backend still mirrors its
/// mixed output to the MiSTer (see the tap in Mixer::Mix) - you just get both.
class MiSTerSound final : public SoundStream
{
public:
  MiSTerSound();
  ~MiSTerSound() override;

  bool Init() override;
  bool SetRunning(bool running) override;
  void SetVolume(int volume) override;

  static bool IsValid() { return true; }

private:
  void SoundLoop();

  std::thread m_thread;
  Common::Flag m_run_thread;
  std::atomic_bool m_running{false};
};
