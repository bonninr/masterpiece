// Organs installed from their packages (#1024), the way a Hauptwerk
// installation keeps them: one folder, the organs folder, holding
// OrganDefinitions/ and OrganInstallationPackages/<id>/ for every organ put
// in it. An organ installed there opens like any installed organ, its RARs no
// longer needed; the packages of a set that comes in several RARs meet in the
// same tree, whatever order they were installed in.
//
// A package is read as its components, as the Hauptwerk installer lists them:
//
//   Organ   one organ definition, with the installation packages it needs;
//   Data    one installation package: samples, images, its own definition
//           with a name, a supplier and a version;
//   Other   anything else the package holds, by its top folder.
//
// For each, what is already installed is set beside what the package offers,
// and the player chooses: install, upgrade, reinstall, or leave it. Installing
// writes into a folder of its own inside the organs folder first, and moves
// the files into place only once every one of them has been read whole: a
// package cut short, a damaged file or a cancel leaves the organs folder as it
// was. Upgrading writes the package's files over the installed ones and keeps
// the rest, as a patch expects.
//
// JUCE-free: the window that drives it is in mp_ui.
#pragma once

#include "OrganArchive.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mp {

struct InstallComponent {
  enum class Kind { Organ, Data, Other };
  Kind kind = Kind::Other;
  // Organ: the definition's placed path ("OrganDefinitions/Cracov.Organ_Hauptwerk_xml",
  // or a GrandOrgue organ's folder, "GrandOrgue/Cracov"); Data: the package's
  // folder ("OrganInstallationPackages/000697"); Other: its top folder, or ""
  // for files at the top. Unique within a package and within an organs folder.
  std::string id;
  std::string name;
  std::string supplier;
  std::string version;  // as written: "1.02"; empty when the package says none
  int64_t bytes = 0;
  int files = 0;
  // Organ: the installation packages its definition names, as six-digit
  // folder names ("000697"); the standard components (000001) left out.
  std::vector<std::string> needs;
  // Data: its six-digit folder name.
  std::string packageId;
};

// The components a package holds, from its index and from the small files
// index() unpacked into `smallFilesDir` (definitions are read there for their
// names and versions).
std::vector<InstallComponent> packageComponents(const OrganArchive& archive,
                                                const std::string& smallFilesDir);

// The components installed in an organs folder, or in any folder laid out
// like one (a Hauptwerk library).
std::vector<InstallComponent> installedComponents(const std::string& root);

// Versions as packages write them: "1.02" is a decimal number, "2.1.3" a
// dotted one. Negative, zero or positive as a is older, the same or newer.
int compareVersions(const std::string& a, const std::string& b);

// What installing a component would do, given the same component installed.
enum class InstallState {
  New,       // not installed
  Newer,     // the package's is newer: an upgrade
  Same,      // the same version is installed
  Older,     // the installed one is newer
};
InstallState installStateOf(const InstallComponent& offered, const InstallComponent* installed);
// Installed by default: a new component and an upgrade. The same version and
// an older one are left alone unless the player asks.
bool installsByDefault(InstallState state);

// Install `chosen` (components of `archive`) into `root`. `progress` gets a
// fraction 0..1 and returns false to cancel. False with `error` on failure;
// `error` is "cancelled" for a cancel. Either way nothing in `root` has
// changed unless it returned true.
bool installComponents(const OrganArchive& archive, const std::vector<InstallComponent>& chosen,
                       const std::string& root, std::string& error,
                       const std::function<bool(double)>& progress = {});

// The bytes installing `chosen` writes, and the bytes free where `root` is
// (its nearest existing folder). -1 when the system cannot say.
int64_t installBytes(const std::vector<InstallComponent>& chosen);
int64_t freeBytesAt(const std::string& root);

// Remove installed components from `root`. Only paths inside `root` are
// touched. False with `error` naming what could not be removed.
bool uninstallComponents(const std::string& root, const std::vector<InstallComponent>& chosen,
                         std::string& error);

// The installed organs that need a package, by name.
std::vector<std::string> organsNeeding(const std::vector<InstallComponent>& installed,
                                       const std::string& packageId);

// The organs an organs folder holds, as paths of their definitions: Hauptwerk
// definitions in OrganDefinitions/, GrandOrgue ones in GrandOrgue/<organ>/.
std::vector<std::string> installedOrganFiles(const std::string& root);

}  // namespace mp
