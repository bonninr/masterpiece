#pragma once

namespace mp::app {

// The exercise a profile-guided build is trained on (--train-profile): an organ
// written to the temporary folder, loaded and played offline in each sample
// storage, with no audio device and no window. Returns the exit code: 0 when
// every pass loaded and sounded.
int trainProfile();

} // namespace mp::app
