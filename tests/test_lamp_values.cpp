// The values the echo sends to a learned button (#1185): a pad grid takes a
// Note On's velocity as a colour, so a stop's pad is lit in one colour and
// dark in a dimmer one. By default a lit button gets 127 and a dark one a
// Note Off, as before.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <vector>

using mp::test::Category;

class LampValuesTest final : public mp::test::Test {
public:
  LampValuesTest() : Test("functional.midi.lamp-values", Category::Functional) {}

  void run() override {
    const juce::File odf(juce::String(MP_TEST_FIXTURES_DIR) + "/minimal.Organ_Hauptwerk_xml");
    const juce::File global = mp::dataFolder().getChildFile("settings.mpglobal");
    const bool had = global.existsAsFile();
    const juce::String before = had ? global.loadFileAsString() : juce::String();
    {
      mp::MasterpieceProcessor proc;
      proc.loadOrgan(odf, 0, false);
      proc.prepareToPlay(48000.0, 256);
      std::vector<juce::MidiMessage> sent;
      proc.outgoingProbe = [&sent](const juce::MidiBuffer& b) {
        for (const auto meta : b) sent.push_back(meta.getMessage());
      };
      juce::AudioBuffer<float> buf(2, 256);
      auto block = [&] {
        juce::MidiBuffer m;
        buf.clear();
        proc.processBlock(buf, m);
      };
      // What note 36 on channel 1 was last sent: its velocity, 0 for a Note
      // On at 0, -1 for a Note Off, -2 for nothing.
      auto last = [&sent] {
        int v = -2;
        for (const auto& m : sent)
          if (m.getChannel() == 1 && m.getNoteNumber() == 36) {
            if (m.isNoteOff(false)) v = -1;
            else if (m.isNoteOnOrOff()) v = m.getVelocity();
          }
        return v;
      };

      mp::MidiBinding pad;  // a stop learned on a pad: note 36, channel 1
      pad.source.kind = mp::MidiSourceKind::Note;
      pad.source.channel = 1;
      pad.source.number = 36;
      pad.targetKind = mp::MidiTargetKind::Switch;
      pad.targetId = 1101;
      pad.trigger = mp::MidiTrigger::Toggle;
      pad.latching = true;
      proc.midiMap().bind(pad);
      proc.setMidiFeedbackEnabled(true);
      block();

      MP_CHECK(proc.midiFeedbackLit() == 127 && proc.midiFeedbackDark() == -1, "by default 127 and a Note Off");
      sent.clear();
      proc.setSwitchEngaged(1101, true);
      block();
      MP_CHECK(last() == 127, "a lit pad gets 127, as before: " + std::to_string(last()));
      sent.clear();
      proc.setSwitchEngaged(1101, false);
      block();
      MP_CHECK(last() == -1, "a dark pad gets a Note Off, as before: " + std::to_string(last()));

      proc.setMidiFeedbackValues(21, 23);  // a colour, and a dimmer one
      sent.clear();
      proc.setSwitchEngaged(1101, true);
      block();
      MP_CHECK(last() == 21, "lit, the pad gets the chosen colour: " + std::to_string(last()));
      sent.clear();
      proc.setSwitchEngaged(1101, false);
      block();
      MP_CHECK(last() == 23, "dark, the pad gets its dim colour: " + std::to_string(last()));

      proc.setMidiFeedbackValues(500, -9);
      MP_CHECK(proc.midiFeedbackLit() == 127 && proc.midiFeedbackDark() == -1, "values out of range are held in range");
      proc.setMidiFeedbackValues(21, 23);
      proc.outgoingProbe = nullptr;
      proc.releaseResources();
    }
    {
      mp::MasterpieceProcessor proc;  // the next start
      proc.loadGlobalDefaults();
      MP_CHECK(proc.midiFeedbackLit() == 21 && proc.midiFeedbackDark() == 23, "the values are kept");
    }
    if (had) global.replaceWithText(before);
    else global.deleteFile();
  }
};
static LampValuesTest g_lampValues;

#endif
