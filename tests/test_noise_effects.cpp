// Noise effects load whole (#1161): a key's and a drawstop's action noise,
// often a silent attack with the sound in its releases, keeps every note and
// every release whatever the loading choices thin out. A GrandOrgue rank that
// takes no retuning is one, as is a rank of the format's Noise table.
#include "MpTest.h"

#include "../src/mp_core/GrandOrgueImport.h"
#include "../src/mp_core/OdfLoader.h"
#include "../src/mp_core/OrganModel.h"
#include "../src/mp_core/SparseLoading.h"

#if defined(MP_TEST_HAS_AUDIO)
#include "../src/mp_audio/SampleLibrary.h"
#endif

#include <cstdio>
#include <string>

using mp::test::Category;

namespace {

// Twelve pipes, each a file with one release: a key action noise when
// `noise`, a speaking stop otherwise.
std::string stopSection(int number, const std::string& name, bool noise) {
  std::string s = "[Stop" + std::string(number < 10 ? "00" : "0") + std::to_string(number) + "]\n";
  s += "Name=" + name + "\nNumberOfLogicalPipes=12\nNumberOfAccessiblePipes=12\n"
       "FirstAccessiblePipeLogicalKeyNumber=1\nWindchestGroup=1\n";
  if (noise) s += "AcceptsRetuning=N\nDefaultToEngaged=Y\nDisplayed=N\n";
  for (int p = 1; p <= 12; ++p) {
    char key[8];
    std::snprintf(key, sizeof key, "Pipe%03d", p);
    const std::string k(key), n = std::to_string(35 + p);
    s += k + "=" + (noise ? std::string("Noises\\BlankLoop.wav") : "Principal\\" + n + ".wav") + "\n";
    s += k + "LoadRelease=" + (noise ? "N" : "Y") + "\n";
    s += k + "ReleaseCount=1\n" + k + "Release001=" + (noise ? "KeyUp\\" : "PrincipalRel\\") + n + ".wav\n";
  }
  return s;
}

mp::OrganModel loadOrgan() {
  const std::string organ =
      "[Organ]\nChurchName=Test\nHasPedals=N\nNumberOfManuals=1\nNumberOfWindchestGroups=1\n"
      "[WindchestGroup001]\nName=Main\n"
      "[Manual001]\nName=Great\nNumberOfLogicalKeys=12\nFirstAccessibleKeyMIDINoteNumber=36\n"
      "NumberOfAccessibleKeys=12\nNumberOfStops=2\nStop001=1\nStop002=2\n" +
      stopSection(1, "Principal 8", false) + stopSection(2, "Key action Great", true);
  const mp::GrandOrgueImportReport rep = mp::convertGrandOrgueText(organ);
  MP_CHECK(rep.ok, "the GrandOrgue definition converts: " + rep.error);
  mp::OdfLoader loader;
  mp::OrganModel model;
  mp::OdfDiagnostics diag;
  mp::OdfLoader::Options options;
  MP_CHECK(loader.loadFromXmlString(rep.xml, "test.organ", options, model, diag), "the converted definition loads");
  return model;
}

size_t releasesOf(const mp::Rank& rank) {
  size_t n = 0;
  for (const auto& pipe : rank.pipes)
    for (const auto& layer : pipe.layers) n += layer.releases.size();
  return n;
}

class NoiseEffectsTest final : public mp::test::Test {
public:
  NoiseEffectsTest() : Test("functional.loading.noise-effects-whole", Category::Functional) {}
  void run() override {
    mp::OrganModel model = loadOrgan();
    MP_CHECK(model.ranks.count(5001) && model.ranks.count(5002), "both stops carry their own pipes as ranks");
    const mp::Rank& stop = model.ranks.at(5001);
    const mp::Rank& noise = model.ranks.at(5002);
    MP_CHECK(!stop.noiseEffect, "a speaking stop is pipework");
    MP_CHECK(noise.noiseEffect, "a rank that takes no retuning is a noise effect");
    MP_CHECK(stop.pipes.size() == 12 && noise.pipes.size() == 12, "twelve pipes each");
    MP_CHECK(releasesOf(noise) == 12, "every key's release is read: " + std::to_string(releasesOf(noise)));

    // Every third note: the stop is thinned, the noise left whole.
    const auto plan = mp::planSparseLoading(model, 3, {}, [](mp::Id) { return true; });
    bool stopThinned = false, noiseThinned = false;
    for (const auto& r : plan.ranks) {
      if (r.rankId == 5001) stopThinned = true;
      if (r.rankId == 5002) noiseThinned = true;
    }
    MP_CHECK(stopThinned, "every third note thins the speaking stop");
    MP_CHECK(!noiseThinned, "and leaves every key of the noise");

#if defined(MP_TEST_HAS_AUDIO)
    // Releases: None. The stop's go, the noise's stay.
    mp::SampleLibrary::pruneReleases(model, mp::SampleLibrary::ReleaseChoice::None);
    MP_CHECK(releasesOf(model.ranks.at(5001)) == 0, "Releases: None leaves out the stop's releases");
    MP_CHECK(releasesOf(model.ranks.at(5002)) == 12,
             "and keeps the noise's, the key coming up: " + std::to_string(releasesOf(model.ranks.at(5002))));
    mp::OrganModel longest = loadOrgan();
    mp::SampleLibrary::pruneReleases(longest, mp::SampleLibrary::ReleaseChoice::Longest);
    MP_CHECK(releasesOf(longest.ranks.at(5002)) == 12, "the longest only keeps them too");
#endif

    // A rank of the Noise table is a noise effect as well.
    const std::string xml =
        "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
        "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>N</Identification_Name></_General></ObjectList>"
        "<ObjectList ObjectType=\"Rank\"><Rank><RankID>7</RankID><Name>Keys</Name></Rank></ObjectList>"
        "<ObjectList ObjectType=\"Noise\"><Noise><RankID>7</RankID></Noise></ObjectList></Hauptwerk>";
    mp::OdfLoader loader;
    mp::OrganModel table;
    mp::OdfDiagnostics diag;
    mp::OdfLoader::Options options;
    MP_CHECK(loader.loadFromXmlString(xml, "t.Organ_Hauptwerk_xml", options, table, diag), "the definition loads");
    MP_CHECK(table.ranks.count(7) && table.ranks.at(7).noiseEffect, "a Noise table rank loads whole");
  }
};
static NoiseEffectsTest g_noiseEffects;

}  // namespace
