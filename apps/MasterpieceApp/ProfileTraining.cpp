#include "ProfileTraining.h"

#include <cmath>
#include <iostream>
#include <memory>

#include "../../src/mp_audio/MasterpieceProcessor.h"

namespace mp::app {
namespace {

constexpr int kFirstKey = 36;
constexpr int kKeys = 61;
constexpr int kRanks = 4;
constexpr int kSampleEvery = 3; // keys per sample: the rest are resampled
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

double keyHz(int key) { return 440.0 * std::pow(2.0, (key - 69) / 12.0); }

// Four ranks, each a different tone and one of them stereo, one sample for
// every third key, looped. Every pipe plays the sample nearest below it, so
// most voices run at a resample ratio away from 1, as on a sparsely sampled
// set.
juce::File writeOrgan(const juce::File& root) {
  root.deleteRecursively();
  const auto defs = root.getChildFile("OrganDefinitions");
  const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
  defs.createDirectory();
  pkg.createDirectory();

  juce::String samples, ranks, pipes, layers, attacks, stops, stopRanks, switches;
  for (int r = 0; r < kRanks; ++r) {
    const int channels = r == 3 ? 2 : 1;
    for (int s = 0; s < kKeys; s += kSampleEvery) {
      const int key = kFirstKey + s;
      const double hz = keyHz(key);
      const int sampleId = 10000 + r * 100 + s;
      const juce::String name = "r" + juce::String(r) + "-" + juce::String(key) + ".wav";
      juce::WavAudioFormat fmt;
      std::unique_ptr<juce::FileOutputStream> os(pkg.getChildFile(name).createOutputStream());
      juce::StringPairArray meta;
      meta.set("NumSampleLoops", "1");
      meta.set("Loop0Start", "4800");
      meta.set("Loop0End", "43199");
      std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), kRate, (unsigned)channels,
                                                                     r == 2 ? 24 : 16, meta, 0));
      juce::AudioBuffer<float> tone(channels, 48000);
      for (int c = 0; c < channels; ++c)
        for (int i = 0; i < 48000; ++i) {
          double v = 0.0;
          for (int h = 1; h <= 2 + 3 * r; ++h)
            v += std::sin(2.0 * juce::MathConstants<double>::pi * hz * h * i / kRate + c * 0.3 * h) / (h * (r + 1));
          tone.setSample(c, i, static_cast<float>(0.15 * v * std::min(1.0, i / 2400.0)));
        }
      w->writeFromAudioSampleBuffer(tone, 0, 48000);
      samples << "<Sample><SampleID>" << sampleId << "</SampleID><InstallationPackageID>1</InstallationPackageID>"
              << "<SampleFilename>" << name << "</SampleFilename><Pitch_SpecificationMethodCode>1"
              << "</Pitch_SpecificationMethodCode><Pitch_ExactSamplePitch>" << juce::String(hz, 3)
              << "</Pitch_ExactSamplePitch></Sample>";
    }
    const int rankId = 200 + r;
    ranks << "<Rank><RankID>" << rankId << "</RankID><Name>R" << r << "</Name></Rank>";
    for (int k = 0; k < kKeys; ++k) {
      const int id = 1000 * (r + 1) + k;
      const int sampleId = 10000 + r * 100 + (k / kSampleEvery) * kSampleEvery;
      pipes << "<Pipe_SoundEngine01><PipeID>" << id << "</PipeID><RankID>" << rankId
            << "</RankID><NormalMIDINoteNumber>" << (kFirstKey + k) << "</NormalMIDINoteNumber></Pipe_SoundEngine01>";
      layers << "<Pipe_SoundEngine01_Layer><LayerID>" << id << "</LayerID><PipeID>" << id
             << "</PipeID></Pipe_SoundEngine01_Layer>";
      attacks << "<Pipe_SoundEngine01_AttackSample><UniqueID>" << id << "</UniqueID><LayerID>" << id
              << "</LayerID><SampleID>" << sampleId << "</SampleID></Pipe_SoundEngine01_AttackSample>";
    }
    stops << "<Stop><StopID>" << (900 + r) << "</StopID><Name>S" << r << "</Name><DivisionID>801</DivisionID>"
          << "<ControllingSwitchID>" << (1100 + r) << "</ControllingSwitchID></Stop>";
    stopRanks << "<StopRank><StopID>" << (900 + r) << "</StopID><RankID>" << rankId << "</RankID>"
              << "<MIDINoteNumOfFirstMappedDivisionInputNode>" << kFirstKey
              << "</MIDINoteNumOfFirstMappedDivisionInputNode><NumberOfMappedDivisionInputNodes>" << kKeys
              << "</NumberOfMappedDivisionInputNodes></StopRank>";
    switches << "<Switch><SwitchID>" << (1100 + r) << "</SwitchID><Name>S" << r
             << "</Name><Latching>Y</Latching></Switch>";
  }

  const auto odf = defs.getChildFile("training.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      juce::String("<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
                   "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Training</Identification_Name>"
                   "<Identification_UniqueOrganID>9091</Identification_UniqueOrganID></_General></ObjectList>"
                   "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
                   "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage>"
                   "</ObjectList><ObjectList ObjectType=\"Sample\">") +
      samples + "</ObjectList><ObjectList ObjectType=\"Rank\">" + ranks +
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01\">" + pipes +
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">" + layers +
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">" + attacks +
      "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1"
      "</DefaultInputOutputKeyboardAsgnCode><KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys>"
      "<KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard></ObjectList>"
      "<ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division>"
      "</ObjectList><ObjectList ObjectType=\"KeyAction\"><KeyAction><SourceKeyboardID>701</SourceKeyboardID>"
      "<DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode>"
      "<MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "</ObjectList><ObjectList ObjectType=\"Stop\">" +
      stops + "</ObjectList><ObjectList ObjectType=\"StopRank\">" + stopRanks +
      "</ObjectList><ObjectList ObjectType=\"Switch\">" + switches + "</ObjectList></Hauptwerk>");
  return odf;
}

