// CMakePresets.json readable by the oldest CMake the project accepts
// (santinod, #1276): schema 6 needs CMake 3.25, and Ubuntu 22.04's 3.22.1
// refused the presets with "Unrecognized "version" field" while
// CMakeLists.txt asks for 3.22. Each schema, the first CMake that reads it.
#include "MpTest.h"

#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace {

using mp::test::Category;

std::string readSourceFile(const char* name) {
  std::ifstream f(std::string(MP_TEST_FIXTURES_DIR) + "/../../" + name, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

// The CMake version, as major * 100 + minor, that reads presets `schema`.
int firstCMakeFor(int schema) {
  static const int first[] = {0, 319, 320, 321, 323, 324, 325, 327, 328, 330, 331};
  return schema >= 1 && schema <= 10 ? first[schema] : 9999;
}

class PresetsSchemaTest final : public mp::test::Test {
public:
  PresetsSchemaTest() : Test("functional.build.presets-schema", Category::Functional) {}

  void run() override {
    const std::string presets = readSourceFile("CMakePresets.json");
    const std::string lists = readSourceFile("CMakeLists.txt");
    MP_CHECK(!presets.empty() && !lists.empty(), "the presets and CMakeLists.txt are found");

    std::smatch m;
    MP_CHECK(std::regex_search(presets, m, std::regex(R"re("version"\s*:\s*(\d+))re")), "the presets state a version");
    const int schema = std::stoi(m[1]);
    MP_CHECK(std::regex_search(presets, m,
                               std::regex(R"re("cmakeMinimumRequired"\s*:\s*\{\s*"major"\s*:\s*(\d+)\s*,\s*"minor"\s*:\s*(\d+))re")),
             "the presets state the CMake they need");
    const int presetsMinimum = std::stoi(m[1]) * 100 + std::stoi(m[2]);
    MP_CHECK(std::regex_search(lists, m, std::regex(R"re(cmake_minimum_required\(VERSION (\d+)\.(\d+))re")),
             "CMakeLists.txt states its minimum");
    const int listsMinimum = std::stoi(m[1]) * 100 + std::stoi(m[2]);

    MP_CHECK(presetsMinimum == listsMinimum, "the presets ask for the same CMake as CMakeLists.txt: " +
                                                 std::to_string(presetsMinimum) + " and " + std::to_string(listsMinimum));
    MP_CHECK(firstCMakeFor(schema) <= listsMinimum,
             "presets schema " + std::to_string(schema) + " needs CMake " + std::to_string(firstCMakeFor(schema) / 100) +
                 "." + std::to_string(firstCMakeFor(schema) % 100) + ", above the minimum the project states");
  }
};
static PresetsSchemaTest g_presetsSchema;

}  // namespace
