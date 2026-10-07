// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Settings/MiSTerPane.h"

#include <array>
#include <string>
#include <utility>

#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

#include "Core/Config/GroovyMiSTerSettings.h"

#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/ConfigControls/ConfigInteger.h"
#include "DolphinQt/Config/ConfigControls/ConfigText.h"

MiSTerPane::MiSTerPane(QWidget* parent) : QWidget(parent)
{
  CreateLayout();
  ConnectLayout();
  AddDescriptions();
  UpdateEnabledState();
}

void MiSTerPane::CreateLayout()
{
  auto* const layout = new QVBoxLayout{this};

  m_enabled = new ConfigBool(tr("Send video and audio to a MiSTer"), Config::GROOVY_MISTER_ENABLED);
  layout->addWidget(m_enabled);

  auto* const blurb = new QLabel(
      tr("The console's picture is streamed over the network to a MiSTer running the GroovyNLC "
         "core, which drives a real CRT from the video mode the game actually programmed rather "
         "than from Dolphin's internal resolution."),
      this);
  blurb->setWordWrap(true);
  layout->addWidget(blurb);

  m_settings_group = new QWidget(this);
  auto* const form = new QFormLayout{m_settings_group};
  form->setContentsMargins(QMargins{});
  form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

  m_host = new ConfigText(Config::GROOVY_MISTER_HOST);
  m_host->setPlaceholderText(tr("e.g. 192.168.1.10"));
  form->addRow(tr("MiSTer address:"), m_host);

  // The switchres presets a console's video modes actually land in. switchres compiles in
  // many more, but the rest are arcade monitor models with no bearing on 240p/480i/480p.
  static const std::array<std::pair<QString, QString>, 8> monitors{{
      {tr("15 kHz arcade / consumer CRT"), QStringLiteral("arcade_15")},
      {tr("15/25 kHz arcade monitor"), QStringLiteral("arcade_15_25")},
      {tr("15/25/31 kHz tri-sync arcade monitor"), QStringLiteral("arcade_15_25_31")},
      {tr("31 kHz arcade monitor"), QStringLiteral("arcade_31")},
      {tr("NTSC television"), QStringLiteral("ntsc")},
      {tr("PAL television"), QStringLiteral("pal")},
      {tr("VGA PC monitor (31 kHz)"), QStringLiteral("vesa_480")},
      {tr("Multisync PC monitor (31-120 kHz)"), QStringLiteral("pc_31_120")},
  }};
  m_monitor = new ConfigStringChoice(monitors, Config::GROOVY_MISTER_MONITOR_PRESET);
  form->addRow(tr("Monitor:"), m_monitor);
  form->addRow(QString{}, new QLabel(tr("What the display can scan."), this));

  m_interlace = new ConfigChoiceMap<GroovyMiSTer::Interlace>(
      {{tr("Interlace them (15 kHz displays)"), GroovyMiSTer::Interlace::ProgressiveFB},
       {tr("Send them as true fields"), GroovyMiSTer::Interlace::Field},
       {tr("Leave them progressive"), GroovyMiSTer::Interlace::Progressive}},
      Config::GROOVY_MISTER_INTERLACE);
  form->addRow(tr("Modes the monitor cannot scan progressively:"), m_interlace);
  form->addRow(QString{}, new QLabel(tr("The MiSTer splits the frame into fields."), this));

  m_codec = new ConfigChoiceMap<GroovyMiSTer::Codec>(
      {{tr("NLC (near-lossless, recommended)"), GroovyMiSTer::Codec::NLC},
       {tr("Uncompressed"), GroovyMiSTer::Codec::Raw}},
      Config::GROOVY_MISTER_CODEC);
  form->addRow(tr("Compression:"), m_codec);

  m_nlc_pack = new ConfigChoiceMap<GroovyMiSTer::NlcPack>(
      {{tr("Rice (3D content)"), GroovyMiSTer::NlcPack::Rice},
       {tr("Tiled (flat 2D content)"), GroovyMiSTer::NlcPack::Tiled}},
      Config::GROOVY_MISTER_NLC_PACK);
  form->addRow(tr("NLC packing:"), m_nlc_pack);

  m_near_level = new ConfigInteger(0, 3, Config::GROOVY_MISTER_NLC_NEAR_LEVEL);
  m_near_level->setSpecialValueText(tr("0 (lossless)"));
  form->addRow(tr("NLC quantisation:"), m_near_level);

  m_audio = new ConfigBool(tr("Send the console's audio too"), Config::GROOVY_MISTER_AUDIO);
  form->addRow(QString{}, m_audio);

  m_crt_safety_cap =
      new ConfigBool(tr("Refuse modes outside a CRT's safe range"), Config::GROOVY_MISTER_CRT_SAFETY_CAP);
  form->addRow(QString{}, m_crt_safety_cap);

  m_mtu = new ConfigInteger(576, 9000, Config::GROOVY_MISTER_MTU, 4);
  form->addRow(tr("Network MTU:"), m_mtu);
  form->addRow(QString{},
               new QLabel(tr("1500 unless every hop carries jumbo frames."), this));

  layout->addWidget(m_settings_group);
  layout->addStretch();
}