// Chords across the compass with every stop drawn, then fewer stops: about
// twenty seconds of playing per pass.
double play(MasterpieceProcessor& proc) {
  juce::AudioBuffer<float> buf(2, kBlock);
  double peak = 0.0;
  const auto block = [&](const juce::MidiBuffer& m) {
    buf.clear();
    juce::MidiBuffer copy(m);
    proc.processBlock(buf, copy);
    peak = std::max(peak, (double)buf.getMagnitude(0, kBlock));
  };
  const juce::MidiBuffer none;
  for (int r = 0; r < kRanks; ++r) proc.setStopEngaged(900 + r, true);
  for (int chord = 0; chord < 40; ++chord) {
    if (chord == 24) proc.setStopEngaged(901, false);
    if (chord == 32) proc.setStopEngaged(903, false);
    const int root = kFirstKey + (chord * 7) % 36;
    juce::MidiBuffer on, off;
    for (const int step : {0, 4, 7, 12, 16, 19, 24}) {
      on.addEvent(juce::MidiMessage::noteOn(1, root + step, 0.8f), 0);
      off.addEvent(juce::MidiMessage::noteOff(1, root + step), 0);
    }
    block(on);
    for (int i = 0; i < 90; ++i) block(none);
    block(off);
    for (int i = 0; i < 2; ++i) block(none);
  }
  for (int i = 0; i < 400; ++i) block(none);
  return peak;
}

} // namespace

int trainProfile() {
  const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-profile-training");
  const auto odf = writeOrgan(root);
  int failures = 0;
  struct Pass {
    const char* name;
    SampleStorage storage;
    bool coded;
  };
  for (const Pass& pass : {Pass{"int24", SampleStorage::Int24, false}, Pass{"int16", SampleStorage::Int16, false},
                           Pass{"float32", SampleStorage::Float32, false},
                           Pass{"compressed", SampleStorage::Int24, true}}) {
    // On the heap, as the program keeps it: the processor is large.
    auto owned = std::make_unique<MasterpieceProcessor>();
    auto& proc = *owned;
    proc.overrideCacheMode(SampleLibrary::CacheMode::Off);
    proc.setSampleStorage(pass.storage);
    proc.setCodeSamples(pass.coded);
    proc.prepareToPlay(kRate, kBlock);
    const auto loaded = proc.loadOrgan(odf, 0, false);
    const double peak = loaded.ok ? play(proc) : 0.0;
    std::cout << "train-profile " << pass.name << ": " << (loaded.ok ? "loaded" : loaded.error)
              << ", peak " << peak << std::endl;
    if (!loaded.ok || peak <= 0.0) ++failures;
    proc.releaseResources();
    proc.settingsFileFor(odf).deleteFile();
  }
  root.deleteRecursively();
  return failures == 0 ? 0 : 1;
}

} // namespace mp::app
