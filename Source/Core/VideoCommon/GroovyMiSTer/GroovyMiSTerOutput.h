// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"

#include "Core/Config/GroovyMiSTerSettings.h"

#include "VideoCommon/GroovyMiSTer/GroovyMiSTerAudioTap.h"
#include "VideoCommon/GroovyMiSTer/GroovyMiSTerKeepAlive.h"
#include "VideoCommon/GroovyMiSTer/GroovyMiSTerModeline.h"
#include "VideoCommon/GroovyMiSTer/GroovyMiSTerPixels.h"

class AbstractFramebuffer;
class AbstractStagingTexture;
class AbstractTexture;

namespace GroovyMiSTer
{
/// Everything the live stream can be asked about, for the OSD and the log.
struct Status
{
  bool connected = false;
  u32 native_width = 0;  // the game's real framebuffer, not the internal-resolution scale
  u32 native_height = 0;
  double refresh_hz = 0.0;
  Modeline modeline{};
  std::string monitor_preset;
  // Latency from the FPGA: how far the raster had travelled when it received our frame,
  // against where it is scanning out now.
  double latency_ms = 0.0;
  u64 frames_sent = 0;
  u64 frames_dropped = 0;
  u32 last_encoded_bytes = 0;
};

/// Owns the streaming pipeline. One instance, created by Open().
///
/// Threading:
///
///   Video thread  Capture(): scale, read back and pack pixels, push to m_queue. Never
///                 touches the socket and never encodes - NLC costs several milliseconds a
///                 frame and would throttle emulation through the GPU thread.
///
///   Sender thread Sole owner of the Groovy video/audio socket (UDP 32100). Applies pending
///                 switchres, drains the audio tap, encodes, blits, paces. Close belongs
///                 here too: the Windows RIO send path defers sends, so a CMD_CLOSE issued
///                 from another thread is dropped and the MiSTer holds our last frame. It
///                 also owns the idle keepalive, because every way Dolphin goes quiet -
///                 pause, savestate load, disc change, a refused modeline - stops the video
///                 thread, not this one, and nothing can stall the sender (under
///                 MisterMaster the video thread blocks on it, never the reverse).
///
///   CPU thread    PublishFieldInfo() only: a pair of relaxed atomic stores.
///
///   Audio thread  AudioTap::Write() only.
class Output
{
public:
  Output();
  ~Output();

  bool Open();
  void Close();

  bool IsActive() const { return m_active.load(std::memory_order_relaxed); }
  bool IsAudioActive() const { return m_audio_tap.IsActive(); }
  u32 GetAudioSampleRate() const { return m_sound_rate_hz; }

  /// CPU thread. The refresh rate and the field index are only safe to read there, so they
  /// are published before the XFB reaches the video thread rather than looked up from it.
  void PublishFieldInfo(u8 field, u32 refresh_numerator, u32 refresh_denominator);

  void Capture(const AbstractTexture* texture, const MathUtil::Rectangle<int>& rect,
               u32 native_width, u32 native_height);
  void WriteAudio(const s16* samples, u32 frames) { m_audio_tap.Write(samples, frames); }

  Status GetStatus() const;

private:
  // --- connection -----------------------------------------------------------------
  bool TryConnect();
  void DoGroovyClose();

  // --- switchres ------------------------------------------------------------------
  bool InitSwitchres();
  void ShutdownSwitchres();
  /// Recompute the modeline if the game's video mode changed. False when there is no usable
  /// mode, in which case nothing is streamed.
  bool EnsureMode(u32 src_w, u32 src_h, double refresh_hz);

  // --- capture --------------------------------------------------------------------
  bool EnsureGpuResources();
  void ReleaseGpuResources();
  void PushFrame(std::vector<u8>&& pixels, u8 field);

  // --- sender ---------------------------------------------------------------------
  void SenderLoop();

  struct OutFrame
  {
    std::vector<u8> pixels;
    u8 field = 0;
  };

  std::atomic_bool m_active{false};
  std::atomic_bool m_quit{false};

  // --- configuration, latched at Open() so a mid-session edit cannot tear ----------
  bool m_cfg_enabled = false;
  std::string m_cfg_host;
  std::string m_cfg_monitor_preset;
  std::string m_cfg_switchres_ini;
  Codec m_cfg_codec = Codec::NLC;
  RgbMode m_cfg_rgb_mode = RgbMode::RGB888;
  NlcPack m_cfg_nlc_pack = NlcPack::Rice;
  int m_cfg_nlc_near_level = 1;
  Interlace m_cfg_interlace = Interlace::ProgressiveFB;
  Pacing m_cfg_pacing = Pacing::DolphinMaster;
  int m_cfg_mtu = 1500;
  bool m_cfg_audio = true;
  bool m_cfg_crt_safety_cap = true;
  int m_cfg_log_verbosity = 0;

  // Negotiated at CMD_INIT and fixed for the session.
  u8 m_codec = 0;
  u8 m_rgb_mode = 0;
  u32 m_bpp = 3;
  u32 m_sound_rate = 0;     // SoundRateCode, what goes on the wire
  u32 m_sound_rate_hz = 0;  // the same rate in Hz, for the audio device
  u8 m_sound_chan = 0;

  // --- published by the CPU thread -------------------------------------------------
  std::atomic<u32> m_vi_refresh_num{0};
  std::atomic<u32> m_vi_refresh_den{1};
  std::atomic<u8> m_vi_field{0};

  // --- switchres / modeline --------------------------------------------------------
  bool m_sr_inited = false;
  Modeline m_modeline{};
  bool m_have_mode = false;
  // Last source geometry we ran through switchres, so we only recompute on change.
  u32 m_src_w = 0;
  u32 m_src_h = 0;
  double m_src_hz = 0.0;

  mutable std::mutex m_sr_lock;
  Modeline m_pending_modeline{};
  bool m_switchres_pending = false;

  // --- GPU capture ------------------------------------------------------------------
  struct Readback
  {
    std::unique_ptr<AbstractStagingTexture> tex;
    bool pending = false;
    u8 field = 0;
  };

  std::unique_ptr<AbstractTexture> m_scratch_texture;
  std::unique_ptr<AbstractFramebuffer> m_scratch_framebuffer;
  Readback m_readback[2];
  u32 m_readback_idx = 0;
  u32 m_dst_w = 0;
  u32 m_dst_h = 0;
  bool m_logged_first_frame = false;
  // One-shot: the sender's oversized-frame guard must not log per frame.
  bool m_logged_oversized_frame = false;

  // --- video -> sender queue ---------------------------------------------------------
  // Deliberately tiny. DolphinMaster drops the older frame rather than build a backlog;
  // MisterMaster blocks the push, which is how the raster throttles emulation.
  static constexpr size_t MAX_QUEUED_FRAMES = 1;
  std::mutex m_queue_lock;
  std::condition_variable m_queue_cv;
  std::condition_variable m_space_cv;
  std::deque<OutFrame> m_queue;

  std::thread m_sender;

  // --- status -------------------------------------------------------------------------
  mutable std::mutex m_status_lock;
  Status m_status;

  // Sender-thread-only, no locking.
  u32 m_blit_frame = 0;
  // Tracks the client's internal auto-reconnect, which is observable but not controllable.
  u32 m_reconnect_epoch = 0;
  KeepAliveScheduler m_keepalive;

  AudioTap m_audio_tap;
};
}  // namespace GroovyMiSTer
