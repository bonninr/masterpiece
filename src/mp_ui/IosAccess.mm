// iPhone and iPad: organ packages read where they are (#197).
//
// The system lends a document picked in the Files app through a
// security-scoped URL. JUCE's file chooser starts that access only long
// enough to make a bookmark of it, and keeps the bookmark in the juce::URL it
// returns. A package of many gigabytes is read for as long as the organ is
// loaded, from the loader thread and the sample streamer, so the access is
// started here from that bookmark and kept until the program ends. Copying the
// package into the app instead would double the space it takes.
//
// A folder is lent the same way, and everything in it with it: how a set
// split into several volumes is read.
//
// The bookmarks are kept, one per line with the path, so a package or folder
// opened before is lent again at the next start without being picked again.

#include "Mobile.h"

#if JUCE_IOS
 #import <Foundation/Foundation.h>

namespace juce {
// Defined in juce_URL.cpp: the bookmark the file chooser keeps in a URL.
void* getURLBookmark(URL&);
}

namespace mp::ui {
namespace {

// Held for the life of the program.
NSMutableArray* heldUrls() {
  static NSMutableArray* urls = [[NSMutableArray alloc] init];
  return urls;
}

juce::File heldList() {
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
      .getChildFile("Masterpiece")
      .getChildFile("held-documents.txt");
}

juce::File fileOf(NSURL* url) {
  // Each message on its own line: "([[" inside a call reads as a C++
  // attribute in Objective-C++.
  NSString* path = [url path];
  return juce::File(juce::String(juce::CharPointer_UTF8([path UTF8String])));
}

// Starts access and keeps the URL. A path inside the app's own container
// needs no grant and reports none: it is returned all the same.
juce::File hold(NSURL* url) {
  if (url == nil || ![url isFileURL]) return {};
  if ([url startAccessingSecurityScopedResource]) [heldUrls() addObject:url];
  return fileOf(url);
}

// Written while the access is held, which a bookmark needs.
void remember(NSURL* url) {
  NSError* error = nil;
  NSData* data = [url bookmarkDataWithOptions:0
               includingResourceValuesForKeys:nil
                                relativeToURL:nil
                                        error:&error];
  if (data == nil) return;
  const juce::String path = fileOf(url).getFullPathName();
  NSString* encoded = [data base64EncodedStringWithOptions:0];
  const juce::String mark(juce::CharPointer_UTF8([encoded UTF8String]));
  juce::StringArray lines, kept;
  lines.addLines(heldList().loadFileAsString());
  for (const auto& line : lines)
    if (line.isNotEmpty() && line.upToFirstOccurrenceOf("\t", false, false) != path) kept.add(line);
  kept.add(path + "\t" + mark);
  heldList().getParentDirectory().createDirectory();
  heldList().replaceWithText(kept.joinIntoString("\n") + "\n");
}

NSURL* resolve(NSData* bookmark) {
  if (bookmark == nil) return nil;
  BOOL stale = NO;
  NSError* error = nil;
  return [NSURL URLByResolvingBookmarkData:bookmark
                                   options:0
                             relativeToURL:nil
                       bookmarkDataIsStale:&stale
                                     error:&error];
}

}  // namespace

juce::File holdPickedAccess(const juce::URL& picked) {
  juce::URL copy(picked);
  NSURL* url = resolve((NSData*)juce::getURLBookmark(copy));
  if (url == nil)
    url = [NSURL URLWithString:[NSString stringWithUTF8String:picked.toString(true).toRawUTF8()]];
  const auto file = hold(url);
  if (file != juce::File()) remember(url);
  juce::Logger::writeToLog("document lent: " + file.getFullPathName());
  return file;
}

void restoreHeldAccess() {
  juce::StringArray lines, kept;
  lines.addLines(heldList().loadFileAsString());
  for (const auto& line : lines) {
    const auto mark = line.fromFirstOccurrenceOf("\t", false, false);
    if (mark.isEmpty()) continue;
    NSData* data = [[NSData alloc] initWithBase64EncodedString:[NSString stringWithUTF8String:mark.toRawUTF8()]
                                                       options:0];
    NSURL* url = resolve(data);
   #if !__has_feature(objc_arc)
    [data release];
   #endif
    // A document that has gone, or whose loan was withdrawn, is dropped.
    if (url != nil && hold(url).exists()) kept.add(line);
  }
  if (kept.size() != lines.size())
    heldList().replaceWithText(kept.joinIntoString("\n") + (kept.isEmpty() ? "" : "\n"));
}

}  // namespace mp::ui
#endif
