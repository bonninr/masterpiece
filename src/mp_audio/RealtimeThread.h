#pragma once

// Real-time scheduling for the threads that render audio, the way each system
// offers it: MMCSS "Pro Audio" on Windows (and never slowed down to save
// power), SCHED_FIFO on Linux where the user may have it, the Mach
// time-constraint policy on macOS and iOS. Each call acts on the calling
// thread. No JUCE here: the platform headers stay out of the rest.

namespace mp::realtime {

// Whether this system has a way the program uses. Android does not let an
// app have one; its audio callback is real-time already.
bool supported();

// What it is called here, for the log.
const char* method();

// Makes the calling thread real-time, for audio work of up to blockSeconds
// each block. False where the system refused: on Linux, a user without
// real-time rights (the audio group, or limits.conf). Called again, it
// updates the thread's setting.
bool makeRealtime(double blockSeconds);

// Undoes makeRealtime() on the calling thread; nothing if it never took.
void makeNormal();

// The audio thread itself. On Linux, JUCE's ALSA thread is an ordinary
// thread: on, it becomes real-time like the render threads, unless an audio
// server (JACK) has made it real-time already; off, it goes back to what it
// was. Elsewhere the system's audio thread is real-time already and this
// does nothing.
void setAudioThread(bool on, double blockSeconds);

} // namespace mp::realtime
