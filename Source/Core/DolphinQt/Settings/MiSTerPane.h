// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWidget>

class ConfigBool;
class ConfigInteger;
class ConfigStringChoice;
class ConfigText;
class QWidget;

namespace GroovyMiSTer
{
enum class Codec : int;
enum class Interlace : int;
enum class NlcPack : int;
}  // namespace GroovyMiSTer

template <typename T>
class ConfigChoiceMap;

/// Where the console's video and audio go when they are not going to a host window.
///
/// Every control writes straight through to Dolphin.ini [GroovyMiSTer]; the output reads
/// that section when a game boots, so a change applies to the next game rather than the
/// one already running.
class MiSTerPane final : public QWidget
{
  Q_OBJECT
public:
  explicit MiSTerPane(QWidget* parent = nullptr);

private:
  void CreateLayout();
  void ConnectLayout();
  void AddDescriptions();
  void UpdateEnabledState();

  ConfigBool* m_enabled;
  ConfigText* m_host;
  ConfigStringChoice* m_monitor;
  ConfigChoiceMap<GroovyMiSTer::Interlace>* m_interlace;
  ConfigChoiceMap<GroovyMiSTer::Codec>* m_codec;
  ConfigChoiceMap<GroovyMiSTer::NlcPack>* m_nlc_pack;
  ConfigInteger* m_near_level;
  ConfigBool* m_audio;
  ConfigInteger* m_mtu;
  ConfigBool* m_crt_safety_cap;

  // Everything that only means something once the output is switched on.
  QWidget* m_settings_group;
};
