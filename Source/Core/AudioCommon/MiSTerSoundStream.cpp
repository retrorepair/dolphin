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

// Most chunks to pull in one wake-up before giving up and resynchronising. A wake-up is
// routinely late (see SoundLoop), so catching up is the normal case, not an error - but a
// machine that has been suspended, or a debugger break, can leave an arbitrarily large
// debt, and replaying minutes of stale audio to clear it would be worse than dropping it.
constexpr u32 MAX_CHUNKS_PER_WAKE = 8;
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

  // Pace off the clock rather than off a device: a device-less backend has nothing else to
  // pull against, and the mixer's own granule queue absorbs the difference between this and
  // the emulated rate.
  const u32 rate =
      GetMixer()->GetSampleRate() != 0 ? GetMixer()->GetSampleRate() : FALLBACK_SAMPLE_RATE;
  const auto chunk_period = std::chrono::nanoseconds(1000000000ULL * FRAMES_PER_CHUNK / rate);
  auto next_pull = std::chrono::steady_clock::now();

  while (m_run_thread.IsSet())
  {
    // Pull every chunk that has fallen due, not just one.
    //
    // Windows' default timer granularity is ~15.6ms against a 5.33ms chunk, so a wake-up is
    // routinely two or three chunks late. Pulling a single chunk per wake-up would drain the
    // mixer at a third of the rate the emulated DSP fills it, which Dolphin reports as
    // "Granule Queue has completely filled and audio samples are being dropped" - and the
    // MiSTer then gets a third of the audio it needs. Accumulating next_pull by the exact
    // chunk period keeps the long-run rate right whatever the timer does.
    const auto now = std::chrono::steady_clock::now();
    u32 pulled = 0;
    while ((next_pull <= now) && (pulled < MAX_CHUNKS_PER_WAKE))
    {
      // Mixer::Mix feeds the tap in Mixer.cpp on its way out, so there is nothing to forward
      // here: this thread exists only to provide the pull no audio device is providing.
      GetMixer()->Mix(chunk.data(), FRAMES_PER_CHUNK);
      next_pull += chunk_period;
      ++pulled;
    }

    // Still behind after the cap: the debt is too large to be timer jitter, so drop it
    // rather than replay stale audio, and let the mixer's own latency control settle.
    if (next_pull < now)
      next_pull = now;

    std::this_thread::sleep_until(next_pull);
  }
}
