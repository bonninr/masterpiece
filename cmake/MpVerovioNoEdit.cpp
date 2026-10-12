// With NO_EDIT_SUPPORT, Verovio 6.3.0 still declares EditorToolkitShared and
// an inline ParseEditorAction(json) that calls the two-argument overload. The
// editor sources are not compiled, and an optimising compiler that guesses
// the one known editor class for Toolkit::Edit() emits a call to that
// overload. No editor is ever created, so this is never reached; it only
// gives the linker the symbol.
#include "editortoolkit_shared.h"

namespace vrv {

bool EditorToolkitShared::ParseEditorAction(const std::string &, bool)
{
    return false;
}

} // namespace vrv
