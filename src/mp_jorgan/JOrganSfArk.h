// SoundFonts packed with sfArk: a .sfArk file, or a self-extracting
// .sfArk.exe, in place of the .sf2 a disposition names. Unpacked once, by
// sfArkLib (GPL-3), into the organ's work folder.
#pragma once

#include <filesystem>
#include <string>

namespace mp {

// The packed file beside a SoundFont that is not there: "x.sfArk" or
// "x.sfArk.exe" for "x.sf2", in any case. Empty when there is none, or in a
// build without sfArkLib.
std::filesystem::path sfArkFor(const std::filesystem::path& soundFont);

// Unpack `packed` into `sf2`. False, with the reason, when it does not unpack.
bool unpackSfArk(const std::filesystem::path& packed, const std::filesystem::path& sf2, std::string& error);

}  // namespace mp
