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
  auto folder = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                    .getChildFile("Masterpiece")
                    .getChildFile("Linked");
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
#endif

void importDocument(const juce::URL& document, std::function<void(juce::File)> done) {
 #if JUCE_ANDROID
  // In place when the file allows it: nothing copied, no space used.
  if (const auto direct = openInPlace(document); direct.existsAsFile()) {
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
  if (const auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    return kMobile ? d->totalArea : d->userArea;
  return {0, 0, 1280, 800};
}

void fitToScreen(juce::DocumentWindow& window) {
  if (!kMobile) return;
  // A native title bar is a desktop thing; the platform draws none.
  window.setUsingNativeTitleBar(false);
  window.setTitleBarHeight(kTouchTitleBar);
  window.setResizable(false, false);
  window.setBounds(screenArea());
}

juce::DialogWindow* launchDialog(juce::DialogWindow::LaunchOptions& options) {
  if (kMobile) {
    options.useNativeTitleBar = false;
    options.resizable = false;
  }
  auto* dialog = options.launchAsync();
  if (dialog != nullptr) fitToScreen(*dialog);
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
