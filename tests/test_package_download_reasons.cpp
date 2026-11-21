// A package that is not one says what went wrong with its download (#1166).
// Among 39 packages tried, a link that led to a page saved that page under
// the package's name, and a download that never wrote left 4 GB of zeros.
// Both read "not a RAR or ZIP archive", which said nothing about what to do.
#include "MpTest.h"

#include "../src/mp_archive/OrganArchive.h"
#include "../src/mp_core/Utf8Path.h"

#include <filesystem>
#include <fstream>
#include <string>

using mp::test::Category;
namespace fs = std::filesystem;

class PackageDownloadReasonsTest final : public mp::test::Test {
public:
  PackageDownloadReasonsTest() : Test("functional.archive.download-reasons", Category::Functional) {}

  static std::string reasonFor(const fs::path& file) {
    mp::OrganArchive a;
    std::string error;
    if (a.discover(mp::utf8Of(file), error) && a.inspect(error)) return "opened";
    return error;
  }

  void run() override {
    const fs::path dir = fs::temp_directory_path() / "mp-download-reasons";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    // A folder each: packages side by side are opened together.
    auto place = [&](const char* folder, const char* name, const std::string& bytes) {
      fs::create_directories(dir / folder);
      const fs::path p = dir / folder / name;
      std::ofstream(p, std::ios::binary) << bytes;
      return p;
    };
    const fs::path page = place("page", "Savaria_demo.Organ.CompPkg.Hauptwerk.rar",
                                "\xEF\xBB\xBF\n  <!DOCTYPE html><html><head><title>Sign in</title>");
    const fs::path message = place("message", "Ujpest_demo.Organ.CompPkg.Hauptwerk.rar",
                                   "Access denied.\r\nLe lien de t\xC3\xA9l\xC3\xA9" "chargement a expir\xC3\xA9.\r\n");
    const fs::path zeros = place("zeros", "Frankfurt_vol0.CompPkg.Hauptwerk.rar", std::string(100000, '\0'));
    std::string binary = "GIF89a";
    for (int i = 0; i < 200; ++i) binary += static_cast<char>(i % 7 == 0 ? 0 : i);
    const fs::path other = place("other", "Other.CompPkg.Hauptwerk.rar", binary);

    for (const auto& p : {page, message}) {
      const auto reason = reasonFor(p);
      MP_CHECK(reason.find("is text saved under the package's name") != std::string::npos,
               "text in place of the package says so: " + reason);
    }
    // Organ definitions are text too, and files in their own right.
    const fs::path hauptwerk = place("hw", "Test.CompPkg.Hauptwerk.rar",
                                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Hauptwerk FileFormat=\"Organ\">");
    const fs::path grandOrgue = place("go", "GrandOrgue_Test.rar", "; GrandOrgue\r\n[Organ]\r\nChurchName=Test\r\n");
    for (const auto& p : {hauptwerk, grandOrgue}) {
      const auto reason = reasonFor(p);
      MP_CHECK(reason.find("is an organ definition, not a package") != std::string::npos,
               "an organ definition is named as one: " + reason);
    }

    const auto zeroReason = reasonFor(zeros);
    MP_CHECK(zeroReason.find("only zeros") != std::string::npos && zeroReason.find("did not finish") != std::string::npos,
             "a file of zeros says its download did not finish: " + zeroReason);
    const auto otherReason = reasonFor(other);
    MP_CHECK(otherReason.find("not a RAR or ZIP archive") != std::string::npos,
             "anything else keeps the general message: " + otherReason);
    fs::remove_all(dir, ec);
  }
};
static PackageDownloadReasonsTest g_packageDownloadReasons;
