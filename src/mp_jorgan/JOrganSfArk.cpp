#include "JOrganSfArk.h"

#include "mp_core/Utf8Path.h"

#include <algorithm>
#include <mutex>
#include <system_error>

#if MP_JORGAN_SFARK
#include <juce_core/juce_core.h>

#include "sfArkLib.h"
#include "sfark/zlib.h"
#endif

namespace mp {

#if !MP_JORGAN_SFARK

std::filesystem::path sfArkFor(const std::filesystem::path&) { return {}; }

bool unpackSfArk(const std::filesystem::path&, const std::filesystem::path&, std::string& error) {
  error = "sfArk is left out in this build";
  return false;
}

#else

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// sfArkLib keeps its state in globals: one unpacking at a time.
std::mutex gSfArkLock;
std::string gSfArkMessage;

}  // namespace

std::filesystem::path sfArkFor(const std::filesystem::path& soundFont) {
  const std::filesystem::path dir = soundFont.parent_path();
  const std::string stem = lower(utf8Of(soundFont.stem()));
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir.empty() ? "." : dir, ec)) {
    const std::string name = lower(utf8Of(entry.path().filename()));
    if (name == stem + ".sfark" || name == stem + ".sfark.exe") return entry.path();
  }
  return {};
}

bool unpackSfArk(const std::filesystem::path& packed, const std::filesystem::path& sf2, std::string& error) {
  std::lock_guard<std::mutex> lock(gSfArkLock);
  gSfArkMessage.clear();
  std::error_code ec;
  std::filesystem::create_directories(sf2.parent_path(), ec);
  const std::filesystem::path part = std::filesystem::path(sf2).replace_extension(".part.sf2");
  const int result = sfkl_Decode(utf8Of(packed).c_str(), utf8Of(part).c_str());
  if (result != SFARKLIB_SUCCESS) {
    error = "sfArk " + std::to_string(result) + (gSfArkMessage.empty() ? std::string() : ": " + gSfArkMessage);
    std::filesystem::remove(part, ec);
    return false;
  }
  std::filesystem::rename(part, sf2, ec);
  if (ec) {
    error = ec.message();
    return false;
  }
  return true;
}

#endif

}  // namespace mp

#if MP_JORGAN_SFARK

// What sfArkLib asks of the program that uses it. Its messages are kept for
// the reason of a failure; the license and notes it carries are written
// beside the SoundFont, as its own tool writes them.
void sfkl_msg(const char* text, int flags) {
  if ((flags & SFARKLIB_MSG_PopUp) != 0 && text != nullptr) mp::gSfArkMessage = text;
}
void sfkl_UpdateProgress(int) {}
bool sfkl_GetLicenseAgreement(const char*, const char*) { return true; }
void sfkl_DisplayNotes(const char*, const char*) {}

// The two zlib calls, on JUCE's zlib.
int mp_sfark_uncompress(unsigned char* dest, unsigned long* destLen, const unsigned char* source,
                        unsigned long sourceLen) {
  juce::GZIPDecompressorInputStream in(new juce::MemoryInputStream(source, sourceLen, false), true,
                                       juce::GZIPDecompressorInputStream::zlibFormat);
  const unsigned long room = *destLen;
  unsigned long got = 0;
  while (got < room) {
    const int n = in.read(dest + got, static_cast<int>(std::min<unsigned long>(room - got, 1u << 20)));
    if (n <= 0) break;
    got += static_cast<unsigned long>(n);
  }
  *destLen = got;
  if (got == room && !in.isExhausted()) {
    char extra;
    if (in.read(&extra, 1) > 0) return Z_BUF_ERROR;
  }
  return got > 0 || sourceLen == 0 ? Z_OK : Z_DATA_ERROR;
}

unsigned long mp_sfark_adler32(unsigned long adler, const unsigned char* buf, unsigned int len) {
  unsigned long a = adler & 0xffff, b = (adler >> 16) & 0xffff;
  for (unsigned int i = 0; i < len; ++i) {
    a = (a + buf[i]) % 65521;
    b = (b + a) % 65521;
  }
  return (b << 16) | a;
}

#endif
