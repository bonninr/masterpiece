// JUCE's JNI helpers, for opening a picked document's descriptor. Defined
// before any JUCE header so juce_core includes them.
#if defined(__ANDROID__)
 #define JUCE_CORE_INCLUDE_JNI_HELPERS 1
#endif
#include "Mobile.h"

#include "../mp_archive/OrganArchive.h"

#if JUCE_ANDROID
 #include <map>
 #include <mutex>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

namespace mp::ui {

namespace {
// A finger is about 9 mm across; at a tablet's density that is near 48
// logical pixels, the size both platforms recommend for a touch target.
constexpr int kTouchTitleBar = 48;

juce::Component::SafePointer<juce::Component>& hostPointer() {
  static juce::Component::SafePointer<juce::Component> host;
  return host;
}

// JUCE's default look, kept, with what a finger needs instead of a mouse.
class TouchLook : public juce::LookAndFeel_V4 {
public:
  void getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator,
                                 int standardMenuItemHeight, int& idealWidth,
                                 int& idealHeight) override {
    juce::LookAndFeel_V4::getIdealPopupMenuItemSize(text, isSeparator, standardMenuItemHeight,
                                                    idealWidth, idealHeight);
    if (!isSeparator) {
      idealHeight = juce::jmax(idealHeight, kTouchTitleBar);
      idealWidth += 32;
    }
  }
  juce::Font getPopupMenuFont() override {
    return juce::LookAndFeel_V4::getPopupMenuFont().withHeight(20.0f);
  }
};

std::unique_ptr<TouchLook> touchLook;
}  // namespace

void installTouchLook() {
  if (!kMobile || touchLook != nullptr) return;
  touchLook = std::make_unique<TouchLook>();
  juce::LookAndFeel::setDefaultLookAndFeel(touchLook.get());
}

void removeTouchLook() {
  if (touchLook == nullptr) return;
  juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
  touchLook.reset();
}

bool isMobilePackage(const juce::String& fileName) {
  if (!kMobile) return true;
  const auto n = fileName.toLowerCase();
  return n.endsWith(".rar") || n.endsWith(".orgue");
}

juce::File importedOrgansFolder() {
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
      .getChildFile("Masterpiece")
      .getChildFile("Organs");
}

namespace {
class ImportThread : public juce::ThreadWithProgressWindow {
public:
  ImportThread(const juce::URL& document, std::function<void(juce::File)> done)
      : juce::ThreadWithProgressWindow("Copying the organ", true, true),
        document_(document), done_(std::move(done)) {}

  void run() override {
   #if JUCE_ANDROID
    const auto doc = juce::AndroidDocument::fromDocument(document_);
    const auto info = doc.getInfo();
    const auto name = juce::File::createLegalFileName(info.getName());
    const juce::int64 size = info.getSizeInBytes();
    if (name.isEmpty()) { failure_ = "This document has no name."; return; }
    // The picker cannot always filter by extension (Android knows no type
    // for .orgue), so the choice is checked here, before anything is copied.
    if (!isMobilePackage(name)) {
      failure_ = "Masterpiece opens an organ from its package: a .rar or .orgue file.";
      return;
    }
    auto folder = importedOrgansFolder();
    folder.createDirectory();
    const auto target = folder.getChildFile(name);
    if (target.existsAsFile() && size > 0 && target.getSize() == size) {
      result_ = target;
      return;
    }
    auto in = doc.createInputStream();
    if (in == nullptr) { failure_ = "The document could not be read."; return; }
    // Written beside the target and renamed at the end, so a cancelled or
    // failed copy never looks like a finished one.
    const auto partial = folder.getChildFile(name + ".part");
    partial.deleteFile();
    {
      juce::FileOutputStream out(partial);
      if (!out.openedOk()) { failure_ = "There is no room to copy it."; return; }
      juce::HeapBlock<char> buffer(1 << 20);
      juce::int64 copied = 0;
      for (;;) {
        if (threadShouldExit()) { out.flush(); partial.deleteFile(); return; }
        const int n = in->read(buffer, 1 << 20);
        if (n <= 0) break;
        if (!out.write(buffer, (size_t)n)) {
          failure_ = "The copy stopped: the device may be full.";
          break;
        }
        copied += n;
        if (size > 0) setProgress((double)copied / (double)size);
        setStatusMessage(juce::File::descriptionOfSizeInBytes(copied) +
                         (size > 0 ? " of " + juce::File::descriptionOfSizeInBytes(size) : juce::String()));
      }
    }
    if (failure_.isNotEmpty()) { partial.deleteFile(); return; }
    target.deleteFile();
    if (partial.moveFileTo(target)) result_ = target;
    else failure_ = "The copy could not be finished.";
   #endif
  }

