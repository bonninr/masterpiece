#include "InstallWindow.h"

#include "../mp_audio/DataFolder.h"
#include "Mobile.h"

#include <algorithm>

namespace mp::ui {

namespace {
const juce::Colour kBg(0xff15171c), kRowBg(0xff20232a), kRowAlt(0xff1b1e24), kText(0xffe6e8ec),
    kDim(0xff8c93a0), kWarn(0xffe0a040), kGood(0xff7cc47f);

enum Column { kAction = 1, kType, kName, kVersion, kInstalled, kSize };

juce::String sizeText(int64_t bytes) {
  if (bytes >= 1000000000) return juce::String(static_cast<double>(bytes) / 1e9, 1) + " GB";
  // Whole numbers: juce::String(double, 0) prints every digit.
  if (bytes >= 1000000) return juce::String((bytes + 500000) / 1000000) + " MB";
  return juce::String((bytes + 500) / 1000) + " kB";
}

juce::String utf8(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

juce::String typeText(InstallComponent::Kind k) {
  switch (k) {
    case InstallComponent::Kind::Organ: return TRANS("Organ");
    case InstallComponent::Kind::Data: return TRANS("Package");
    case InstallComponent::Kind::Other: break;
  }
  return TRANS("Files");
}

juce::String nameText(const InstallComponent& c) {
  if (c.kind == InstallComponent::Kind::Data) {
    const auto name = utf8(c.name);
    return name == utf8(c.packageId) ? TRANS("Package %1").replace("%1", name) : name + " (" + utf8(c.packageId) + ")";
  }
  if (c.kind == InstallComponent::Kind::Other && c.id.empty()) return TRANS("Files at the top of the package");
  return utf8(c.name);
}
}  // namespace

// The action of one row, chosen as the installer's "Selected action" is.
class InstallPanel::ActionBox : public juce::ComboBox {
public:
  explicit ActionBox(InstallPanel& owner) : owner_(owner) {
    onChange = [this] { owner_.setAction(row_, getSelectedId() == 1); };
  }
  void show(int row) {
    row_ = row;
    const Row& r = owner_.rows_[static_cast<size_t>(row)];
    clear(juce::dontSendNotification);
    addItem(owner_.actionText(r, true), 1);
    addItem(owner_.actionText(r, false), 2);
    setSelectedId(r.act ? 1 : 2, juce::dontSendNotification);
    setEnabled(!owner_.busy_ && !r.inUse);
    setTitle(nameText(r.offered));
  }

private:
  InstallPanel& owner_;
  int row_ = 0;
};

InstallPanel::InstallPanel(MasterpieceProcessor& p, Mode mode, juce::File package, Open open)
    : proc_(p), mode_(mode), package_(std::move(package)), open_(std::move(open)) {
  for (auto* l : {&heading_, &folderLabel_, &summary_, &status_}) {
    addAndMakeVisible(*l);
    l->setColour(juce::Label::textColourId, kDim);
  }
  heading_.setColour(juce::Label::textColourId, kText);
  heading_.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
  addAndMakeVisible(folderButton_);
  folderButton_.setTooltip(TRANS("The folder installed organs are kept in, on any drive"));
  folderButton_.onClick = [this] { chooseFolder(); };

  addAndMakeVisible(table_);
  table_.setModel(this);
  table_.setRowHeight(kMobile ? 40 : 30);
  table_.setColour(juce::ListBox::backgroundColourId, kBg);
  table_.setColour(juce::ListBox::outlineColourId, juce::Colour(0xff3a404c));
  table_.setOutlineThickness(1);
  auto& h = table_.getHeader();
  const int flags = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::resizable;
  h.addColumn(TRANS("Action"), kAction, 190, 120, 300, flags);
  h.addColumn(TRANS("Type"), kType, 80, 60, 140, flags);
  h.addColumn(TRANS("Name"), kName, 250, 120, 600, flags);
  h.addColumn(mode_ == Mode::Install ? TRANS("In the package") : TRANS("Version"), kVersion, 100, 60, 200, flags);
  h.addColumn(mode_ == Mode::Install ? TRANS("Installed") : TRANS("Needed by"), kInstalled, 150, 80, 400, flags);
  h.addColumn(TRANS("Size"), kSize, 80, 60, 140, flags);
  h.setStretchToFitActive(true);

  addChildComponent(bar_);
  bar_.setColour(juce::ProgressBar::foregroundColourId, juce::Colour(0xff4a7ab8));
  addAndMakeVisible(go_);
  go_.setButtonText(mode_ == Mode::Install ? TRANS("Install") : TRANS("Uninstall"));
  go_.onClick = [this] { start(); };
  addChildComponent(cancel_button_);
  cancel_button_.onClick = [this] { cancel_->store(true); };
  addChildComponent(openButton_);
  openButton_.onClick = [this] {
    if (justInstalled_.isEmpty()) return;
    const juce::File organ = justInstalled_[0];
    auto open = open_;
    juce::Component::SafePointer<juce::DialogWindow> dw(findParentComponentOfClass<juce::DialogWindow>());
    juce::MessageManager::callAsync([dw, open, organ] {
      if (dw != nullptr) dw->closeButtonPressed();
      if (open) open(organ);
    });
  };
  addAndMakeVisible(close_);
  close_.onClick = [this] {
    juce::Component::SafePointer<juce::DialogWindow> dw(findParentComponentOfClass<juce::DialogWindow>());
    juce::MessageManager::callAsync([dw] {
      if (dw != nullptr) dw->closeButtonPressed();
    });
  };

  setSize(kMobile ? 640 : 880, 540);
  startTimerHz(10);
  if (mode_ == Mode::Install) {
    heading_.setText(package_.getFileName(), juce::dontSendNotification);
    analyse();
  } else {
    heading_.setText(TRANS("Organs installed in the organs folder"), juce::dontSendNotification);
    scanInstalled();
  }
  updateSummary();
}

InstallPanel::~InstallPanel() {
  alive_->store(false);
  table_.setModel(nullptr);
}

// ---------------------------------------------------------------- reading

void InstallPanel::analyse() {
  setBusy(true, TRANS("Reading the package..."));
  auto alive = alive_;
  auto cancel = cancel_;
  auto fraction = fraction_;
  const std::string path = package_.getFullPathName().toStdString();
  const juce::File data = mp::dataFolder();
  // Taken here, on the message thread: the worker reads no settings.
  const auto packagedRoots = proc_.packagedRoots();
  const std::string organsRoot = root().getFullPathName().toStdString();
  std::vector<std::string> others;
  for (const auto& lib : proc_.sampleLibraries())
    if (lib != root()) others.push_back(lib.getFullPathName().toStdString());
  juce::Component::SafePointer<InstallPanel> safe(this);
  juce::Thread::launch([=] {
    auto archive = std::make_shared<OrganArchive>();
    std::string error;
    juce::File small;
    bool owned = false;
    bool ok = archive->discover(path, error) && archive->inspect(error);
    if (ok) {
      // A package opened before has its list and its definitions already
      // unpacked: a solid archive is not walked a second time.
      juce::File packaged;
      bool known = false;
      for (const auto& packagedRoot : packagedRoots) {
        packaged = packagedRoot.getChildFile(juce::String(archive->identity()));
        known = !readArchiveMarker(packaged.getFullPathName().toStdString()).empty() &&
                archive->loadIndexFor(packaged.getChildFile("archive-index.txt").getFullPathName().toStdString());
        if (known) break;
      }
      if (known) {
        small = packaged;
      } else {
        small = data.getChildFile("Installing").getChildFile(juce::String(archive->identity()));
        small.deleteRecursively();
        small.createDirectory();
        owned = true;
        ok = archive->index(
            error,
            [alive, cancel, fraction](double f) {
              fraction->store(f);
              return alive->load() && !cancel->load();
            },
            small.getFullPathName().toStdString());
      }
    }
    auto components = std::make_shared<std::vector<InstallComponent>>();
    if (ok) *components = packageComponents(*archive, small.getFullPathName().toStdString());
    auto installed = std::make_shared<std::vector<InstallComponent>>(installedComponents(organsRoot));
    auto elsewhere = std::make_shared<std::vector<std::pair<std::string, InstallComponent>>>();
    for (const auto& lib : others)
      for (auto& c : installedComponents(lib)) elsewhere->emplace_back(lib, std::move(c));
    if (!alive->load()) {
      if (owned) small.deleteRecursively();
      return;
    }
    juce::MessageManager::callAsync([=] {
      if (safe == nullptr) {
        if (owned) small.deleteRecursively();
        return;
      }
      safe->setBusy(false);
      safe->archive_ = archive;
      safe->smallFiles_ = small;
      safe->smallFilesOwned_ = owned;
      if (!ok) {
        safe->status_.setText(error == "cancelled" ? TRANS("Cancelled.")
                                                   : TRANS("This package cannot be installed: %1").replace("%1", utf8(error)),
                              juce::dontSendNotification);
        safe->go_.setEnabled(false);
        return;
      }
      safe->rows_.clear();
      for (const auto& c : *components) {
        Row r;
        r.offered = c;
        for (const auto& i : *installed)
          if (i.id == c.id) r.had = i;
        if (!r.had)
          for (const auto& [lib, i] : *elsewhere)
            if (i.id == c.id) {
              r.elsewhere = utf8(lib);
              r.state = installStateOf(c, &i);
              break;
            }
        if (r.had) r.state = installStateOf(c, &*r.had);
        // Installed elsewhere already: the organ opens from there, so it is
        // left alone unless the player asks.
        r.act = r.elsewhere.isEmpty() && installsByDefault(r.state);
        safe->rows_.push_back(std::move(r));
      }
      safe->showRows();
      safe->status_.setText(safe->rows_.empty() ? TRANS("This package holds no organ to install.") : juce::String(),
                            juce::dontSendNotification);
    });
  });
}

void InstallPanel::scanInstalled() {
  rows_.clear();
  const auto installed = installedComponents(root().getFullPathName().toStdString());
  // The organ on the console, and the packages it reads, stay while it plays.
  const juce::File loaded = proc_.loadedOrganFile();
  std::vector<std::string> loadedNeeds;
  for (const auto& c : installed)
    if (c.kind == InstallComponent::Kind::Organ && root().getChildFile(utf8(c.id)) == loaded) loadedNeeds = c.needs;
  for (const auto& c : installed) {
    Row r;
    r.offered = c;
    if (c.kind == InstallComponent::Kind::Data) {
      for (const auto& name : organsNeeding(installed, c.packageId)) r.neededBy.add(utf8(name));
      r.inUse = std::find(loadedNeeds.begin(), loadedNeeds.end(), c.packageId) != loadedNeeds.end();
    } else if (c.kind == InstallComponent::Kind::Organ) {
      const juce::File f = root().getChildFile(utf8(c.id));
      r.inUse = f == loaded || loaded.isAChildOf(f);
    }
    rows_.push_back(std::move(r));
  }
  showRows();
  status_.setText(rows_.empty() ? TRANS("No organ is installed in this folder yet.") : juce::String(),
                  juce::dontSendNotification);
}

void InstallPanel::showRows() {
  table_.updateContent();
  table_.repaint();
  updateSummary();
}

// ---------------------------------------------------------------- choices

juce::String InstallPanel::actionText(const Row& r, bool act) const {
  if (mode_ == Mode::Uninstall) return act ? TRANS("Uninstall") : TRANS("Keep");
  if (!act) return r.had || r.elsewhere.isNotEmpty() ? TRANS("Leave as it is") : TRANS("Skip");
  if (!r.had) return TRANS("Install");
  switch (r.state) {
    case InstallState::Newer: return TRANS("Upgrade");
    case InstallState::Same: return TRANS("Reinstall");
    case InstallState::Older: return TRANS("Install the older version");
    case InstallState::New: break;
  }
  return TRANS("Install");
}

juce::String InstallPanel::stateText(const Row& r) const {
  if (mode_ == Mode::Uninstall) {
    if (r.inUse) return TRANS("On the console now");
    return r.neededBy.joinIntoString(", ");
  }
  if (r.had) return r.had->version.empty() ? TRANS("Yes") : utf8(r.had->version);
  if (r.elsewhere.isNotEmpty()) return TRANS("In %1").replace("%1", r.elsewhere);
  return TRANS("No");
}

void InstallPanel::setAction(int row, bool act) {
  if (row < 0 || row >= static_cast<int>(rows_.size())) return;
  Row& r = rows_[static_cast<size_t>(row)];
  r.act = act;
  // An organ removed takes the packages no other organ kept still needs; one
  // kept again keeps them.
  if (mode_ == Mode::Uninstall && r.offered.kind == InstallComponent::Kind::Organ) {
    for (auto& pkg : rows_) {
      if (pkg.offered.kind != InstallComponent::Kind::Data || pkg.inUse) continue;
      const auto& needs = r.offered.needs;
      if (std::find(needs.begin(), needs.end(), pkg.offered.packageId) == needs.end()) continue;
      if (!act) {
        pkg.act = false;
        continue;
      }
      bool keptElsewhere = false;
      for (const auto& other : rows_)
        if (&other != &r && other.offered.kind == InstallComponent::Kind::Organ && !other.act &&
            std::find(other.offered.needs.begin(), other.offered.needs.end(), pkg.offered.packageId) !=
                other.offered.needs.end())
          keptElsewhere = true;
      if (!keptElsewhere) pkg.act = true;
    }
  }
  table_.updateContent();
  table_.repaint();
  updateSummary();
}

void InstallPanel::updateSummary() {
  {
    const int64_t room = freeBytesAt(root().getFullPathName().toStdString());
    folderLabel_.setText(room >= 0 ? TRANS("Organs folder: %1 (%2 free)")
                                         .replace("%1", root().getFullPathName())
                                         .replace("%2", sizeText(room))
                                   : TRANS("Organs folder: %1").replace("%1", root().getFullPathName()),
                         juce::dontSendNotification);
  }
  int count = 0;
  int64_t bytes = 0;
  juce::StringArray broken;
  for (const auto& r : rows_) {
    if (!r.act) continue;
    ++count;
    bytes += r.offered.bytes;
    if (mode_ == Mode::Uninstall && r.offered.kind == InstallComponent::Kind::Data)
      for (const auto& organ : r.neededBy) {
        bool kept = false;
        for (const auto& o : rows_)
          if (o.offered.kind == InstallComponent::Kind::Organ && utf8(o.offered.name) == organ && !o.act) kept = true;
        if (kept) broken.addIfNotAlreadyThere(organ);
      }
  }
  juce::String text;
  // A drive without room for what is ticked takes no install: the player is
  // sent to Change... for a folder on another drive, before anything is
  // written (#1245).
  bool room = true;
  if (mode_ == Mode::Install) {
    const int64_t free = freeBytesAt(root().getFullPathName().toStdString());
    room = free < 0 || free >= bytes;
    if (room) {
      text = TRANS("Installs %1 component(s), %2").replace("%1", juce::String(count)).replace("%2", sizeText(bytes));
      if (free >= 0) text << TRANS(", with %1 free on that drive").replace("%1", sizeText(free));
    } else {
      text = TRANS("Installs %1 component(s), %2, but that drive has %3 free: Change... picks a folder on another drive")
                 .replace("%1", juce::String(count))
                 .replace("%2", sizeText(bytes))
                 .replace("%3", sizeText(free));
    }
    summary_.setColour(juce::Label::textColourId, room ? kDim : kWarn);
  } else if (!rows_.empty()) {
    text = TRANS("Removes %1 component(s), %2").replace("%1", juce::String(count)).replace("%2", sizeText(bytes));
    if (!broken.isEmpty())
      text << ". " << TRANS("These organs need what is removed: %1").replace("%1", broken.joinIntoString(", "));
    summary_.setColour(juce::Label::textColourId, broken.isEmpty() ? kDim : kWarn);
  }
  summary_.setText(text, juce::dontSendNotification);
  go_.setEnabled(!busy_ && count > 0 && (mode_ == Mode::Uninstall || (archive_ != nullptr && room)));
}

void InstallPanel::chooseFolder() {
  chooser_ = std::make_unique<juce::FileChooser>(TRANS("Choose the organs folder"), root());
  juce::Component::SafePointer<InstallPanel> safe(this);
  chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                        [safe](const juce::FileChooser& fc) {
                          const auto dir = fc.getResult();
                          if (safe == nullptr || dir == juce::File()) return;
                          safe->proc_.setOrgansFolder(dir);
                          if (safe->mode_ == Mode::Uninstall) {
                            safe->scanInstalled();
                            return;
                          }
                          // What is installed there decides each row anew.
                          const auto installed = installedComponents(dir.getFullPathName().toStdString());
                          for (auto& r : safe->rows_) {
                            r.had.reset();
                            for (const auto& i : installed)
                              if (i.id == r.offered.id) r.had = i;
                            if (r.had) {
                              r.elsewhere.clear();
                              r.state = installStateOf(r.offered, &*r.had);
                            } else if (r.elsewhere.isEmpty()) {
                              r.state = InstallState::New;
                            }
                            r.act = r.elsewhere.isEmpty() && installsByDefault(r.state);
                          }
                          safe->showRows();
                        });
}

// ---------------------------------------------------------------- doing

void InstallPanel::askWhere() {
  const int64_t free = freeBytesAt(root().getFullPathName().toStdString());
  juce::String text = TRANS("Organs are installed in %1").replace("%1", root().getFullPathName());
  if (free >= 0) text << TRANS(", with %1 free on that drive").replace("%1", sizeText(free));
  text << ".\n\n" << TRANS("Install them there, or choose a folder on another drive? Organs installed in "
                           "earlier folders stay where they are and are still listed.");
  juce::Component::SafePointer<InstallPanel> safe(this);
  auto* box = new juce::AlertWindow(TRANS("Where to install"), text, juce::MessageBoxIconType::QuestionIcon, this);
  box->addButton(TRANS("Install here"), 1, juce::KeyPress(juce::KeyPress::returnKey));
  box->addButton(TRANS("Choose a folder..."), 2);
  box->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
  box->enterModalState(true, juce::ModalCallbackFunction::create([safe](int result) {
                         if (safe == nullptr) return;
                         if (result == 1) {
                           safe->proc_.confirmOrgansFolder();
                           safe->start();
                         } else if (result == 2) {
                           safe->chooseFolder();
                         }
                       }),
                       true);
}

void InstallPanel::start() {
  if (busy_) return;
  // Asked once, on a desktop, before the first install writes anything.
  if (mode_ == Mode::Install && !kMobile && !proc_.organsFolderConfirmed()) {
    askWhere();
    return;
  }
  std::vector<InstallComponent> chosen;
  for (const auto& r : rows_)
    if (r.act) chosen.push_back(r.offered);
  if (chosen.empty()) return;
  auto alive = alive_;
  auto cancel = cancel_;
  auto fraction = fraction_;
  cancel->store(false);
  const std::string organsRoot = root().getFullPathName().toStdString();
  juce::Component::SafePointer<InstallPanel> safe(this);

  if (mode_ == Mode::Install) {
    setBusy(true, TRANS("Installing..."));
    auto archive = archive_;
    juce::Thread::launch([=] {
      std::string error;
      const bool ok = installComponents(*archive, chosen, organsRoot, error, [=](double f) {
        fraction->store(f);
        return alive->load() && !cancel->load();
      });
      juce::MessageManager::callAsync([=] {
        if (safe != nullptr) safe->finishInstall(ok, utf8(error));
      });
    });
    return;
  }

  // Removing cannot be undone: said once, with what goes.
  int64_t bytes = 0;
  for (const auto& c : chosen) bytes += c.bytes;
  const auto question = TRANS("Remove %1 component(s), %2, from %3? Removed files cannot be recovered.")
                            .replace("%1", juce::String(static_cast<int>(chosen.size())))
                            .replace("%2", sizeText(bytes))
                            .replace("%3", root().getFullPathName());
  juce::AlertWindow::showOkCancelBox(
      juce::MessageBoxIconType::WarningIcon, TRANS("Uninstall"), question, TRANS("Uninstall"), TRANS("Cancel"), this,
      juce::ModalCallbackFunction::create([=](int answer) {
        if (answer != 1 || safe == nullptr) return;
        safe->setBusy(true, TRANS("Removing..."));
        juce::Thread::launch([=] {
          std::string error;
          const bool ok = uninstallComponents(organsRoot, chosen, error);
          juce::MessageManager::callAsync([=] {
            if (safe != nullptr) safe->finishUninstall(ok, utf8(error));
          });
        });
      }));
}

void InstallPanel::finishInstall(bool ok, const juce::String& error) {
  setBusy(false);
  if (!ok) {
    status_.setText(error == "cancelled" ? TRANS("Cancelled. The organs folder is as it was.")
                                         : TRANS("Not installed: %1. The organs folder is as it was.").replace("%1", error),
                    juce::dontSendNotification);
    status_.setColour(juce::Label::textColourId, error == "cancelled" ? kDim : kWarn);
    return;
  }
  proc_.rememberSampleLibrary(root());
  // The organs this install put in place, to open now or later from Open.
  justInstalled_.clear();
  const auto organs = proc_.installedOrgans();
  juce::StringArray names;
  for (auto& r : rows_) {
    if (!r.act) continue;
    if (r.offered.kind == InstallComponent::Kind::Organ) {
      names.add(utf8(r.offered.name));
      const juce::File at = root().getChildFile(utf8(r.offered.id));
      for (const auto& f : organs)
        if (f == at || f.isAChildOf(at)) justInstalled_.add(f);
    }
    r.had = r.offered;
    r.elsewhere.clear();
    r.state = InstallState::Same;
    r.act = false;
  }
  if (smallFilesOwned_) {
    smallFiles_.deleteRecursively();
    smallFilesOwned_ = false;
  }
  status_.setColour(juce::Label::textColourId, kGood);
  status_.setText(names.isEmpty() ? TRANS("Installed.")
                                  : TRANS("Installed: %1. It is listed in Open, under Installed.")
                                        .replace("%1", names.joinIntoString(", ")),
                  juce::dontSendNotification);
  if (!justInstalled_.isEmpty()) {
    openButton_.setButtonText(TRANS("Open %1").replace("%1", justInstalled_[0].getFileNameWithoutExtension()));
    openButton_.setVisible(true);
  }
  showRows();
  resized();
}

void InstallPanel::finishUninstall(bool ok, const juce::String& error) {
  setBusy(false);
  scanInstalled();
  status_.setColour(juce::Label::textColourId, ok ? kGood : kWarn);
  status_.setText(ok ? TRANS("Removed.") : TRANS("Not everything was removed: %1").replace("%1", error),
                  juce::dontSendNotification);
}

void InstallPanel::setBusy(bool busy, const juce::String& what) {
  busy_ = busy;
  fraction_->store(busy ? -1.0 : 0.0);
  progress_ = busy ? -1.0 : 0.0;
  bar_.setVisible(busy);
  cancel_button_.setVisible(busy);
  close_.setVisible(!busy);
  folderButton_.setEnabled(!busy);
  if (busy) {
    openButton_.setVisible(false);
    status_.setColour(juce::Label::textColourId, kDim);
    status_.setText(what, juce::dontSendNotification);
  }
  table_.updateContent();
  updateSummary();
  resized();
}

void InstallPanel::timerCallback() {
  if (busy_) progress_ = fraction_->load();
}

// ---------------------------------------------------------------- table

int InstallPanel::getNumRows() { return static_cast<int>(rows_.size()); }

void InstallPanel::paintRowBackground(juce::Graphics& g, int row, int, int, bool selected) {
  g.fillAll(selected ? juce::Colour(0xff2a2f3a) : (row % 2 == 0 ? kRowBg : kRowAlt));
}

void InstallPanel::paintCell(juce::Graphics& g, int row, int column, int w, int h, bool) {
  if (row < 0 || row >= static_cast<int>(rows_.size())) return;
  const Row& r = rows_[static_cast<size_t>(row)];
  juce::String text;
  juce::Colour colour = kText;
  switch (column) {
    case kType: text = typeText(r.offered.kind); colour = kDim; break;
    case kName: text = nameText(r.offered); break;
    case kVersion: text = r.offered.version.empty() ? juce::String("-") : utf8(r.offered.version); break;
    case kInstalled:
      text = stateText(r);
      colour = kDim;
      if (mode_ == Mode::Install && r.state == InstallState::Newer) colour = kGood;
      if (mode_ == Mode::Uninstall && r.act && !r.neededBy.isEmpty()) colour = kWarn;
      break;
    case kSize: text = sizeText(r.offered.bytes); colour = kDim; break;
    default: return;
  }
  g.setColour(colour);
  g.setFont(juce::Font(juce::FontOptions(14.0f)));
  g.drawText(text, 6, 0, w - 10, h, column == kSize ? juce::Justification::centredRight : juce::Justification::centredLeft,
             true);
}

juce::Component* InstallPanel::refreshComponentForCell(int row, int column, bool, juce::Component* existing) {
  if (column != kAction || row < 0 || row >= static_cast<int>(rows_.size())) {
    delete existing;
    return nullptr;
  }
  auto* box = dynamic_cast<ActionBox*>(existing);
  if (box == nullptr) {
    delete existing;
    box = new ActionBox(*this);
  }
  box->show(row);
  return box;
}

juce::String InstallPanel::getCellTooltip(int row, int column) {
  if (row < 0 || row >= static_cast<int>(rows_.size())) return {};
  const Row& r = rows_[static_cast<size_t>(row)];
  if (column == kName) return utf8(r.offered.id) + (r.offered.supplier.empty() ? "" : " - " + utf8(r.offered.supplier));
  if (column == kInstalled) return stateText(r);
  return {};
}

// ---------------------------------------------------------------- layout

void InstallPanel::paint(juce::Graphics& g) { g.fillAll(kBg); }

void InstallPanel::resized() {
  auto r = getLocalBounds().reduced(14);
  heading_.setBounds(r.removeFromTop(26));
  auto folder = r.removeFromTop(kMobile ? 40 : 30);
  folderButton_.setBounds(folder.removeFromRight(110).reduced(0, 2));
  folder.removeFromRight(8);
  folderLabel_.setBounds(folder);
  r.removeFromTop(6);
  auto bottom = r.removeFromBottom(kMobile ? 44 : 34);
  go_.setBounds(bottom.removeFromRight(130));
  bottom.removeFromRight(8);
  (cancel_button_.isVisible() ? cancel_button_ : close_).setBounds(bottom.removeFromRight(110));
  if (openButton_.isVisible()) openButton_.setBounds(bottom.removeFromLeft(juce::jmin(320, bottom.getWidth() - 10)));
  r.removeFromBottom(6);
  summary_.setBounds(r.removeFromBottom(22));
  status_.setBounds(r.removeFromBottom(22));
  if (bar_.isVisible()) {
    r.removeFromBottom(4);
    bar_.setBounds(r.removeFromBottom(18));
  }
  r.removeFromBottom(6);
  table_.setBounds(r);
}

// ---------------------------------------------------------------- windows

namespace {
void launch(MasterpieceProcessor& p, InstallPanel::Mode mode, const juce::File& package, InstallPanel::Open open) {
  juce::DialogWindow::LaunchOptions o;
  o.content.setOwned(new InstallPanel(p, mode, package, std::move(open)));
  o.dialogTitle = mode == InstallPanel::Mode::Install ? TRANS("Install organ package") : TRANS("Uninstall organ");
  o.dialogBackgroundColour = kBg;
  o.escapeKeyTriggersCloseButton = true;
  o.useNativeTitleBar = true;
  o.resizable = true;
  launchDialog(o);
}
}  // namespace

void InstallPanel::showInstall(MasterpieceProcessor& p, Open open, juce::File package) {
  if (package != juce::File()) {
    launch(p, Mode::Install, package, std::move(open));
    return;
  }
  // The package's own names: a RAR, its first volume, a GrandOrgue package,
  // and the names the format's installer gives its packages.
  static std::unique_ptr<juce::FileChooser> chooser;
  chooser = std::make_unique<juce::FileChooser>(TRANS("Choose an organ package to install"), p.openFolder(),
                                                "*.rar;*.orgue;*.CompPkg_Hauptwerk_rar;*.HauptwerkOrgan;*.zip");
  auto* proc = &p;
  chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                       [proc, open](const juce::FileChooser& fc) {
                         const auto f = fc.getResult();
                         if (!f.existsAsFile()) return;
                         proc->setOpenFolder(f.getParentDirectory());
                         // Replacing the chooser has to wait until its callback returns.
                         juce::MessageManager::callAsync([proc, open, f] { launch(*proc, Mode::Install, f, open); });
                       });
}

void InstallPanel::showUninstall(MasterpieceProcessor& p, Open open) { launch(p, Mode::Uninstall, {}, std::move(open)); }

}  // namespace mp::ui
