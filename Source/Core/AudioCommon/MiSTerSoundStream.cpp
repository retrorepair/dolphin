// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AudioCommon/MiSTerSoundStream.h"

#include <array>
#include <chrono>
#include <thread>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"

#include "VideoCommon/GroovyMiSTer/GroovyMiSTer.h"

namespace
{
// The rate CMD_INIT negotiates when the MiSTer session has audio, and the rate the
// GameCube's own DSP runs at. Used when the Groovy output is not up yet, so the mixer still
// has a sane rate to resample to and switching it on mid-session does not need a restart.
constexpr u32 FALLBACK_SAMPLE_RATE = 48000;

// 5.33ms at 48kHz. Small enough that the ring never carries meaningful latency, large
// enough that the pull does not cost more in wakeups than it moves in samples.
constexpr u32 FRAMES_PER_CHUNK = 256;
}  // namespace

MiSTerSound::MiSTerSound() = default;

MiSTerSound::~MiSTerSound()
{
  SetRunning(false);
}

bool MiSTerSound::Init()
{
  // The Groovy session's rate is fixed at CMD_INIT, so the mixer has to resample to it
  // rather than the other way round.
  const u32 rate = GroovyMiSTer::GetAudioSampleRate();
  GetMixer()->SetSampleRate(rate != 0 ? rate : FALLBACK_SAMPLE_RATE);
  return true;
}

bool MiSTerSound::SetRunning(bool running)
{
  if (running == m_running.load(std::memory_order_relaxed))
    return true;

  if (running)
  {
    m_run_thread.Set();
    m_thread = std::thread(&MiSTerSound::SoundLoop, this);
    m_running.store(true, std::memory_order_relaxed);
  }
  else
  {
    m_run_thread.Clear();
    if (m_thread.joinable())
      m_thread.join();
    m_running.store(false, std::memory_order_relaxed);
  }

  return true;
}

void MiSTerSound::SetVolume(int volume)
{
  // Volume belongs to the MiSTer's own mixer, which has its own OSD control, so there is
  // nothing to scale here - and scaling in software before an analog stage only costs
  // resolution.
}

void MiSTerSound::SoundLoop()
{
  Common::SetCurrentThreadName("MiSTer Audio");

  std::array<s16, FRAMES_PER_CHUNK * 2> chunk{};

  // Pace off the clock rather than off a device, accumulating the chunk period so rounding
  // cannot drift: a device-less backend has nothing else to pull against, and the mixer's
  // own granule queue absorbs the difference between this and the emulated rate.
  const u32 rate = GetMixer()->GetSampleRate() != 0 ? GetMixer()->GetSampleRate() :
                                                      FALLBACK_SAMPLE_RATE;
  const auto chunk_period = std::chrono::nanoseconds(1000000000ULL * FRAMES_PER_CHUNK / rate);
  auto next_wake = std::chrono::steady_clock::now();

  while (m_run_thread.IsSet())
  {
    next_wake += chunk_period;

    GetMixer()->Mix(chunk.data(), FRAMES_PER_CHUNK);

    // Mixer::Mix already fed the tap in Mixer.cpp, so there is nothing to forward here:
    // this thread exists to provide the pull that no audio device is providing.

    const auto now = std::chrono::steady_clock::now();
    if (next_wake > now)
    {
      std::this_thread::sleep_until(next_wake);
    }
    else if (now - next_wake > chunk_period * 8)
    {
      // Fell far enough behind that catching up would mean a burst of stale audio. Resync
      // to now and let the mixer's own latency control settle it.
      next_wake = now;
    }
  }
}
