// Finding a file whose name the organ definition spells in another case.
//
// Sets are authored on Windows, where "pipes/go/04-rea" and "pipes/GO/04-Rea"
// are the same folder. On Linux they are not, and on a case-sensitive macOS
// volume neither: a set whose definition and disk disagree in any folder's
// case loaded no samples at all (#90, Burton Berlin). Every part of the path is
// matched ignoring case, not only the file name.
#pragma once

#include <filesystem>

namespace mp {

// `wanted` itself when it exists; otherwise the path on disk that matches it
// ignoring case in every component; otherwise `wanted` unchanged. Folder
// listings are cached, since a set asks for thousands of files in the same
// few folders. Thread-safe.
std::filesystem::path resolvePathIgnoringCase(const std::filesystem::path& wanted);

}  // namespace mp
