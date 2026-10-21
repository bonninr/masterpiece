// macOS: the file dialogs stay with the program (#492).
//
// JUCE raises an open or save panel to the modal-panel level, above every
// normal window, and the panel stayed above other programs' windows once the
// player went to one. Each panel hides when the program leaves the front and
// comes back with it, as the Recorder and Combinations windows do. One
// observer, for the application's lifetime: a panel is seen as it takes the
// keyboard.

#include "Mobile.h"

#if JUCE_MAC
 #import <AppKit/AppKit.h>

namespace mp::ui {

void keepFileDialogsWithProgram() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidBecomeKeyNotification
                                                    object:nil
                                                     queue:nil
                                                usingBlock:^(NSNotification* note) {
                                                  id window = note.object;
                                                  if ([window isKindOfClass:[NSSavePanel class]])
                                                    [(NSSavePanel*)window setHidesOnDeactivate:YES];
                                                }];
}

}  // namespace mp::ui

#endif
