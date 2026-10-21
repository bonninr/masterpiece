// The symbols Verovio 6.3.0 still names from the parts MpVerovio.cmake leaves
// out. None of them is ever reached by the score panel.
#include "editortoolkit_shared.h"
#include "filereader.h"

namespace vrv {

// With NO_EDIT_SUPPORT, Verovio still declares EditorToolkitShared and an
// inline ParseEditorAction(json) that calls the two-argument overload. The
// editor sources are not compiled, and an optimising compiler that guesses
// the one known editor class for Toolkit::Edit() emits a call to that
// overload. No editor is ever created.
bool EditorToolkitShared::ParseEditorAction(const std::string &, bool)
{
    return false;
}

// Verovio's zip reader, without its zip library. The program unpacks a
// compressed score itself, and loads no font from a zip, so the toolkit only
// meets this reader through the option for extra fonts, which is never set.
ZipFileReader::ZipFileReader() : m_file(nullptr) {}
ZipFileReader::~ZipFileReader() {}
void ZipFileReader::Reset() {}
bool ZipFileReader::Load(const std::string &)
{
    return false;
}
bool ZipFileReader::LoadBytes(const std::vector<unsigned char> &)
{
    return false;
}
bool ZipFileReader::HasFile(const std::string &)
{
    return false;
}
std::string ZipFileReader::ReadTextFile(const std::string &)
{
    return {};
}
std::list<std::string> ZipFileReader::GetFileList() const
{
    return {};
}

} // namespace vrv
