// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace GroovyMiSTer
{
/// Decides when a keepalive datagram goes on the wire. Does no I/O - the caller sends - so
/// the timing contract is testable where the socket path is not.
///
/// Having advertised GM_CAP_KEEPALIVE at CMD_INIT, the core arms an idle timer and ends the
/// session if nothing arrives on the video socket within it (OSD: Server -> Idle timeout,
/// 5s default). Dolphin is silent whenever it is alive and not presenting - paused, loading
/// a savestate, changing discs, or held off because the video mode produced no acceptable
/// modeline - so without a keepalive, pausing drops the MiSTer back to its
/// connection-search screen and takes the CRT with it.
class KeepAliveScheduler
{
public:
  /// Send once the last outbound datagram is this old. The core's shortest timeout is 5s
  /// and the contract is to send at <= timeout/2; UDP has no retransmit, so halving is what
  /// lets one keepalive be lost without losing the session.
  static constexpr u64 IDLE_THRESHOLD_MS = 2000;

  /// How often the caller asks, which is not the send interval:
  ///
  ///     worst-case silence = idle threshold + poll period
  ///
  /// since a poll landing just under the threshold defers the send by a whole period.
  /// Polling at the send rate would turn a 2s threshold into a ~4s worst case, inside a 5s
  /// timeout only until a datagram drops. The poll itself is one comparison.
  static constexpr u64 POLL_PERIOD_MS = 250;

  static_assert(IDLE_THRESHOLD_MS + POLL_PERIOD_MS <= 2500,
                "worst-case silence must stay within half of the core's 5s idle timeout");
  static_assert(2 * (IDLE_THRESHOLD_MS + POLL_PERIOD_MS) < 5000,
                "one lost keepalive must still leave us inside the core's 5s idle timeout");

  /// Call after every outbound datagram - blit and audio alike, not just keepalives.
  /// Gating on real wire activity rather than a free-running heartbeat means that at 60fps
  /// this is refreshed every ~16ms and the threshold is never reached during play.
  void NotifyWireActivity(u64 now_ms) { m_last_wire_ms = now_ms; }

  /// True when the session has been silent long enough to need a keepalive.
  bool ShouldSend(u64 now_ms) const
  {
    // A rewound clock reads as fresh activity rather than underflowing the subtraction
    // into a huge age.
    return now_ms >= m_last_wire_ms && (now_ms - m_last_wire_ms) >= IDLE_THRESHOLD_MS;
  }

  /// Re-arm from a known-quiet baseline, so a fresh connection does not look stale.
  void Reset(u64 now_ms) { m_last_wire_ms = now_ms; }

private:
  u64 m_last_wire_ms = 0;
};
}  // namespace GroovyMiSTer