void MiSTerPane::ConnectLayout()
{
  connect(m_enabled, &QCheckBox::toggled, this, &MiSTerPane::UpdateEnabledState);
}

void MiSTerPane::AddDescriptions()
{
  m_enabled->SetDescription(
      tr("Streams the console's picture and sound to a MiSTer instead of, or as well as, this "
         "PC.<br><br>Takes effect the next time a game starts."));

  // ConfigText is a plain QLineEdit rather than one of the ToolTip* controls, so it has no
  // description pop-up of its own.
  m_host->setToolTip(tr("The MiSTer's address on your network."));

  m_monitor->SetTitle(tr("Monitor"));
  m_monitor->SetDescription(
      tr("What the display on the other end can scan. This is what decides how a console's video "
         "mode reaches it: a 480p title is 31 kHz, which a 15 kHz CRT cannot show at all and a PC "
         "monitor shows natively."));

  m_interlace->SetTitle(tr("Unscannable Modes"));
  m_interlace->SetDescription(
      tr("What to do with a mode the monitor cannot scan progressively.<br><br><b>Interlace "
         "them</b> sends the whole frame and lets the MiSTer split it into fields, so a 480p "
         "title appears on a 15 kHz CRT. <b>True fields</b> sends one field per blit instead. "
         "<b>Leave them progressive</b> refuses those modes, which is what you want on a monitor "
         "that can scan them."));

  m_codec->SetTitle(tr("Compression"));
  m_codec->SetDescription(
      tr("NLC is a near-lossless codec, and is what brings a 640x480 frame under the core's "
         "ingest ceiling at 60 Hz. Uncompressed only fits the smallest modes."));

  m_nlc_pack->SetTitle(tr("NLC Packing"));
  m_nlc_pack->SetDescription(
      tr("NLC's entropy stage. Rice suits the dithered, photographic output of a 3D console; "
         "Tiled suits flat 2D.<br><br>Rice needs a core with the Rice decoder - on an older one "
         "the picture is garbage rather than an error."));

  m_near_level->SetTitle(tr("NLC Quantisation"));
  m_near_level->SetDescription(
      tr("0 is lossless. 1 is +/-1 per channel, which is invisible on an analogue display and is "
         "usually what keeps a 480-line frame inside the core's bandwidth."));

  m_audio->SetDescription(
      tr("Sends the mixed audio alongside the video, so it comes out of the MiSTer in step with "
         "the picture.<br><br>Choosing the MiSTer audio backend on the Audio page sends it there "
         "and nowhere else; any other backend plays here as well."));

  m_crt_safety_cap->SetDescription(
      tr("Refuses a modeline beyond what a CRT is built to scan. An arcade or consumer CRT driven "
         "far outside its range can be damaged.<br><br>Leave this on unless you know the display "
         "can take it."));

  m_mtu->SetTitle(tr("Network MTU"));
  m_mtu->SetDescription(
      tr("1500 is the safe value. 3800 needs jumbo frames enabled on the core and on every switch "
         "between here and it."));
}

/// Everything below the checkbox only means something when the output is on.
void MiSTerPane::UpdateEnabledState()
{
  m_settings_group->setEnabled(m_enabled->isChecked());
}