  void threadComplete(bool cancelled) override {
    if (!cancelled && failure_.isNotEmpty())
      juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                             "Organ not copied", failure_);
    if (done_) done_(cancelled ? juce::File() : result_);
    delete this;
  }

private:
  juce::URL document_;
  std::function<void(juce::File)> done_;
  juce::File result_;
  juce::String failure_;
};
}  // namespace

#if JUCE_ANDROID
namespace {
// A file descriptor for a picked document, from the system's own
// ContentResolver.openFileDescriptor; -1 if the provider will not give one.
int openDocumentDescriptor(const juce::URL& document) {
  auto* env = juce::getEnv();
  if (env == nullptr) return -1;
  const auto ctx = juce::getAppContext();
  int fd = -1;
  jclass ctxClass = env->GetObjectClass(ctx.get());
  jobject resolver = env->CallObjectMethod(
      ctx.get(), env->GetMethodID(ctxClass, "getContentResolver", "()Landroid/content/ContentResolver;"));
  jclass uriClass = env->FindClass("android/net/Uri");
  jstring text = env->NewStringUTF(document.toString(true).toRawUTF8());
  jobject uri = env->CallStaticObjectMethod(
      uriClass, env->GetStaticMethodID(uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"), text);
  jclass resolverClass = env->GetObjectClass(resolver);
  jstring mode = env->NewStringUTF("r");
  jobject pfd = env->CallObjectMethod(
      resolver,
      env->GetMethodID(resolverClass, "openFileDescriptor",
                       "(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;"),
      uri, mode);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  } else if (pfd != nullptr) {
    jclass pfdClass = env->GetObjectClass(pfd);
    // Detached: the descriptor is ours to keep open for as long as the organ
    // is read, not the Java object's to close when it is collected.
    fd = env->CallIntMethod(pfd, env->GetMethodID(pfdClass, "detachFd", "()I"));
    if (env->ExceptionCheck()) { env->ExceptionClear(); fd = -1; }
    env->DeleteLocalRef(pfdClass);
    env->DeleteLocalRef(pfd);
  }
  for (jobject o : {(jobject)ctxClass, resolver, (jobject)uriClass, (jobject)text, uri,
                    (jobject)resolverClass, (jobject)mode})
    if (o != nullptr) env->DeleteLocalRef(o);
  return fd;
}

// Package name -> the descriptor its link points through. Kept open for the
// life of the app, so the link stays valid to look at (its size, that it
// exists); replaced when the same package is picked again.
std::map<juce::String, int>& openDescriptors() {
  static std::map<juce::String, int> fds;
  return fds;
}

// Link path -> the document behind it. The link cannot be opened by name
// (Android refuses to reopen a shared file through /proc), so the archive
// reader asks the system for a fresh descriptor every time it opens one.
std::map<std::string, juce::URL>& linkedDocuments() {
  static std::map<std::string, juce::URL> docs;
  return docs;
}
std::mutex& linkedDocumentsLock() {
  static std::mutex m;
  return m;
}

// Keeps read access to a picked document across restarts. Read only: that is
// all a package needs, and the one grant the picker always makes persistable.
void keepReadAccess(const juce::URL& document) {
  auto* env = juce::getEnv();
  if (env == nullptr) return;
  const auto ctx = juce::getAppContext();
  jclass ctxClass = env->GetObjectClass(ctx.get());
  jobject resolver = env->CallObjectMethod(
      ctx.get(), env->GetMethodID(ctxClass, "getContentResolver", "()Landroid/content/ContentResolver;"));
  jclass uriClass = env->FindClass("android/net/Uri");
  jstring text = env->NewStringUTF(document.toString(true).toRawUTF8());
  jobject uri = env->CallStaticObjectMethod(
      uriClass, env->GetStaticMethodID(uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"), text);
  jclass resolverClass = env->GetObjectClass(resolver);
  constexpr jint kReadGrant = 1;  // Intent.FLAG_GRANT_READ_URI_PERMISSION
  env->CallVoidMethod(resolver,
                      env->GetMethodID(resolverClass, "takePersistableUriPermission",
                                       "(Landroid/net/Uri;I)V"),
                      uri, kReadGrant);
  if (env->ExceptionCheck()) env->ExceptionClear();
  for (jobject o : {(jobject)ctxClass, resolver, (jobject)uriClass, (jobject)text, uri,
                    (jobject)resolverClass})
    if (o != nullptr) env->DeleteLocalRef(o);
}

int openLinkedDocument(const std::string& path) {
  juce::URL url;
  {
    std::lock_guard<std::mutex> lock(linkedDocumentsLock());
    const auto it = linkedDocuments().find(path);
    if (it == linkedDocuments().end()) return -1;
    url = it->second;
  }
  return openDocumentDescriptor(url);
}
}  // namespace

juce::File linkedFolder() {
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
      .getChildFile("Masterpiece")
      .getChildFile("Linked");
}

// The documents read in place, one per line, so their links can be made again
// when the app starts: the descriptors behind them close with the process.
juce::File linkedList() { return linkedFolder().getChildFile("documents.txt"); }

// The document read where it is, through a link named like the package (the
// loader goes by the extension) that points at a descriptor the system gave
// us. Only for a file on the device: a provider that streams (a cloud drive)
// gives a descriptor that cannot seek, and the archive reader must seek.
static juce::File openInPlace(const juce::URL& document) {
  const auto name = juce::File::createLegalFileName(
      juce::AndroidDocument::fromDocument(document).getInfo().getName());
  if (name.isEmpty() || !isMobilePackage(name)) return {};
  const int fd = openDocumentDescriptor(document);
  if (fd < 0) return {};
  struct stat st {};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || lseek(fd, 0, SEEK_END) < 0 ||
      lseek(fd, 0, SEEK_SET) != 0) {
    close(fd);
    return {};
  }
  auto folder = linkedFolder();
  folder.createDirectory();
  const auto link = folder.getChildFile(name);
  auto& fds = openDescriptors();
  if (const auto it = fds.find(name); it != fds.end()) {
    close(it->second);
    fds.erase(it);
  }
  unlink(link.getFullPathName().toRawUTF8());
  const auto target = "/proc/self/fd/" + juce::String(fd);
  if (symlink(target.toRawUTF8(), link.getFullPathName().toRawUTF8()) != 0 || !link.existsAsFile()) {
    close(fd);
    return {};
  }
  fds[name] = fd;
  {
    std::lock_guard<std::mutex> lock(linkedDocumentsLock());
    linkedDocuments()[link.getFullPathName().toStdString()] = document;
  }
  setArchiveOpenHook(&openLinkedDocument);
  juce::Logger::writeToLog("organ read in place: " + name);
  return link;
}

// Picked now: kept for the next start too.
static juce::File openInPlaceAndRemember(const juce::URL& document) {
  const auto link = openInPlace(document);
  if (!link.existsAsFile()) return link;
  keepReadAccess(document);
  juce::StringArray lines;
  lines.addLines(linkedList().loadFileAsString());
  lines.removeString(document.toString(true));
  lines.removeEmptyStrings();
  lines.add(document.toString(true));
  linkedList().replaceWithText(lines.joinIntoString("\n") + "\n");
  return link;
}
#endif

void restoreLinkedDocuments() {
 #if JUCE_IOS
  restoreHeldAccess();
 #endif
 #if JUCE_ANDROID
  juce::StringArray lines, kept;
  lines.addLines(linkedList().loadFileAsString());
  for (const auto& line : lines)
    if (line.isNotEmpty() && openInPlace(juce::URL(line)).existsAsFile()) kept.add(line);
  // A document that has gone (deleted, or its access withdrawn) is dropped.
  if (kept.size() != lines.size())
    linkedList().replaceWithText(kept.joinIntoString("\n") + (kept.isEmpty() ? "" : "\n"));
 #endif
}

void importDocument(const juce::URL& document, std::function<void(juce::File)> done) {
 #if JUCE_ANDROID
  // In place when the file allows it: nothing copied, no space used.
  if (const auto direct = openInPlaceAndRemember(document); direct.existsAsFile()) {
    if (done) done(direct);
    return;
  }
  juce::Logger::writeToLog("organ copied: its document cannot be read in place");
 #endif
  // Deletes itself when the copy ends.
  (new ImportThread(document, std::move(done)))->launchThread();
}

juce::Rectangle<int> screenArea() {
  // The whole display: on a phone or tablet the console runs in kiosk mode,
  // with the system's bars hidden (see the app's main window). Each panel is
  // a window of its own there, so leaving room for the bars would mean
  // finding them for every one; hiding them gives every panel the screen.
  //
  // An iPad keeps what kiosk mode cannot hide: since iPadOS 26 every app runs
  // in a window, with the window's controls drawn over its top-left corner,
  // and the screen's rounded corners and home indicator cut into the edges.
  // Those are the display's safe-area insets, and drawing under them put the
  // menu where no tap reaches it (#197).
  //
  // On iOS the scene's own figures: JUCE's display stays upright on a turned
  // iPad (see IosAccess.mm).
 #if JUCE_IOS
  juce::Rectangle<int> whole, safe;
  if (sceneBounds(whole, safe)) return safe;
 #endif
  if (const auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) {
   #if JUCE_IOS
    return d->safeAreaInsets.subtractedFrom(d->totalArea);
   #else
    return kMobile ? d->totalArea : d->userArea;
   #endif
  }
  return {0, 0, 1280, 800};
}

juce::Rectangle<int> screenBounds() {
 #if JUCE_IOS
  juce::Rectangle<int> whole, safe;
  if (sceneBounds(whole, safe)) return whole;
 #endif
  if (const auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    return d->totalArea;
  return {0, 0, 1280, 800};
}

void fitToScreen(juce::DocumentWindow& window) {
  if (!kMobile) return;
  // A native title bar is a desktop thing; the platform draws none.
  window.setUsingNativeTitleBar(false);
  window.setTitleBarHeight(kTouchTitleBar);
  window.setResizable(false, false);
  // Inside the main window it fills the layer it is in (setPanelHost).
  if (auto* parent = window.getParentComponent()) window.setBounds(parent->getLocalBounds());
  else window.setBounds(screenArea());
}

void setPanelHost(juce::Component* host) { hostPointer() = host; }
juce::Component* panelHost() { return hostPointer().getComponent(); }

namespace {
// Into the main window's layer, from the desktop or nowhere: a component
// added as a child leaves the desktop.
void hostIn(juce::Component& host, juce::DocumentWindow& window) {
  if (window.getParentComponent() != &host) host.addChildComponent(window);
  fitToScreen(window);
}
}  // namespace

void showFloating(juce::DocumentWindow& window, bool show, bool onTop) {
  if (auto* host = panelHost(); kMobile && host != nullptr) {
    juce::ignoreUnused(onTop);
    if (!show) {
      window.setVisible(false);
      host->removeChildComponent(&window);
      return;
    }
    hostIn(*host, window);
    window.setVisible(true);
    window.toFront(true);
    return;
  }
 #if JUCE_IOS
  // On iOS a hidden JUCE window hides its view and keeps its UIKit window,
  // which still covers the screen and takes every touch. The Combinations
  // window, built hidden at startup, became the key window once the first
  // dialog closed, and no tap reached the console again (#197). A hidden
  // window leaves the screen; shown, it comes back.
  juce::ignoreUnused(onTop);
  if (!show) {
    window.setVisible(false);
    window.removeFromDesktop();
    return;
  }
  if (!window.isOnDesktop()) window.addToDesktop();
  window.setVisible(true);
  window.toFront(true);
  return;
 #endif
  if (!kLiveOnTop) {
    window.setVisible(false);
    window.removeFromDesktop();
    if (!show) return;
    window.setAlwaysOnTop(onTop);  // off the desktop: a flag, no window remade
    window.addToDesktop();
  } else {
    window.setAlwaysOnTop(onTop);
  }
  window.setVisible(show);
  if (show) window.toFront(true);
}

namespace {
// The size each kind of dialog was last left at (#153), one "kind=w h" line
// each, beside the main window's own place.
juce::File dialogSizesFile() {
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
      .getChildFile("Masterpiece")
      .getChildFile("dialogs.txt");
}

// A kind of dialog, from its title. The MIDI dialogs are titled by what they
// set up ("Swell - MIDI"), and share one size.
juce::String dialogKind(const juce::String& title) {
  return title.contains(" - ") ? title.fromLastOccurrenceOf(" - ", false, false) : title;
}

juce::Rectangle<int> savedDialogSize(const juce::String& kind) {
  juce::StringArray lines;
  lines.addLines(dialogSizesFile().loadFileAsString());
  for (const auto& line : lines)
    if (line.upToFirstOccurrenceOf("=", false, false) == kind) {
      const auto wh = juce::StringArray::fromTokens(line.fromFirstOccurrenceOf("=", false, false), " ", "");
      if (wh.size() == 2) return {wh[0].getIntValue(), wh[1].getIntValue()};
    }
  return {};
}

void saveDialogSize(const juce::String& kind, int w, int h) {
  juce::StringArray lines;
  lines.addLines(dialogSizesFile().loadFileAsString());
  lines.removeEmptyStrings();
  for (int i = lines.size(); --i >= 0;)
    if (lines[i].upToFirstOccurrenceOf("=", false, false) == kind) lines.remove(i);
  lines.add(kind + "=" + juce::String(w) + " " + juce::String(h));
  dialogSizesFile().getParentDirectory().createDirectory();
  dialogSizesFile().replaceWithText(lines.joinIntoString("\n") + "\n");
}

// Writes the dialog's size down as it closes. Owned by nobody but itself: it
// goes with the dialog.
class SizeKeeper : public juce::ComponentListener {
public:
  explicit SizeKeeper(juce::String kind) : kind_(std::move(kind)) {}
  void componentBeingDeleted(juce::Component& c) override {
    if (c.getWidth() > 0 && c.getHeight() > 0) saveDialogSize(kind_, c.getWidth(), c.getHeight());
    c.removeComponentListener(this);
    delete this;
  }

private:
  juce::String kind_;
};
}  // namespace

juce::DialogWindow* launchDialog(juce::DialogWindow::LaunchOptions& options) {
  if (kMobile) {
    options.useNativeTitleBar = false;
    options.resizable = false;
  }
  auto* dialog = options.launchAsync();
  if (dialog == nullptr) return dialog;
  if (auto* host = panelHost(); kMobile && host != nullptr) {
    hostIn(*host, *dialog);
    dialog->setVisible(true);
    dialog->toFront(true);
  }
  fitToScreen(*dialog);
  // A dialog the player can resize opens at the size they last left it,
  // as far as the screen it opens on allows.
  if (options.resizable && !kMobile) {
    const auto kind = dialogKind(options.dialogTitle);
    const auto saved = savedDialogSize(kind);
    if (!saved.isEmpty()) {
      const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(dialog->getScreenBounds());
      const auto area = display != nullptr ? display->userBounds.toNearestInt() : juce::Rectangle<int>(saved.getWidth(), saved.getHeight());
      dialog->centreAroundComponent(options.componentToCentreAround,
                                    juce::jmin(saved.getWidth(), area.getWidth()),
                                    juce::jmin(saved.getHeight(), area.getHeight()));
    }
    dialog->addComponentListener(new SizeKeeper(kind));
  }
  return dialog;
}

bool closeFrontWindow(juce::Component* mainWindow) {
  auto& desktop = juce::Desktop::getInstance();
  // The desktop lists its windows back to front.
  for (int i = desktop.getNumComponents(); --i >= 0;) {
    auto* window = dynamic_cast<juce::DocumentWindow*>(desktop.getComponent(i));
    if (window == nullptr || window == mainWindow || !window->isVisible()) continue;
    // A window the player may not leave (the load in progress): back does
    // nothing, rather than closing it or the app behind it.
    if (window->getProperties()[kStaysOpen]) return true;
    window->closeButtonPressed();
    return true;
  }
  return false;
}

}  // namespace mp::ui
