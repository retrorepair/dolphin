// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GroovyMiSTer/GroovyMiSTerAudioTap.h"

#include <algorithm>
#include <cstring>

namespace GroovyMiSTer
{
void AudioTap::Reset()
{
  std::lock_guard<std::mutex> guard(m_lock);
  m_buffer.assign(CAPACITY, 0);
  m_read_pos = 0;
  m_size = 0;
  m_dropped_bytes.store(0, std::memory_order_relaxed);
}

void AudioTap::Write(const s16* samples, u32 frames)
{
  if (!IsActive() || frames == 0)
    return;

  std::lock_guard<std::mutex> guard(m_lock);
  if (m_buffer.size() != CAPACITY) [[unlikely]]
    m_buffer.assign(CAPACITY, 0);

  // A write larger than the ring itself means something upstream is wrong; keep the newest
  // tail rather than corrupt the ring.
  if (static_cast<size_t>(frames) * BYTES_PER_SAMPLE >= CAPACITY)
  {
    const u32 keep_frames = static_cast<u32>(CAPACITY / BYTES_PER_SAMPLE);
    const u32 skip = frames - keep_frames;
    samples += static_cast<size_t>(skip) * CHANNELS;
    frames = keep_frames;
    m_dropped_bytes.fetch_add(static_cast<u64>(skip) * BYTES_PER_SAMPLE,
                              std::memory_order_relaxed);
    m_read_pos = 0;
    m_size = 0;
  }

  const size_t incoming = static_cast<size_t>(frames) * BYTES_PER_SAMPLE;

  // Drop-oldest: make room by advancing the read cursor.
  if (m_size + incoming > CAPACITY)
  {
    const size_t overflow = (m_size + incoming) - CAPACITY;
    m_read_pos = (m_read_pos + overflow) % CAPACITY;
    m_size -= overflow;
    m_dropped_bytes.fetch_add(overflow, std::memory_order_relaxed);
  }

  // Already s16 interleaved stereo, so this is a copy rather than a conversion - but the
  // ring wraps, so it can be two.
  const size_t write_pos = (m_read_pos + m_size) % CAPACITY;
  const size_t first = std::min<size_t>(incoming, CAPACITY - write_pos);
  std::memcpy(&m_buffer[write_pos], samples, first);
  if (incoming > first)
    std::memcpy(&m_buffer[0], reinterpret_cast<const u8*>(samples) + first, incoming - first);

  m_size += incoming;
}

u32 AudioTap::Read(u8* dst, u32 max_bytes)
{
  std::lock_guard<std::mutex> guard(m_lock);

  // Whole stereo frames only; a half-frame desyncs the L/R interleave from there on.
  u32 avail = static_cast<u32>(std::min<size_t>(m_size, max_bytes));
  avail -= (avail % BYTES_PER_SAMPLE);
  if (avail == 0)
    return 0;

  const size_t first = std::min<size_t>(avail, CAPACITY - m_read_pos);
  std::memcpy(dst, &m_buffer[m_read_pos], first);
  if (avail > first)
    std::memcpy(dst + first, &m_buffer[0], avail - first);

  m_read_pos = (m_read_pos + avail) % CAPACITY;
  m_size -= avail;
  return avail;
}
}  // namespace GroovyMiSTer
