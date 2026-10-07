// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include "Common/CommonTypes.h"

namespace GroovyMiSTer
{
/// Bridges the mixer's output to the Groovy sender thread.
///
/// Audio is mixed on whichever thread is draining the mixer, but only the sender thread may
/// drive the Groovy socket: the RIO send path is not thread-safe and a cross-thread send is
/// dropped without error. The audio thread deposits samples here and the sender drains
/// them.
///
/// Producer: audio thread (Write). Consumer: sender thread (Read). Exactly one of each.
///
/// Overflow drops oldest, so a stalled sender costs stale samples rather than unbounded
/// latency, and the stream self-corrects instead of drifting.
class AudioTap
{
public:
  /// The mixer hands over interleaved stereo s16, which is already the wire format.
  static constexpr u32 CHANNELS = 2;
  static constexpr u32 BYTES_PER_SAMPLE = sizeof(s16) * CHANNELS;

  /// ~256KB, about 1.3 seconds at 48kHz stereo: more headroom than should ever be used, so
  /// a brief sender hiccup costs nothing.
  static constexpr size_t CAPACITY = 256 * 1024;

  void Reset();

  void SetActive(bool active) { m_active.store(active, std::memory_order_release); }
  bool IsActive() const { return m_active.load(std::memory_order_acquire); }

  /// Audio thread. `samples` is interleaved stereo s16, `frames` the number of stereo
  /// frames.
  void Write(const s16* samples, u32 frames);

  /// Sender thread. Copies up to `max_bytes` into `dst`; returns bytes written. Always a
  /// whole number of stereo frames.
  u32 Read(u8* dst, u32 max_bytes);

  u64 GetDroppedBytes() const { return m_dropped_bytes.load(std::memory_order_relaxed); }

private:
  std::mutex m_lock;
  std::vector<u8> m_buffer;
  size_t m_read_pos = 0;
  size_t m_size = 0;  // bytes currently queued

  std::atomic_bool m_active{false};
  std::atomic<u64> m_dropped_bytes{0};
};
}  // namespace GroovyMiSTer
