// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GroovyMiSTer/GroovyMiSTerOutput.h"
#include "VideoCommon/GroovyMiSTer/GroovyMiSTer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/Timer.h"

#include "Core/Config/GroovyMiSTerSettings.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/TextureConfig.h"

#include "groovymister_wrapper.h"
#include "switchres_wrapper.h"

// =====================================================================================
//  Protocol constants, pinned.
// =====================================================================================
// These go on the wire in CMD_INIT. Pinned so a re-vendor that renumbers any of them fails
// to build rather than silently changing the handshake.
static_assert(static_cast<int>(GroovyMiSTer::Codec::Raw) == LZ4_OFF);
static_assert(static_cast<int>(GroovyMiSTer::Codec::LZ4) == LZ4);
static_assert(static_cast<int>(GroovyMiSTer::Codec::LZ4HC) == LZ4_HC);
static_assert(static_cast<int>(GroovyMiSTer::Codec::NLC) == NLC);
static_assert(static_cast<int>(GroovyMiSTer::RgbMode::RGB888) == RGB_888);
static_assert(static_cast<int>(GroovyMiSTer::RgbMode::RGBA8888) == RGB_A888);
static_assert(static_cast<int>(GroovyMiSTer::RgbMode::RGB565) == RGB_565);
static_assert(static_cast<int>(GroovyMiSTer::NlcPack::Tiled) == 1, "NLC_PACK_TILED");
static_assert(static_cast<int>(GroovyMiSTer::NlcPack::Rice) == 2, "NLC_PACK_RICE");

