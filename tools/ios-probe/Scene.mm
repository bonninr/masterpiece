// The window scene's bounds and safe area, from UIKit: the figures the probe
// compares with JUCE's display.
#include <juce_gui_basics/juce_gui_basics.h>
#import <UIKit/UIKit.h>
#include <cmath>

static UIWindowScene* activeScene() {
  UIWindowScene* any = nil;
  for (UIScene* s in UIApplication.sharedApplication.connectedScenes) {
    if (![s isKindOfClass:UIWindowScene.class]) continue;
    if (s.activationState == UISceneActivationStateForegroundActive) return (UIWindowScene*)s;
    if (any == nil) any = (UIWindowScene*)s;
  }
  return any;
}

bool probeScene(juce::Rectangle<int>& whole, juce::Rectangle<int>& safe, juce::String& about) {
  UIWindowScene* scene = activeScene();
  if (scene == nil) { about = "no scene"; return false; }
  const CGRect b = scene.coordinateSpace.bounds;
  UIWindow* largest = nil;
  CGFloat area = 0;
  int windows = 0;
  for (UIWindow* w in scene.windows) {
    ++windows;
    if (w.hidden) continue;
    const CGFloat a = w.frame.size.width * w.frame.size.height;
    if (a > area) { area = a; largest = w; }
  }
  const UIEdgeInsets in = largest != nil ? largest.safeAreaInsets : UIEdgeInsetsZero;
  whole = {0, 0, (int)b.size.width, (int)b.size.height};
  safe = whole.withTrimmedLeft((int)std::ceil(in.left)).withTrimmedTop((int)std::ceil(in.top))
             .withTrimmedRight((int)std::ceil(in.right)).withTrimmedBottom((int)std::ceil(in.bottom));
  const CGRect lf = largest != nil ? largest.frame : CGRectZero;
  about << "orientation " << (int)scene.effectiveGeometry.interfaceOrientation
        << ", windows " << windows
        << ", largest " << (int)lf.origin.x << "," << (int)lf.origin.y << " "
        << (int)lf.size.width << "x" << (int)lf.size.height
        << ", insets t" << in.top << " l" << in.left << " b" << in.bottom << " r" << in.right
        << ", screen " << (int)scene.screen.bounds.size.width << "x" << (int)scene.screen.bounds.size.height;
  return b.size.width > 0 && b.size.height > 0;
}
