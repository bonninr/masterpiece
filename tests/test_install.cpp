// The organ installer (#1024): a package read as its components, installed
// into an organs folder, upgraded, cancelled, cut short, and removed again.
#include "MpTest.h"

#include "../src/mp_archive/OrganArchive.h"
#include "../src/mp_archive/OrganInstaller.h"
#include "../src/mp_core/Utf8Path.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using mp::test::Category;

namespace {

namespace fs = std::filesystem;
using Files = std::vector<std::pair<std::string, std::string>>;

uint32_t crc32(const std::string& d) {
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char b : d) {
    c ^= b;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

std::string le(uint32_t v, int bytes) {
  std::string out;
  for (int i = 0; i < bytes; ++i) out += static_cast<char>((v >> (8 * i)) & 0xff);
  return out;
}

std::string header(const std::string& body) { return le(crc32(body) & 0xffff, 2) + body; }

// A RAR 4 archive with its files stored, as a package is laid out.
std::string rar4(const Files& files) {
  std::string b("Rar!\x1a\x07\x00", 7);
  b += header(std::string("\x73", 1) + le(0, 2) + le(13, 2) + std::string(6, '\0'));
  for (const auto& [name, data] : files) {
    b += header(std::string("\x74", 1) + le(0x8000, 2) + le(32 + static_cast<uint32_t>(name.size()), 2) +
                le(static_cast<uint32_t>(data.size()), 4) + le(static_cast<uint32_t>(data.size()), 4) +
                std::string("\x02", 1) + le(crc32(data), 4) + le(0x5A2E0000u, 4) +
                std::string("\x14\x30", 2) + le(static_cast<uint32_t>(name.size()), 2) + le(0x20, 4) + name);
    b += data;
  }
  return b + header(std::string("\x7b", 1) + le(0x4000, 2) + le(7, 2));
}

std::string pattern(int k, int n) {
  std::string out(static_cast<size_t>(n), '\0');
  for (int i = 0; i < n; ++i) out[static_cast<size_t>(i)] = static_cast<char>((i * 7 + k * 13) % 251);
  return out;
}

std::string slurp(const fs::path& f) {
  std::ifstream in(f, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string organXml(const std::vector<int>& packages, int version) {
  std::string x = "<?xml version=\"1.0\"?>\n<Hauptwerk FileFormat=\"Organ\">\n"
                  "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Tiny</Identification_Name>"
                  "<Control_OrganVersion>" + std::to_string(version) + "</Control_OrganVersion></_General></ObjectList>\n"
                  "<ObjectList ObjectType=\"RequiredInstallationPackage\">";
  for (int p : packages)
    x += "<RequiredInstallationPackage><InstallationPackageID>" + std::to_string(p) +
         "</InstallationPackageID></RequiredInstallationPackage>";
  return x + "</ObjectList>\n</Hauptwerk>\n";
}

std::string packageXml(const std::string& id, const std::string& name, const std::string& version) {
  return "<?xml version=\"1.0\"?>\n<Hauptwerk FileFormat=\"InstallationPackage\">\n<ObjectList ObjectType=\"_General\">"
         "<_General><UniqueInstallationPackageID>" + id + "</UniqueInstallationPackageID><Name>" + name +
         "</Name><SupplierName>Tiny &amp; Sons</SupplierName><PackageVersion>" + version +
         "</PackageVersion></_General></ObjectList>\n</Hauptwerk>\n";
}

// A package written to `dir`, opened and indexed, its small files in `dir/small-<name>`.
bool openPackage(const fs::path& dir, const std::string& name, const std::string& bytes, mp::OrganArchive& a,
                 std::string& small) {
  const fs::path file = dir / name;
  std::ofstream(file, std::ios::binary) << bytes;
  small = mp::utf8Of(dir / ("small-" + name));
  std::string error;
  return a.discover(mp::utf8Of(file), error) && a.inspect(error) && a.index(error, {}, small);
}

const mp::InstallComponent* byId(const std::vector<mp::InstallComponent>& list, const std::string& id) {
  for (const auto& c : list)
    if (c.id == id) return &c;
  return nullptr;
}

}  // namespace

class OrganInstallerTest final : public mp::test::Test {
public:
  OrganInstallerTest() : Test("functional.install.organ-package", Category::Functional) {}
  void run() override {
    const fs::path dir = fs::temp_directory_path() / "mp-installer";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path root = dir / "Organs";
    const std::string rootUtf8 = mp::utf8Of(root);

    const Files tiny = {
        {"OrganInstallationPackages\\000123\\PackageID000123.InstallationPackageDefinition_Hauptwerk_xml",
         packageXml("000123", "Tiny Package", "1.02")},
        {"OrganInstallationPackages\\000123\\pipe1.wav", pattern(1, 40000)},
        {"OrganInstallationPackages\\000123\\pipe2.wav", pattern(2, 30000)},
        {"OrganInstallationPackages\\000123\\Images\\console.png", pattern(3, 5000)},
        {"OrganInstallationPackages\\000123\\empty.txt", ""},
        {"Temperaments\\Werckmeister.Temperament_Hauptwerk_xml", "<Hauptwerk>t</Hauptwerk>"},
        {"Licence.rtf", "licence"},
        // A definition at the top: a Hauptwerk installation keeps it in
        // OrganDefinitions/.
        {"Tiny.Organ_Hauptwerk_xml", organXml({1, 123}, 3)}};

    // The components, as the Hauptwerk installer lists them.
    mp::OrganArchive a;
    std::string small;
    MP_CHECK(openPackage(dir, "Tiny.CompPkg.Hauptwerk.rar", rar4(tiny), a, small), "the package opens");
    const auto offered = mp::packageComponents(a, small);
    MP_CHECK(offered.size() == 4, "an organ, a data package and two others: " + std::to_string(offered.size()));
    const auto* organ = byId(offered, "OrganDefinitions/Tiny.Organ_Hauptwerk_xml");
    MP_CHECK(organ != nullptr && organ->kind == mp::InstallComponent::Kind::Organ && organ->name == "Tiny" &&
                 organ->version == "3",
             "the organ, by its definition's name and version");
    MP_CHECK(organ != nullptr && organ->needs == std::vector<std::string>{"000123"},
             "it needs its own package; the standard components are left out");
    const auto* data = byId(offered, "OrganInstallationPackages/000123");
    MP_CHECK(data != nullptr && data->kind == mp::InstallComponent::Kind::Data && data->name == "Tiny Package" &&
                 data->version == "1.02" && data->supplier == "Tiny & Sons" && data->packageId == "000123",
             "the package, by its own definition");
    MP_CHECK(data != nullptr && data->files == 5 && data->bytes == 75000 + static_cast<int64_t>(packageXml(
                                                                             "000123", "Tiny Package", "1.02")
                                                                             .size()),
             "with its files and their size");
    MP_CHECK(offered.front().kind == mp::InstallComponent::Kind::Organ && byId(offered, "Temperaments") != nullptr &&
                 byId(offered, "") != nullptr,
             "organs first; the other folders and the files at the top are components too");
    MP_CHECK(mp::installBytes(offered) == mp::installBytes({offered[0]}) + mp::installBytes({offered[1]}) +
                                             mp::installBytes({offered[2]}) + mp::installBytes({offered[3]}),
             "the bytes an install writes add up");

    // Nothing installed: every component is new, and installed by default.
    auto installed = mp::installedComponents(rootUtf8);
    MP_CHECK(installed.empty(), "an organs folder not made yet holds nothing");
    for (const auto& c : offered)
      MP_CHECK(mp::installStateOf(c, byId(installed, c.id)) == mp::InstallState::New &&
                   mp::installsByDefault(mp::InstallState::New),
               c.id + " is new");

    // Installed: every file in its place, the definition beside the packages.
    std::string error;
    std::vector<double> steps;
    MP_CHECK(mp::installComponents(a, offered, rootUtf8, error, [&](double f) { steps.push_back(f); return true; }),
             "installed: " + error);
    MP_CHECK(slurp(root / "OrganDefinitions" / "Tiny.Organ_Hauptwerk_xml") == organXml({1, 123}, 3) &&
                 slurp(root / "OrganInstallationPackages" / "000123" / "pipe1.wav") == pattern(1, 40000) &&
                 slurp(root / "OrganInstallationPackages" / "000123" / "Images" / "console.png") == pattern(3, 5000) &&
                 fs::exists(root / "OrganInstallationPackages" / "000123" / "empty.txt") &&
                 slurp(root / "Temperaments" / "Werckmeister.Temperament_Hauptwerk_xml") == "<Hauptwerk>t</Hauptwerk>" &&
                 slurp(root / "Licence.rtf") == "licence",
             "every file is where an installed organ has it");
    MP_CHECK(!fs::exists(root / ".masterpiece-installing"), "the folder it was put together in is gone");
    MP_CHECK(!steps.empty() && steps.front() == 0.0 && steps.back() == 1.0 &&
                 std::is_sorted(steps.begin(), steps.end()),
             "progress runs from 0 to 1");
    MP_CHECK(mp::installedOrganFiles(rootUtf8) ==
                 std::vector<std::string>{mp::utf8Of(root / "OrganDefinitions" / "Tiny.Organ_Hauptwerk_xml")},
             "the organ is listed for opening");

    installed = mp::installedComponents(rootUtf8);
    MP_CHECK(installed.size() == 4, "four components installed: " + std::to_string(installed.size()));
    const auto* had = byId(installed, "OrganInstallationPackages/000123");
    MP_CHECK(had != nullptr && had->version == "1.02" && had->files == 5,
             "the installed package reads back with its version");
    for (const auto& c : offered)
      MP_CHECK(mp::installStateOf(c, byId(installed, c.id)) == mp::InstallState::Same &&
                   !mp::installsByDefault(mp::InstallState::Same),
               c.id + " is the same version now, left alone by default");
    MP_CHECK(mp::organsNeeding(installed, "000123") == std::vector<std::string>{"Tiny"},
             "the package is needed by the organ");

    // Versions as packages write them.
    MP_CHECK(mp::compareVersions("1.02", "1.1") < 0 && mp::compareVersions("1.10", "1.02") > 0 &&
                 mp::compareVersions("2.1.3", "2.1.10") < 0 && mp::compareVersions("3", "3.0") == 0 &&
                 mp::compareVersions("", "1.0") == 0,
             "versions compare as numbers");

    // An upgrade that carries only what changed, as a patch does: its files
    // replace the installed ones, the rest stay.
    const Files patch = {
        {"OrganInstallationPackages\\000123\\PackageID000123.InstallationPackageDefinition_Hauptwerk_xml",
         packageXml("000123", "Tiny Package", "1.10")},
        {"OrganInstallationPackages\\000123\\pipe1.wav", pattern(9, 41000)}};
    mp::OrganArchive p;
    MP_CHECK(openPackage(dir, "TinyPatch.CompPkg.Hauptwerk.rar", rar4(patch), p, small), "the patch opens");
    const auto upgrade = mp::packageComponents(p, small);
    MP_CHECK(upgrade.size() == 1 &&
                 mp::installStateOf(upgrade[0], byId(installed, upgrade[0].id)) == mp::InstallState::Newer &&
                 mp::installsByDefault(mp::InstallState::Newer),
             "the patch is an upgrade, installed by default");

    // Cancelled half way: the installed organ is untouched.
    error.clear();
    MP_CHECK(!mp::installComponents(p, upgrade, rootUtf8, error, [](double f) { return f < 0.5; }) &&
                 error == "cancelled",
             "the upgrade can be cancelled: " + error);
    MP_CHECK(slurp(root / "OrganInstallationPackages" / "000123" / "pipe1.wav") == pattern(1, 40000) &&
                 !fs::exists(root / ".masterpiece-installing"),
             "a cancel leaves the organs folder as it was");

    error.clear();
    MP_CHECK(mp::installComponents(p, upgrade, rootUtf8, error), "upgraded: " + error);
    MP_CHECK(slurp(root / "OrganInstallationPackages" / "000123" / "pipe1.wav") == pattern(9, 41000) &&
                 slurp(root / "OrganInstallationPackages" / "000123" / "pipe2.wav") == pattern(2, 30000),
             "the patch's file replaced, the others kept");
    installed = mp::installedComponents(rootUtf8);
    MP_CHECK(byId(installed, "OrganInstallationPackages/000123")->version == "1.10", "now version 1.10");
    MP_CHECK(mp::installStateOf(offered[1], byId(installed, offered[1].id)) == mp::InstallState::Older &&
                 !mp::installsByDefault(mp::InstallState::Older),
             "the first package is now the older one, left alone by default");

    // A package cut short, as a download that stopped: it does not install,
    // and nothing installed changes.
    const Files second = {
        {"OrganInstallationPackages\\000124\\PackageID000124.InstallationPackageDefinition_Hauptwerk_xml",
         packageXml("000124", "Tiny Pedal", "1.00")},
        {"OrganInstallationPackages\\000124\\pedal1.wav", pattern(4, 50000)},
        {"OrganInstallationPackages\\000124\\pedal2.wav", pattern(5, 50000)}};
    const std::string whole = rar4(second);
    mp::OrganArchive cut;
    {
      const fs::path file = dir / "Cut.CompPkg.Hauptwerk.rar";
      std::ofstream(file, std::ios::binary) << whole.substr(0, whole.size() - 30000);
      small = mp::utf8Of(dir / "small-cut");
      MP_CHECK(cut.discover(mp::utf8Of(file), error) && cut.inspect(error), "the cut package opens: " + error);
      cut.index(error, {}, small);  // its list may stop short; what it has is listed
    }
    error.clear();
    const auto cutOffered = mp::packageComponents(cut, small);
    MP_CHECK(byId(cutOffered, "OrganInstallationPackages/000124") != nullptr, "the cut package is listed");
    MP_CHECK(!mp::installComponents(cut, cutOffered, rootUtf8, error) && !error.empty(),
             "a package cut short does not install");
    MP_CHECK(!fs::exists(root / "OrganInstallationPackages" / "000124") && !fs::exists(root / ".masterpiece-installing"),
             "and leaves nothing behind: " + error);

    // The set's second package, a download of its own (#701): installed into
    // the same organs folder, beside the first.
    mp::OrganArchive s;
    MP_CHECK(openPackage(dir, "TinyPedal.CompPkg.Hauptwerk.rar", whole, s, small), "the second package opens");
    error.clear();
    MP_CHECK(mp::installComponents(s, mp::packageComponents(s, small), rootUtf8, error), "second: " + error);
    installed = mp::installedComponents(rootUtf8);
    MP_CHECK(byId(installed, "OrganInstallationPackages/000124") != nullptr &&
                 byId(installed, "OrganInstallationPackages/000123") != nullptr,
             "both packages are installed side by side");

    // Removed again: the package, the temperaments by their record, the
    // organ. The rest stays.
    error.clear();
    MP_CHECK(mp::uninstallComponents(rootUtf8,
                                     {*byId(installed, "OrganInstallationPackages/000124"),
                                      *byId(installed, "Temperaments")},
                                     error),
             "uninstalled: " + error);
    MP_CHECK(!fs::exists(root / "OrganInstallationPackages" / "000124") && !fs::exists(root / "Temperaments") &&
                 fs::exists(root / "OrganInstallationPackages" / "000123" / "pipe2.wav") &&
                 fs::exists(root / "OrganDefinitions" / "Tiny.Organ_Hauptwerk_xml") && fs::exists(root / "Licence.rtf"),
             "only what was chosen is removed");
    installed = mp::installedComponents(rootUtf8);
    MP_CHECK(byId(installed, "Temperaments") == nullptr && byId(installed, "") != nullptr,
             "the record of the removed files goes with them");
    MP_CHECK(mp::uninstallComponents(rootUtf8, installed, error), "everything removed: " + error);
    MP_CHECK(mp::installedComponents(rootUtf8).empty() && mp::installedOrganFiles(rootUtf8).empty() &&
                 !fs::exists(root / "OrganDefinitions") && !fs::exists(root / "OrganInstallationPackages"),
             "the organs folder is empty again");

    // Nothing outside the organs folder is ever removed.
    fs::create_directories(dir / "outside");
    std::ofstream(dir / "outside" / "keep.txt") << "keep";
    mp::InstallComponent escape;
    escape.kind = mp::InstallComponent::Kind::Data;
    escape.id = "../outside";
    error.clear();
    MP_CHECK(!mp::uninstallComponents(rootUtf8, {escape}, error) && fs::exists(dir / "outside" / "keep.txt"),
             "a path that leads out of the organs folder is refused");

    // A GrandOrgue package: one organ, in a folder of its own.
    const Files go = {{"Small\\Small.organ", "[Organ]\nChurchName=Small\n"},
                      {"Small\\pipes\\c.wav", pattern(6, 9000)}};
    mp::OrganArchive g;
    MP_CHECK(openPackage(dir, "Small Organ.orgue", rar4(go), g, small), "the GrandOrgue package opens");
    const auto goOffered = mp::packageComponents(g, small);
    MP_CHECK(goOffered.size() == 1 && goOffered[0].kind == mp::InstallComponent::Kind::Organ &&
                 goOffered[0].id == "GrandOrgue/Small Organ" && goOffered[0].name == "Small",
             "one organ, named by its organ file");
    error.clear();
    MP_CHECK(mp::installComponents(g, goOffered, rootUtf8, error), "GrandOrgue installed: " + error);
    MP_CHECK(slurp(root / "GrandOrgue" / "Small Organ" / "Small" / "pipes" / "c.wav") == pattern(6, 9000) &&
                 mp::installedOrganFiles(rootUtf8) ==
                     std::vector<std::string>{mp::utf8Of(root / "GrandOrgue" / "Small Organ" / "Small" / "Small.organ")},
             "its files keep their layout, and the organ is listed");
    installed = mp::installedComponents(rootUtf8);
    MP_CHECK(installed.size() == 1 && installed[0].name == "Small", "and it reads back as one organ");
    MP_CHECK(mp::uninstallComponents(rootUtf8, installed, error) && !fs::exists(root / "GrandOrgue"),
             "and is removed whole");
    fs::remove_all(dir, ec);
  }
};
static OrganInstallerTest g_organInstaller;