namespace GroovyMiSTer
{
namespace
{
std::unique_ptr<Output> s_output;

void GmwLogSink(const char* msg)
{
  if (msg && *msg)
    INFO_LOG_FMT(VIDEO, "[MiSTer] {}", StripWhitespace(msg));
}

void SrLogSink(const char* format, ...)
{
  // switchres logs printf-style.
  char buf[512];
  va_list ap;
  va_start(ap, format);
  std::vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  INFO_LOG_FMT(VIDEO, "[MiSTer/switchres] {}", StripWhitespace(buf));
}

u32 SoundRateCodeFor(u32 hz)
{
  switch (hz)
  {
  case 22050:
    return RATE_22050;
  case 44100:
    return RATE_44100;
  case 48000:
    return RATE_48000;
  default:
    return RATE_OFF;
  }
}

// Tri-sync arcade monitor (15/25/31 kHz), compiled into switchres; no ini needed.
constexpr const char* DEFAULT_MONITOR = "arcade_15_25_31";

// Every compiled-in switchres preset (Externals/switchres/monitor.cpp, monitor_set_preset).
// switchres matches preset names with a raw strcmp and falls back to generic_15 on an
// unknown one without reporting it - and generic_15 refuses every 31 kHz mode, which is
// every 480p GameCube and Wii title - so validate here instead. "custom" takes its timings
// from a Switchres INI and is handled separately.
bool IsKnownMonitorPreset(const std::string& p)
{
  static constexpr std::array kKnown = {
      // Arcade multi-sync / single-sync
      "arcade_15_25_31", "arcade_15_31", "arcade_15_25", "arcade_15", "arcade_15ex",
      "arcade_25", "arcade_31",
      // Generic / broadcast
      "generic_15", "ntsc", "pal",
      // Monitor models. "d9400" and "polo" are switchres aliases of "d9800" and "h9110".
      "d9800", "d9400", "d9200", "k7000", "k7131", "m3129", "m2929", "h9110", "polo",
      "pstar", "ms2930", "ms929", "r666b",
      // PC CRT / VESA GTF
      "pc_31_120", "pc_70_120", "vesa_480", "vesa_600", "vesa_768", "vesa_1024",
      // Timings supplied by the user's Switchres INI
      "custom"};

  return std::ranges::find(kKnown, p) != kKnown.end();
}

const char* CodecName(u8 codec)
{
  switch (codec)
  {
  case LZ4_OFF:
    return "raw";
  case NLC:
    return "NLC";
  default:
    return "LZ4";
  }
}
}  // namespace

// =================================================================================
//  Lifecycle
// =================================================================================

Output::Output() = default;

Output::~Output()
{
  Close();
}

bool Output::Open()
{
  if (m_active.load(std::memory_order_relaxed))
    return true;

  // Latch the configuration once. Everything below is read from these copies, so a setting
  // edited mid-session cannot change the pixel format out from under a queued frame.
  m_cfg_enabled = Config::Get(Config::GROOVY_MISTER_ENABLED);
  m_cfg_host = Config::Get(Config::GROOVY_MISTER_HOST);
  m_cfg_monitor_preset = Config::Get(Config::GROOVY_MISTER_MONITOR_PRESET);
  m_cfg_switchres_ini = Config::Get(Config::GROOVY_MISTER_SWITCHRES_INI);
  m_cfg_codec = Config::Get(Config::GROOVY_MISTER_CODEC);
  m_cfg_rgb_mode = Config::Get(Config::GROOVY_MISTER_RGB_MODE);
  m_cfg_nlc_pack = Config::Get(Config::GROOVY_MISTER_NLC_PACK);
  m_cfg_nlc_near_level = std::clamp(Config::Get(Config::GROOVY_MISTER_NLC_NEAR_LEVEL), 0, 3);
  m_cfg_interlace = Config::Get(Config::GROOVY_MISTER_INTERLACE);
  m_cfg_pacing = Config::Get(Config::GROOVY_MISTER_PACING);
  m_cfg_mtu = std::clamp(Config::Get(Config::GROOVY_MISTER_MTU), 576, 9000);
  m_cfg_audio = Config::Get(Config::GROOVY_MISTER_AUDIO);
  m_cfg_crt_safety_cap = Config::Get(Config::GROOVY_MISTER_CRT_SAFETY_CAP);
  m_cfg_log_verbosity = std::clamp(Config::Get(Config::GROOVY_MISTER_LOG_VERBOSITY), 0, 2);

  if (!m_cfg_enabled)
    return false;

  if (m_cfg_host.empty())
  {
    ERROR_LOG_FMT(VIDEO, "[MiSTer] Output is enabled but no host is set; add Host = <ip> under "
                         "[GroovyMiSTer] in Dolphin.ini.");
    return false;
  }

  m_quit.store(false, std::memory_order_relaxed);
  m_blit_frame = 0;
  m_have_mode = false;
  m_src_w = m_src_h = 0;
  m_src_hz = 0.0;
  m_logged_first_frame = false;
  m_logged_oversized_frame = false;

  m_codec = static_cast<u8>(m_cfg_codec);
  m_rgb_mode = static_cast<u8>(m_cfg_rgb_mode);

  // NLC is RGB888-only: the FPGA decoder has three plane cores and no pixel-format input,
  // and CmdInit refuses any other rgbMode outright. Fall back rather than fail to connect,
  // which would surface as an unreachable host and retry forever.
  if (m_cfg_codec == Codec::NLC && m_cfg_rgb_mode != RgbMode::RGB888)
  {
    WARN_LOG_FMT(VIDEO, "[MiSTer] NLC requires RGB888; ignoring the configured RgbMode.");
    m_rgb_mode = static_cast<u8>(RgbMode::RGB888);
  }
  m_bpp = BytesPerPixel(static_cast<RgbMode>(m_rgb_mode));

  // The rate is baked into CMD_INIT, so it has to be fixed up front. Dolphin's mixer
  // resamples everything to its output rate, and 48kHz is the one the GameCube's own DSP
  // runs at, so there is nothing to gain from any other.
  if (m_cfg_audio)
  {
    m_sound_rate_hz = 48000;
    m_sound_rate = SoundRateCodeFor(m_sound_rate_hz);
    m_sound_chan = CHAN_STEREO;
  }
  else
  {
    m_sound_rate_hz = 0;
    m_sound_rate = RATE_OFF;
    m_sound_chan = CHAN_OFF;
  }

  if (!InitSwitchres())
    return false;

  if (!TryConnect())
  {
    // Non-fatal: a MiSTer that is off or asleep must not stop a game from booting. The
    // sender thread keeps retrying.
    WARN_LOG_FMT(VIDEO, "[MiSTer] Could not reach {} yet; will keep retrying.", m_cfg_host);
  }

  m_audio_tap.Reset();
  m_audio_tap.SetActive(m_cfg_audio && m_sound_chan != CHAN_OFF);

  m_active.store(true, std::memory_order_release);
  m_sender = std::thread([this]() { SenderLoop(); });

  INFO_LOG_FMT(VIDEO, "[MiSTer] Output enabled (host={}, codec={}, {} bpp).", m_cfg_host,
               CodecName(m_codec), m_bpp);
  return true;
}

void Output::Close()
{
  if (!m_active.exchange(false, std::memory_order_acq_rel))
  {
    // Never connected, or already closed. switchres may still be up if Open() bailed after
    // initialising it.
    ShutdownSwitchres();
    return;
  }

  m_audio_tap.SetActive(false);

  {
    std::lock_guard<std::mutex> guard(m_queue_lock);
    m_quit.store(true, std::memory_order_release);
    m_queue.clear();
  }
  m_queue_cv.notify_all();
  m_space_cv.notify_all();

  // The sender owns the socket and must be the one to close it (see the RIO note in the
  // header); DoGroovyClose() runs at the end of SenderLoop().
  if (m_sender.joinable())
    m_sender.join();

  ReleaseGpuResources();
  ShutdownSwitchres();

  INFO_LOG_FMT(VIDEO, "[MiSTer] Output closed.");
}

// =================================================================================
//  Connection
// =================================================================================

bool Output::TryConnect()
{
  gmw_set_log_callback(&GmwLogSink, m_cfg_log_verbosity);

  if (m_cfg_codec == Codec::NLC)
  {
    if (m_cfg_nlc_pack == NlcPack::Rice)
    {
      // Not negotiated. A core without the Rice decoder ignores the bit and parses Rice
      // bytes as Tiled, which produces a garbage picture rather than an error.
      INFO_LOG_FMT(VIDEO,
                   "[MiSTer] NLC codec: pack=Rice NEAR={} - REQUIRES a core with the Rice "
                   "decoder (rbf_rice_r3 kit or newer). On an older core the picture will be "
                   "garbage; set NlcPack = 1 for Tiled if so.",
                   m_cfg_nlc_near_level);
    }
    else
    {
      INFO_LOG_FMT(VIDEO, "[MiSTer] NLC codec: pack=Tiled NEAR={}.", m_cfg_nlc_near_level);
    }
  }

  // Drop anything half-alive from a previous attempt.
  gmw_close();

  // Everything below has to happen before gmw_init(); afterwards it is a silent no-op,
  // because all of it rides the CMD_INIT packing.
  if (m_cfg_codec == Codec::NLC)
  {
    gmw_set_nlc_pack(static_cast<u8>(m_cfg_nlc_pack));
    gmw_set_near_level(static_cast<u8>(m_cfg_nlc_near_level));
  }

  // Advertise GMW_CAP_KEEPALIVE. The core applies its idle timeout only to clients that ask
  // for it, which is what lets a crashed Dolphin release the CRT instead of leaving its last
  // frame up. It commits us to sending while idle, which SenderLoop's timed wait does.
  gmw_set_keepalive(1);

  // The reconnect watchdog is opt-in; arm it, and the client replays the stashed modeline
  // itself after an internal reconnect.
  gmw_set_auto_reconnect(1);

  if (gmw_init(m_cfg_host.c_str(), m_codec, m_sound_rate, m_sound_chan, m_rgb_mode,
               static_cast<u16>(m_cfg_mtu)) < 0)
  {
    return false;
  }

  {
    std::lock_guard<std::mutex> guard(m_status_lock);
    m_status.connected = true;
  }

  // A fresh CMD_INIT restarts the core's frame counter at zero, so ours restarts with it.
  // Re-seed the epoch from the client rather than assuming a value: it is per-object and
  // restarts at 0 here, while the client's watchdog increments it within an object's life,
  // so the sender loop compares it for inequality rather than for growth.
  m_blit_frame = 0;
  m_reconnect_epoch = gmw_reconnect_epoch();
  m_keepalive.Reset(Common::Timer::NowMs());

  // Force the modeline to be re-sent on the new connection.
  m_have_mode = false;
  m_src_w = m_src_h = 0;

  INFO_LOG_FMT(VIDEO, "[MiSTer] Connected to {} (client v{}).", m_cfg_host, gmw_get_version());
  return true;
}

void Output::DoGroovyClose()
{
  // Tell the MiSTer we are leaving so it returns to its connection-search screen instead of
  // holding the last frame. Sent three times; one lost datagram would strand it.
  for (int i = 0; i < 3; i++)
    gmw_send_close();

  gmw_close();

  std::lock_guard<std::mutex> guard(m_status_lock);
  m_status.connected = false;
}

// =================================================================================
//  switchres
// =================================================================================

bool Output::InitSwitchres()
{
  if (m_sr_inited)
    return true;

  sr_init();
  sr_set_log_callback_error(reinterpret_cast<void*>(&SrLogSink));
  sr_set_log_callback_info(reinterpret_cast<void*>(&SrLogSink));
  sr_set_log_callback_debug(reinterpret_cast<void*>(&SrLogSink));

  // switchres defaults to generic_15, a 15 kHz-only band that refuses every 31 kHz mode, and
  // matches preset names with a raw strcmp, falling back to that default on an unknown name.
  // Lowercase and validate before trusting the setting.
  std::string preset = m_cfg_monitor_preset;
  Common::ToLower(&preset);
  if (preset.empty() || !IsKnownMonitorPreset(preset))
  {
    if (!preset.empty())
    {
      WARN_LOG_FMT(VIDEO, "[MiSTer] Unknown monitor preset '{}'; using '{}'.", preset,
                   DEFAULT_MONITOR);
    }
    preset = DEFAULT_MONITOR;
  }

  // "custom" takes its timings from a Switchres INI (monitor custom + crt_range*). Without
  // one there is nothing to define the ranges.
  if (preset == "custom" && m_cfg_switchres_ini.empty())
  {
    WARN_LOG_FMT(VIDEO,
                 "[MiSTer] Monitor preset is 'custom' but no SwitchresIni is set; using '{}'.",
                 DEFAULT_MONITOR);
    preset = DEFAULT_MONITOR;
  }

  // After sr_init(), which parses any switchres.ini in the working directory, so this
  // overrides a stray ini's `monitor` line. "custom" is left unset: the INI's own
  // `monitor custom` line supplies it.
  if (preset != "custom")
    sr_set_monitor(preset.c_str());

  // "dummy" is calculate-only: switchres must never touch the host's display, only do the
  // modeline maths. The library is compiled SR_CALC_ONLY, so no backend is linked.
  sr_init_disp("dummy", nullptr);

  if (!m_cfg_switchres_ini.empty())
    sr_load_ini(const_cast<char*>(m_cfg_switchres_ini.c_str()));

  m_sr_inited = true;

  // Log what switchres settled on. A mismatch with `preset` means a stray switchres.ini or a
  // fallback is in play, which otherwise surfaces only as "no valid signal" on the CRT.
  sr_state st{};
  sr_get_state(&st);
  INFO_LOG_FMT(VIDEO, "[MiSTer] switchres ready (requested monitor '{}', active '{}').", preset,
               st.monitor);
  return true;
}

void Output::ShutdownSwitchres()
{
  if (!m_sr_inited)
    return;

  sr_deinit();
  m_sr_inited = false;
}

bool Output::EnsureMode(u32 src_w, u32 src_h, double refresh_hz)
{
  // Reject a not-yet-ready source triple without caching it, so it is retried on the next
  // frame rather than latched as a failed mode: VI reports no refresh rate at all until it
  // has been programmed. The window is deliberately wide - it only screens out garbage, and
  // switchres applies its own per-monitor refresh tolerance.
  if (src_w == 0 || src_h == 0 || refresh_hz < 40.0 || refresh_hz > 130.0)
    return false;

  const bool unchanged = (src_w == m_src_w && src_h == m_src_h &&
                          std::abs(refresh_hz - m_src_hz) < 0.01);
  if (unchanged)
    return m_have_mode;

  m_src_w = src_w;
  m_src_h = src_h;
  m_src_hz = refresh_hz;
  m_have_mode = false;

  // Let switchres pick progressive or interlaced from the monitor's frequency bands rather
  // than asking for one: a 640x480@60 frame is 31 kHz progressive on a tri-sync monitor and
  // 15 kHz interlaced on an arcade_15, and both are the right answer for their display.
  // Interlace::Progressive is the one case that forces the question, by refusing to take an
  // interlaced modeline at all.
  sr_mode srm{};
  const int ok = sr_add_mode(static_cast<int>(src_w), static_cast<int>(src_h), refresh_hz, 0, &srm);
  if (!ok || srm.width <= 0 || srm.height <= 0)
  {
    ERROR_LOG_FMT(VIDEO, "[MiSTer] switchres could not produce a modeline for {}x{}@{:.2f}Hz.",
                  src_w, src_h, refresh_hz);
    return false;
  }

  if (srm.interlace && m_cfg_interlace == Interlace::Progressive)
  {
    ERROR_LOG_FMT(VIDEO,
                  "[MiSTer] {}x{}@{:.2f}Hz only fits an interlaced modeline on monitor preset "
                  "'{}', but Interlace is set to Progressive. Nothing will be streamed for "
                  "this mode.",
                  src_w, src_h, refresh_hz, m_cfg_monitor_preset);
    return false;
  }

  Modeline ml{};
  ml.pclock = static_cast<double>(srm.pclock) / 1000000.0;  // switchres reports Hz
  ml.h_active = static_cast<u16>(srm.width);
  ml.h_begin = static_cast<u16>(srm.hbegin);
  ml.h_end = static_cast<u16>(srm.hend);
  ml.h_total = static_cast<u16>(srm.htotal);
  ml.v_active = static_cast<u16>(srm.height);
  ml.v_begin = static_cast<u16>(srm.vbegin);
  ml.v_end = static_cast<u16>(srm.vend);
  ml.v_total = static_cast<u16>(srm.vtotal);

  // The wire value is the user's choice, not switchres's: a progressive modeline is always
  // 0, an interlaced one is either true fields (1) or a progressive framebuffer over an
  // interlaced signal (2).
  ml.interlace = srm.interlace ? static_cast<u8>(m_cfg_interlace) : 0;

  if (!IsModelineAcceptable(ml, m_bpp, m_cfg_crt_safety_cap))
  {
    const bool malformed = !IsWellFormed(ml);
    const bool too_big = !malformed && !FitsBlitBuffer(ml, m_bpp);

    // The byte-budget case is the only one the user can act on, so it names the remedy if
    // there is one - past a certain size no pixel format fits.
    std::string reason;
    std::string osd_reason;
    if (malformed)
    {
      reason = "the modeline is malformed";
      osd_reason = "malformed modeline";
    }
    else if (too_big)
    {
      const char* remedy = "and no RgbMode is small enough for a mode this large";
      if (m_bpp > 3 && FitsBlitBuffer(ml, 3))
        remedy = "- set RgbMode to 0 (RGB888) or 2 (RGB565)";
      else if (m_bpp > 2 && FitsBlitBuffer(ml, 2))
        remedy = "- set RgbMode to 2 (RGB565)";

      reason = fmt::format("at {} bytes/pixel it needs {} bytes per blit, over the {}-byte "
                           "buffer {}",
                           m_bpp, BlitBytes(ml, m_bpp), MAX_BLIT_BYTES, remedy);
      osd_reason = "too many bytes per frame - see the log for which RgbMode fits";
    }
    else
    {
      reason = fmt::format("it exceeds the CRT safety cap ({}x{} max)", MAX_SAFE_H_ACTIVE,
                           MAX_SAFE_V_ACTIVE);
      osd_reason = "outside the CRT-safe range";
    }

    ERROR_LOG_FMT(VIDEO, "[MiSTer] Refusing modeline {}x{} @ {:.2f}Hz: {}.", ml.h_active,
                  ml.v_active, refresh_hz, reason);
    OSD::AddMessage(fmt::format("MiSTer: refusing {}x{} @ {:.0f}Hz - {}. Streaming is paused "
                                "until the game changes video mode.",
                                ml.h_active, ml.v_active, refresh_hz, osd_reason),
                    OSD::Duration::VERY_LONG, OSD::Color::RED);
    return false;
  }

  m_modeline = ml;
  m_have_mode = true;
  m_dst_w = ml.h_active;
  m_dst_h = ml.v_active;

  // A true-field stream sends one field per blit, so the buffer is half height.
  if (ml.interlace == static_cast<u8>(Interlace::Field))
    m_dst_h = ml.v_active / 2;

  ReleaseGpuResources();  // geometry changed; rebuild lazily on the next capture

  {
    std::lock_guard<std::mutex> guard(m_sr_lock);
    m_pending_modeline = ml;
    m_switchres_pending = true;
  }

  {
    std::lock_guard<std::mutex> guard(m_status_lock);
    m_status.native_width = src_w;
    m_status.native_height = src_h;
    m_status.refresh_hz = refresh_hz;
    m_status.modeline = ml;
    m_status.monitor_preset = m_cfg_monitor_preset;
  }

  // Report the game's native framebuffer and what it became.
  const std::string msg =
      fmt::format("MiSTer: {}x{} @ {:.2f}Hz -> {}x{}{} {:.2f}kHz ({})", src_w, src_h, refresh_hz,
                  ml.h_active, ml.v_active, srm.interlace ? "i" : "p", srm.hfreq / 1000.0,
                  m_cfg_monitor_preset);

  INFO_LOG_FMT(VIDEO, "[MiSTer] {}", msg);
  INFO_LOG_FMT(VIDEO,
               "[MiSTer]   modeline: pclock={:.4f}MHz h({} {} {}) v({} {} {}) interlace={}",
               ml.pclock, ml.h_begin, ml.h_end, ml.h_total, ml.v_begin, ml.v_end, ml.v_total,
               ml.interlace);
  OSD::AddMessage(msg, OSD::Duration::NORMAL);

  return true;
}

// =================================================================================
//  Capture (video thread)
// =================================================================================

void Output::PublishFieldInfo(u8 field, u32 refresh_numerator, u32 refresh_denominator)
{
  if (refresh_numerator == 0 || refresh_denominator == 0)
    return;

  m_vi_refresh_num.store(refresh_numerator, std::memory_order_relaxed);
  m_vi_refresh_den.store(refresh_denominator, std::memory_order_relaxed);
  m_vi_field.store(field, std::memory_order_relaxed);
}

bool Output::EnsureGpuResources()
{
  if (m_dst_w == 0 || m_dst_h == 0)
    return false;

  for (Readback& rb : m_readback)
  {
    if (!rb.tex || rb.tex->GetWidth() != m_dst_w || rb.tex->GetHeight() != m_dst_h)
    {
      rb.pending = false;
      rb.tex.reset();
      rb.tex = g_gfx->CreateStagingTexture(
          StagingTextureType::Readback,
          TextureConfig(m_dst_w, m_dst_h, 1, 1, 1, AbstractTextureFormat::RGBA8, 0,
                        AbstractTextureType::Texture_2DArray));
      if (!rb.tex)
      {
        ERROR_LOG_FMT(VIDEO, "[MiSTer] Failed to create a {}x{} readback texture.", m_dst_w,
                      m_dst_h);
        return false;
      }
    }
  }

  return true;
}

void Output::ReleaseGpuResources()
{
  m_scratch_framebuffer.reset();
  m_scratch_texture.reset();

  for (Readback& rb : m_readback)
  {
    rb.tex.reset();
    rb.pending = false;
  }
  m_readback_idx = 0;
}

void Output::PushFrame(std::vector<u8>&& pixels, u8 field)
{
  std::unique_lock<std::mutex> lock(m_queue_lock);

  if (m_cfg_pacing == Pacing::MisterMaster)
  {
    // The CRT is the clock. Blocking until the sender has room sends backpressure down the
    // video thread and on into emulation, which is what paces the game from the raster
    // rather than from Dolphin's own frame limiter.
    m_space_cv.wait(lock, [this]() {
      return m_queue.size() < MAX_QUEUED_FRAMES || m_quit.load(std::memory_order_acquire);
    });
    if (m_quit.load(std::memory_order_acquire))
      return;
  }
  else
  {
    // Dolphin is the clock. Newest wins: drop rather than grow a backlog.
    while (m_queue.size() >= MAX_QUEUED_FRAMES)
    {
      m_queue.pop_front();
      std::lock_guard<std::mutex> sguard(m_status_lock);
      m_status.frames_dropped++;
    }
  }

  m_queue.push_back(OutFrame{std::move(pixels), field});
  lock.unlock();
  m_queue_cv.notify_one();
}

void Output::Capture(const AbstractTexture* texture, const MathUtil::Rectangle<int>& rect,
                     u32 native_width, u32 native_height)
{
  if (!texture || !g_gfx)
    return;

  const u32 num = m_vi_refresh_num.load(std::memory_order_relaxed);
  const u32 den = m_vi_refresh_den.load(std::memory_order_relaxed);
  if (num == 0 || den == 0)
    return;  // VI has not been programmed yet

  // The game's own framebuffer size, not Dolphin's internal-resolution scale: a 15kHz CRT
  // wants the 640x480 the console actually produced. The upscale is for the host window.
  if (!EnsureMode(native_width, native_height, static_cast<double>(num) / static_cast<double>(den)))
    return;  // no usable modeline - stay quiet

  if (!EnsureGpuResources())
    return;

  // The FPGA derives the field cadence from the modeline, so ProgressiveFB must blit as
  // field 0: sending VI's alternating field there makes the core read consecutive frames
  // from its two field buffers and comb on horizontal motion. Only a true-field stream
  // carries real alternating fields.
  const u8 blit_field = (m_modeline.interlace == static_cast<u8>(Interlace::Field)) ?
                            m_vi_field.load(std::memory_order_relaxed) :
                            0;

  const MathUtil::Rectangle<int> valid = rect;
  if (valid.GetWidth() <= 0 || valid.GetHeight() <= 0)
    return;

  if (!m_logged_first_frame)
  {
    m_logged_first_frame = true;
    INFO_LOG_FMT(VIDEO, "[MiSTer] first frame: xfb={}x{} valid={}x{} native={}x{} -> dst={}x{}",
                 texture->GetWidth(), texture->GetHeight(), valid.GetWidth(), valid.GetHeight(),
                 native_width, native_height, m_dst_w, m_dst_h);
  }

  // Scale to the modeline's active area. This is usually a downscale, because Dolphin
  // renders the XFB at the internal resolution multiplier and the CRT wants native.
  const AbstractTexture* readback_src = texture;
  MathUtil::Rectangle<int> readback_rect = valid;

  if (static_cast<u32>(valid.GetWidth()) != m_dst_w ||
      static_cast<u32>(valid.GetHeight()) != m_dst_h)
  {
    if (!m_scratch_texture || m_scratch_texture->GetWidth() != m_dst_w ||
        m_scratch_texture->GetHeight() != m_dst_h)
    {
      m_scratch_framebuffer.reset();
      m_scratch_texture.reset();
      m_scratch_texture = g_gfx->CreateTexture(
          TextureConfig(m_dst_w, m_dst_h, 1, 1, 1, AbstractTextureFormat::RGBA8,
                        AbstractTextureFlag_RenderTarget, AbstractTextureType::Texture_2DArray),
          "GroovyMiSTer scale target");
      if (!m_scratch_texture)
        return;

      m_scratch_framebuffer = g_gfx->CreateFramebuffer(m_scratch_texture.get(), nullptr);
      if (!m_scratch_framebuffer)
      {
        m_scratch_texture.reset();
        return;
      }
    }

    g_gfx->ScaleTexture(m_scratch_framebuffer.get(), m_scratch_framebuffer->GetRect(), texture,
                        valid);
    readback_src = m_scratch_texture.get();
    readback_rect = m_scratch_texture->GetRect();
  }

  Readback& slot = m_readback[m_readback_idx];
  const MathUtil::Rectangle<int> dst_rect(0, 0, static_cast<int>(m_dst_w),
                                          static_cast<int>(m_dst_h));

  // Drain the frame started on the previous call. The GPU has finished it by now, so the
  // Flush is effectively free: one frame of extra latency in exchange for never blocking
  // the video thread on the GPU, which at 60Hz is the better trade - the MiSTer's own
  // raster-chase is what actually hides the latency.
  if (slot.pending)
  {
    slot.tex->Flush();
    if (slot.tex->Map())
    {
      std::vector<u8> packed;
      PackFrame(static_cast<RgbMode>(m_rgb_mode),
                reinterpret_cast<const u8*>(slot.tex->GetMappedPointer()),
                static_cast<u32>(slot.tex->GetMappedStride()), m_dst_w, m_dst_h, packed);
      slot.tex->Unmap();
      PushFrame(std::move(packed), slot.field);
    }
    slot.pending = false;
  }

  slot.tex->CopyFromTexture(readback_src, readback_rect, 0, 0, dst_rect);
  slot.pending = true;
  slot.field = blit_field;
  m_readback_idx ^= 1u;
}

// =================================================================================
//  Sender thread - sole owner of the Groovy video/audio socket
// =================================================================================

void Output::SenderLoop()
{
  Common::SetCurrentThreadName("GroovyMiSTer Sender");

  // CMD_AUDIO carries its payload size in a u16, so the cast below must not be able to
  // wrap: a 65536-byte drain casts to 0 and puts an empty CMD_AUDIO on the wire, which the
  // core rejects with UDP_ERROR. Cap it well inside u16, at a whole number of 4-byte stereo
  // frames, and small enough not to dump a large stale burst in one packet (~85ms @ 48kHz
  // stereo; steady state is ~3.2KB per frame).
  static constexpr u32 MAX_AUDIO_SEND_BYTES = 16 * 1024;
  static_assert(MAX_AUDIO_SEND_BYTES <= 65535, "CMD_AUDIO size must fit in a u16");
  static_assert((MAX_AUDIO_SEND_BYTES % AudioTap::BYTES_PER_SAMPLE) == 0,
                "whole stereo frames only");

  std::vector<u8> audio_scratch(MAX_AUDIO_SEND_BYTES);
  u64 last_reconnect_attempt_ms = 0;

  m_keepalive.Reset(Common::Timer::NowMs());
  m_reconnect_epoch = gmw_reconnect_epoch();

  while (!m_quit.load(std::memory_order_acquire))
  {
    // Wake on a frame or on a keepalive poll tick. The timed wait is what keeps an idle
    // session alive: every way Dolphin stops producing frames - pause, savestate load, disc
    // change, a modeline the safety gate refused - parks this thread here, and a session
    // that sends nothing for the core's idle timeout is closed core-side.
    OutFrame frame;
    bool have_frame = false;
    {
      std::unique_lock<std::mutex> lock(m_queue_lock);
      m_queue_cv.wait_for(lock, std::chrono::milliseconds(KeepAliveScheduler::POLL_PERIOD_MS),
                          [this]() {
                            return !m_queue.empty() || m_quit.load(std::memory_order_acquire);
                          });
      if (m_quit.load(std::memory_order_acquire))
        break;

      if (!m_queue.empty())
      {
        frame = std::move(m_queue.front());
        m_queue.pop_front();
        have_frame = true;
      }
    }
    if (have_frame)
      m_space_cv.notify_one();

    if (!gmw_is_connected())
    {
      // Retry at a human pace, not a spin. This also runs on idle ticks, so a session lost
      // while the emulator is paused comes back without waiting for frames to resume.
      const u64 now_ms = Common::Timer::NowMs();
      if (now_ms - last_reconnect_attempt_ms >= 2000)
      {
        last_reconnect_attempt_ms = now_ms;
        TryConnect();
      }
      if (!gmw_is_connected())
        continue;
    }

    // The client's internal watchdog may have reconnected underneath us. The counter is
    // deliberately NOT restarted here even though the core's has been: that reconnect
    // happens inside the same client object, whose input filter still compares against the
    // last-seen frame number. The blit below realigns upward from status.frame instead.
    const u32 epoch = gmw_reconnect_epoch();
    if (epoch != m_reconnect_epoch)
    {
      m_reconnect_epoch = epoch;
      INFO_LOG_FMT(VIDEO, "[MiSTer] Client reconnected (epoch {}).", epoch);
    }

    // Receive pending ACKs. Not optional: the client updates fpga.frameEcho only inside
    // getACK(), and its CmdBlit watchdog force-reconnects when frameEcho stops advancing for
    // 10 blits. getStatus() below only copies the cache and gmw_blit() never receives, so
    // without this frameEcho sticks at 0 and every blit drives a reconnect loop - each
    // reconnect sending CMD_CLOSE, which drops the FPGA's video output so the CRT never
    // locks. MisterMaster gets this for free from gmw_waitSync(); DolphinMaster does not.
    //
    // Polling on idle ticks matters too: fpga.frame is the core's own counter and free-runs
    // at the CRT's refresh rate whether or not we blit, so otherwise it would hold its
    // pre-pause value and the first frame after a long pause would be numbered thousands
    // behind the core and discarded as stale.
    gmw_getACK(0);

    // Hold the session open if nothing has gone out lately. Evaluated every iteration, not
    // only on idle ticks: an iteration can carry a frame and still send nothing (the
    // oversized-frame drop below, or a null blit buffer), and gating on !have_frame would
    // let that starve the core's idle timer while looking busy. During normal play
    // ShouldSend() is false because every blit refreshes the timestamp.
    if (m_keepalive.ShouldSend(Common::Timer::NowMs()))
    {
      gmw_send_keepalive();
      m_keepalive.NotifyWireActivity(Common::Timer::NowMs());
    }

    // Idle tick: holding the session open is all there is to do.
    if (!have_frame)
      continue;

    // Any modeline change must land before the frame that depends on it.
    {
      std::lock_guard<std::mutex> guard(m_sr_lock);
      if (m_switchres_pending)
      {
        const Modeline& m = m_pending_modeline;
        if (gmw_switchres(m.pclock, m.h_active, m.h_begin, m.h_end, m.h_total, m.v_active,
                          m.v_begin, m.v_end, m.v_total, m.interlace) < 0)
        {
          // Already retried internally, so the connection is likely down. The
          // gmw_is_connected()/TryConnect() loop above catches that, and the client's own
          // watchdog replays the last-stashed modeline once it reconnects.
          WARN_LOG_FMT(VIDEO, "[MiSTer] Switchres ACK failed; video may stay blank until the "
                              "next reconnect.");
        }
        m_switchres_pending = false;
      }
    }

    // Audio first: the core wants it ahead of the frame it belongs to.
    gmw_fpgaStatus status{};
    gmw_getStatus(&status);
    if (status.audio && m_audio_tap.IsActive())
    {
      const u32 n = m_audio_tap.Read(audio_scratch.data(), MAX_AUDIO_SEND_BYTES);
      if (n > 0)
      {
        std::memcpy(gmw_get_pBufferAudio(), audio_scratch.data(), n);
        gmw_audio(static_cast<u16>(n));
      }
    }

    // The FPGA displays frames in counter order, so ours must stay ahead of what it is
    // showing or the frame is discarded as stale.
    //
    // The forward jump is unbounded on purpose. status.frame free-runs while a paused
    // session is held open, so after a 60s pause the core is legitimately ~3600 frames
    // ahead; clamping the jump would leave us numbering behind the display position with
    // every frame stale.
    m_blit_frame = std::max(m_blit_frame + 1, status.frame + 1);

    // EnsureMode() already refuses any mode whose blit would not fit (FitsBlitBuffer), so
    // reaching this means the queued geometry and the negotiated pixel format have
    // disagreed. Dropping the frame is survivable; the memcpy below, into a fixed-size
    // RIO-registered allocation, is not.
    if (frame.pixels.size() > MAX_BLIT_BYTES)
    {
      if (!m_logged_oversized_frame)
      {
        m_logged_oversized_frame = true;
        ERROR_LOG_FMT(VIDEO,
                      "[MiSTer] Dropping oversized frame: {} bytes exceeds the {}-byte blit "
                      "buffer. This is a bug - the mode gate should have refused it.",
                      frame.pixels.size(), MAX_BLIT_BYTES);
      }

      std::lock_guard<std::mutex> guard(m_status_lock);
      m_status.frames_dropped++;
      continue;
    }

    char* blit_buf = gmw_get_pBufferBlit(frame.field);
    if (blit_buf)
    {
      std::memcpy(blit_buf, frame.pixels.data(), frame.pixels.size());

      // vCountSync = 1: raster-chase at line 1. The client's automatic frame delay mode
      // assumes a synchronous per-frame caller, which this is not.
      gmw_blit(m_blit_frame, frame.field, 1, 0, 0);

      // Gate the keepalive on real wire activity rather than a free-running heartbeat: at
      // 60fps this lands every ~16ms, so the idle threshold is never reached during play.
      // Any CmdAudio above rode the same socket, so one update covers both.
      m_keepalive.NotifyWireActivity(Common::Timer::NowMs());

      std::lock_guard<std::mutex> guard(m_status_lock);
      m_status.frames_sent++;
      m_status.last_encoded_bytes = static_cast<u32>(frame.pixels.size());
      m_status.connected = true;
      // frameEcho/vCountEcho say where the raster was when the FPGA got our frame;
      // frame/vCount say where it is now. The gap is the end-to-end latency.
      if (m_modeline.v_total > 0 && m_src_hz > 0.0)
      {
        const double lines = static_cast<double>(m_modeline.v_total);
        const double frame_ms = 1000.0 / m_src_hz;
        const int dl = static_cast<int>(status.vCount) - static_cast<int>(status.vCountEcho);
        m_status.latency_ms = (static_cast<double>(dl) / lines) * frame_ms;
      }
    }

    if (m_cfg_pacing == Pacing::MisterMaster)
    {
      // Sleep until the CRT is ready for the next frame, making the raster rather than
      // Dolphin's frame limiter the master clock.
      gmw_waitSync();
    }
  }

  DoGroovyClose();
}

Status Output::GetStatus() const
{
  std::lock_guard<std::mutex> guard(m_status_lock);
  return m_status;
}

// =================================================================================
//  Facade (GroovyMiSTer.h) - the only surface upstream Dolphin touches
// =================================================================================

void Open()
{
  if (!Config::Get(Config::GROOVY_MISTER_ENABLED))
    return;

  if (!s_output)
    s_output = std::make_unique<Output>();

  if (!s_output->Open())
    s_output.reset();
}

void Close()
{
  if (s_output)
  {
    s_output->Close();
    s_output.reset();
  }
}

bool IsActive()
{
  return s_output && s_output->IsActive();
}

void OnFieldOutput(u8 field, u32 refresh_rate_numerator, u32 refresh_rate_denominator)
{
  if (s_output)
    s_output->PublishFieldInfo(field, refresh_rate_numerator, refresh_rate_denominator);
}

void OnXFB(const AbstractTexture* texture, const MathUtil::Rectangle<int>& rect, u32 native_width,
           u32 native_height)
{
  if (s_output)
    s_output->Capture(texture, rect, native_width, native_height);
}

bool IsAudioActive()
{
  return s_output && s_output->IsAudioActive();
}

void OnAudioChunk(const s16* samples, u32 frames)
{
  if (s_output)
    s_output->WriteAudio(samples, frames);
}

u32 GetAudioSampleRate()
{
  return s_output ? s_output->GetAudioSampleRate() : 0;
}
}  // namespace GroovyMiSTer
