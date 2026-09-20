// Console artwork stored as Windows BMP.
//
// JUCE reads PNG, JPEG and GIF. Hauptwerk predates all three as a console
// format: the older sets paint their jambs, keys and drawstops in BMP, and
// asking JUCE for one returns an invalid image, which draws as black. Issue
// #24 is a set that renders correctly only once its bitmaps are converted.
//
// This reads the shapes those sets actually use: 1, 4, 8, 16, 24 and 32 bits
// per pixel, uncompressed or RLE4/RLE8, stored bottom-up or top-down, with
// the colour masks a BI_BITFIELDS header declares. 32-bit files carry an
// alpha channel only sometimes, so it is used when any pixel is not opaque
// and ignored when every one of them is zero -- a file whose alpha bytes are
// unwritten must not come out invisible.
#pragma once
#include <juce_graphics/juce_graphics.h>

namespace mp {

// The image, or an invalid image if the file is not a BMP this can read.
juce::Image loadBmpImage(const juce::File& file);

// Any image the console asks for: JUCE's own formats first, then BMP.
juce::Image loadConsoleImage(const juce::File& file);

} // namespace mp
