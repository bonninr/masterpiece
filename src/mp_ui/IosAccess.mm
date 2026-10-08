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
 #import <UIKit/UIKit.h>
 #include <cmath>

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
  // Assigned: "String mark(CharPointer_UTF8([...]))" parses as a function
  // declaration with an array parameter.
  const juce::String mark = juce::String::fromUTF8([encoded UTF8String]);
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

// ------------------------------------------------------------ the scene

// The screen as the app has it now. JUCE reads UIScreen's bounds and the
// insets of its own first window; under iPadOS's scenes the first stays
// upright on a turned iPad, and the second follows a window that was sized
// from it, so the console filled the upright width of a landscape screen with
// the clock over its first buttons. The scene knows its own size, and a
// window covering all of it knows the safe area.
namespace {

UIWindowScene* activeScene() {
  UIWindowScene* any = nil;
  for (UIScene* s in UIApplication.sharedApplication.connectedScenes) {
    if (![s isKindOfClass:UIWindowScene.class]) continue;
    if (s.activationState == UISceneActivationStateForegroundActive) return (UIWindowScene*)s;
    if (any == nil) any = (UIWindowScene*)s;
  }
  return any;
}

// The safe area of the scene's largest visible window, which is the app's
// main window: once that covers the scene (the main window is sized to it),
// its insets are the scene's. A window of our own for this covered the
// console and left the screen black.
UIEdgeInsets sceneInsets(UIWindowScene* scene) {
  UIWindow* largest = nil;
  CGFloat area = 0;
  for (UIWindow* w in scene.windows) {
    if (w.hidden) continue;
    const CGFloat a = w.frame.size.width * w.frame.size.height;
    if (a > area) { area = a; largest = w; }
  }
  return largest != nil ? largest.safeAreaInsets : UIEdgeInsetsZero;
}

}  // namespace

// Every window of every scene, front to back as UIKit stacks them, with what
// decides whether it takes a touch: for finding what lies over the console
// when taps reach nothing in it (#197).
juce::String describeWindows() {
  juce::String out;
  for (UIScene* s in UIApplication.sharedApplication.connectedScenes) {
    if (![s isKindOfClass:UIWindowScene.class]) continue;
    UIWindowScene* scene = (UIWindowScene*)s;
    out << "scene state " << (int)scene.activationState << ":";
    for (UIWindow* w in scene.windows) {
      const CGRect f = w.frame;
      out << "\n  " << juce::String::fromUTF8(NSStringFromClass(w.class).UTF8String)
          << " level " << (double)w.windowLevel << " frame " << (int)f.origin.x << "," << (int)f.origin.y
          << " " << (int)f.size.width << "x" << (int)f.size.height
          << (w.hidden ? " hidden" : "") << (w.isKeyWindow ? " key" : "")
          << (w.userInteractionEnabled ? "" : " no-touch") << " alpha " << (double)w.alpha
          << " root " << (w.rootViewController != nil
                              ? juce::String::fromUTF8(NSStringFromClass(w.rootViewController.class).UTF8String)
                              : juce::String("none"));
      if (UIViewController* shown = w.rootViewController.presentedViewController)
        out << " presenting " << juce::String::fromUTF8(NSStringFromClass(shown.class).UTF8String);
    }
  }
  return out;
}

bool sceneBounds(juce::Rectangle<int>& whole, juce::Rectangle<int>& safe) {
  UIWindowScene* scene = activeScene();
  if (scene == nil) return false;
  const CGRect b = scene.coordinateSpace.bounds;
  if (b.size.width <= 0 || b.size.height <= 0) return false;
  const UIEdgeInsets in = sceneInsets(scene);
  whole = {0, 0, (int)b.size.width, (int)b.size.height};
  safe = whole.withTrimmedLeft((int)std::ceil(in.left))
             .withTrimmedTop((int)std::ceil(in.top))
             .withTrimmedRight((int)std::ceil(in.right))
             .withTrimmedBottom((int)std::ceil(in.bottom));
  return true;
}

}  // namespace mp::ui
#endif
