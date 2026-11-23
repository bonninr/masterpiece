// jOrgan consoles, drawn by their skins.
//
// A jOrgan console names a skin: a folder or a ZIP file beside the
// disposition with a skin.xml and its pictures and fonts. The skin's styles
// are stacks of layers -- images (stretched by a nine-part border), texts
// (the element's name or a line of its description), buttons (one child per
// state) and sliders (one child per position) -- and every element on the
// console names a style and a place.
//
// This draws each element in each of its states, as jOrgan's own layout
// rules place every layer, into PNG files in the organ's work folder, and
// the console's own style as its background tile. The console page then
// shows the pictures; nothing reads the skin again until it changes.
#pragma once

#include "JOrganDisposition.h"
#include "JOrganOrgan.h"

#include <string>
#include <vector>

namespace mp {

// The consoles of `d` that name a skin found beside the disposition, drawn
// into `workFolder`/skin. Empty when none does, or in a build without
// graphics. What a skin has that is not drawn goes into `notes`.
std::vector<JOrganOrgan::Console> drawJOrganConsoles(const JOrganDisposition& d, const std::string& dispositionDir,
                                                     const std::string& workFolder, std::vector<std::string>& notes);

// The skin files the consoles of `d` name, as found: for the work folder's stamp.
std::vector<std::string> jorganSkinFiles(const JOrganDisposition& d, const std::string& dispositionDir);

}  // namespace mp
