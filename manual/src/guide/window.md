# A tour of the window

This page describes the main window from top to bottom: the five menus and
every control in the top bar, the console pages and how the mouse works on
them, pages in windows of their own, panels, the score, the plain stop list,
the expression pedals, the on-screen keyboard and the context menus.
Registration, pistons, tuning and recording are covered on
[Playing](playing.md).

![The main window with Nancy's console and a few stops drawn](../images/console-nancy.jpg)

The window opens at 1280 x 760 pixels the first time, and afterwards at the
place and size it had when the program last closed. A saved place that no
display shows any longer is replaced by the centre of the main display. The
window can be resized freely. From top to bottom it holds:

| Part | Height | Shown |
|---|---|---|
| Top bar | 36 px (a second row of 36 px in a window narrower than 1024 px) | Always |
| Page tabs | 28 px | When the console has more than one page, or a panel or the score is shown as a tab |
| Console, panel, score or stop list | The rest | Always |
| Expression pedals | 120 px wide, at the right | **View > Expression pedals** |
| On-screen keyboard | 96 px, plus 24 px for the manual selector | **View > Keyboard** |
| Status line | 20 px | Always |

The title bar reads `Masterpiece <version> - <organ name>`, for example
`Masterpiece 0.9.0 - Nancy`. A build other than a tagged release shows the
first five characters of its commit in place of the version: `main 3fa91` for
a build of the main branch, `dev 3fa91` for any other build. **Help > About
Masterpiece**, the diagnostics report and the log name the build the same way.

## The top bar

From left to right the bar holds the **File**, **Organ**, **View**, **Settings**
and **Help** menus; the volume slider with its dB readout; the level meter;
**Set**; the stepper (**<**, the frame readout and **>**); the tuning button,
which shows the temperament and the pitch of A; the save button; and the Panic
button, drawn as an octagon with a cross. The bar holds what a player looks at
or presses while playing; choices made once are in the menus.

![The File menu](../images/menu-file.jpg)

### File

The organs and the settings files: what is opened and saved.

| Item | What it does |
|---|---|
| **Open organ...** | Opens the favourites window, which has a button for the file dialog. Until an organ has been added to the favourites, **Open organ...** goes straight to the file dialog. See [Opening an organ](opening.md). |
| **Open another version** | A submenu, shown when the loaded organ's package or folder holds more than one definition (a perspective each, or full and light). Each definition is listed by its file name without the extension. The loaded one is ticked and disabled; choosing another loads it. |
| **Import settings...** | Reads a settings archive (a `.zip` written by **Export settings...** or **Export organ settings...**). A question names what the file holds and what it replaces; **Import** puts it in place, after the settings there now are saved in the backups folder. |
| **Export settings...** | Writes every setting, the program's own and those of each organ, to one `.zip` archive. |
| **Export organ settings...** | Writes the loaded organ's settings to one `.zip` archive. Disabled until an organ is loaded. |
| **Exit** (Windows), **Quit** (Linux) | Closes the program, as the window's close button does. |

**Import settings...**, **Export settings...** and **Export organ settings...**
are absent on phones and tablets. **Exit** and **Quit** are absent in the plugins,
where the host closes the plugin, and on macOS, where **Quit Masterpiece** is in
the application menu. The archives are described on
[Files and folders](files.md).

### Organ

The organ that is loaded: its settings, its tuning, its combinations. Every
item but **All notes off** is disabled until an organ is loaded.

| Item | What it does |
|---|---|
| **Organ settings...** | Opens the settings of the loaded organ. See [Organ settings](organ-settings.md). |
| **Temperament and tuning...** | Opens the tuning panel, as the tuning button in the bar does. See [Transposer, temperament and pitch](playing.md#transposer-temperament-and-pitch). |
| **Combinations** | Shows or hides the Combinations window. Ticked while shown. See [The player's own combinations](playing.md#the-players-own-combinations). |
| **Extra couplers** | The program's own couplers between the manuals, at 16', 8' and 4', for an organ that has too few of its own. Ticked while on. The couplers are built with the organ, so choosing the item saves the choice for this organ and loads it again. |
| **Reload organ** | Loads the organ again from its files, with its current settings. |
| **Copy settings from another organ...** | Copies the **MIDI map**, the **Combinations** or the **Mixer and voicing** of another organ that has settings saved, chosen under **From**, to this one. They replace this organ's own. Absent on phones and tablets. |
| **Licence accepted** | Present when the organ's samples need the publisher's licence. Ticked once the licence is confirmed. Choosing it while ticked withdraws the confirmation and loads the organ again without the licensed samples; choosing it while unticked asks the licence question. See [Licences](opening.md#licences). |
| **All notes off** | Releases every key on every channel. |

### View

![The View menu](../images/menu-view.jpg)

What the window shows, and the windows beside it, in four groups.

| Item | What it does |
|---|---|
| One item per console page | The page's name. Shows that page. The page shown is ticked. An organ that ships no console artwork has **Stop list** here in their place, which shows the plain list. |
| **Open page in new window** | A submenu with one item per console page, ticked while the page has a window; choosing a ticked page closes its window. Present in the standalone application, while the console is shown. See [A page in its own window](#a-page-in-its-own-window). |
| **Stop list** | Shows the plain list of stops over the console, for a player who cannot see the console. Present when the organ ships console artwork; ticked while the list is shown. See [The plain stop list](#the-plain-stop-list). |
| One item per panel | The panel's name. Shows the panel's tab, or brings the panel's window to the front. Ticked while that panel is shown. |
| **Panel of the whole organ** | Shown while the organ has no panel. Makes the first panel. |
| **New panel** | Shown once the organ has a panel. Makes a panel; disabled when the organ already has two. |
| **Rename or delete a panel** | A submenu, shown when the organ has a panel, with **Rename \<name\>...** and **Delete \<name\>** for each. |
| **Score** | Shows the score tab, or brings the score's window to the front. Ticked while the score is shown. See [The score](#the-score). |
| **Expression pedals** | Shows or hides the strip of expression pedal faders. Present when the organ has at least one swell shoe. |
| **Keyboard** | Shows or hides the on-screen keyboard. |
| **Highlight played keys** | Lights the keys of the drawn manuals as they are played. Ticked by default. |
| **Scale small pages to fit** | Magnifies a page much smaller than the organ's first page until it fills the window. Off by default. See [Scaling](#scaling). |
| **Fit window to page** | Sizes the window to the page shown, at 100 % or at the largest size that fits the screen, and centres it on its screen. |
| **Console layout** | A submenu with the console's layouts, the one in use ticked. Present when the console is shown and the organ offers more than one layout. See [Layouts](#layouts). |
| **Split keyboard** | Shows or hides the Split keyboard window. See [Split keyboard](split-keyboard.md). |
| **Recorder** | Shows or hides the Recorder window. Ticked while shown. See [Recorder](playing.md#recorder). |

The expression pedals, the keyboard and the page windows are described in the
sections below. **Highlight played keys** is described under
[Highlight played keys](#highlight-played-keys).

### Settings

![The Settings menu](../images/menu-settings.jpg)

The program's settings, the same for every organ: one item per section of the
Settings window, each opening the window on that tab.

| Item | Tab |
|---|---|
| **General...** | The display mode, the organ at start, the sample cache and its folder, the package folder |
| **Audio...** | The audio device, the output compressor and limiter |
| **MIDI...** | The MIDI inputs and output, the default channels, the console's buttons |
| **Performance...** | The performance preset, the audio engine, the memory limit and the polyphony |
| **Reverb...** | The room |
| **Metronome...** | The metronome |
| **Combination sets...** | The combination sets kept as favourites |
| **Console display...** | How the console is drawn |
| **Language...** | The language of the interface |
| **Remote control...** | The control API, the web console, MIDI System Exclusive and OSC |

The Settings window itself has an eleventh tab, **Diagnostics**, opened from
**Help > Diagnostics...**. In the VST3, LV2 and AU plugins the menu lists
**General...**, **Performance...**, **Reverb...**, **Metronome...**,
**Combination sets...**, **Console display...** and **Language...**: the host
owns the audio device and the MIDI ports, and **Organ settings...** opens
without its **MIDI** tab. See [Settings](general-settings.md).

The display mode on the **General** tab, **Simple** or **Advanced**, decides
how much the settings windows show. **Simple**, the default, shows the main
settings with their presets; **Advanced** shows every setting. The first-run
wizard asks for it first.

### Help

| Item | What it does |
|---|---|
| **Documentation** | Opens the project's page on GitHub, with its manual, in the browser. |
| **Release notes** | Opens the releases page on GitHub in the browser. |
| **Check for updates...** | Asks GitHub for the newest release's version number at once. The status line then reads **Masterpiece \<version\> is the latest version.**, or **No answer from the update server. Check the network and try again.**, or the update notice appears. |
| **Diagnostics...** | Opens the Settings window on its **Diagnostics** tab: the log, the checks and the diagnostics report. Present in the standalone application. |
| **About Masterpiece** | The version or build, and the licence. |

### The update notice

The standalone program asks GitHub once a day for the newest release's version
number, ten seconds after it starts. The request carries no account and nothing
about the player. When a newer release is out, a notice at the right of the
status line reads **Masterpiece \<version\> is out**, with three buttons:

| Button | What it does |
|---|---|
| **Download** | Opens the newest release's page, with its downloads, in the browser. |
| **Never check again** | Turns the daily check off. **Check for updates daily** on **Settings > Diagnostics** turns it on again. |
| **×** | Closes the notice. |

The notice closes by itself after 30 seconds; during an organ load it waits,
and the 30 seconds count from the end of the load. **Check for updates daily**
is on by default and is stored as `updatecheck` in `settings.mpglobal`. A
scripted run (`--play-midi`, `--gui-only`) makes no check, and neither does a
start with the environment variable `MASTERPIECE_UPDATE_CHECK=0`.

### Controls in the bar

| Control | What it does | Default and range |
|---|---|---|
| Volume slider | The master volume, with a readout in dB to its right. It widens with the window, from 160 px to 320 px. The bottom of the slider, -40 dB, is silence. Each organ keeps its own volume. | 0 dB; -40 to +24 dB in steps of 0.1 dB |
| Level meter | Two rows of 14 lamps, left and right, showing the output level after the master fader. The scale starts at -48 dB; lamps 10 to 12 are amber and lamps 13 and 14 are red. The meter is hidden when less than 40 px remain for it. | |
| **Set** | Stores what is drawn into the piston or step chosen last. See [Capture](playing.md#capture). | |
| **<**, frame readout, **>** | The sequencer. The readout reads **Frame 003 / 012** (the current frame and the last one holding anything), or **No frames**. A click on the readout asks for a frame number. | Frames 000 to 998 |
| Tuning button | Shows the temperament, the pitch of A and the transposition, for example `Equal  A 439.8`, or `Equal  A 440.0  +2` with a transposition. A click opens the tuning panel. See [Transposer, temperament and pitch](playing.md#transposer-temperament-and-pitch). | |
| Save button (a disk icon; its name is **Save state**) | Records the stops as they are now as the organ's starting state. See [Remembered between sessions](playing.md#remembered-between-sessions). | |
| **Panic** (an octagon with a cross, last in the bar) | Releases every key on every channel, silences every note still sounding, and opens the MIDI inputs again. | |

The bar is one row from a window 1024 px wide, in every language. It shortens
in a fixed order when the window narrows: the volume fader narrows to 160 px,
then the level meter gives way. Each menu button is as wide as its name, from
52 px up to 88 px; a longer name is drawn narrower. Below that, the menus and
the fader move to a row of their own.

Plain sample playback, with the wind model, tremulants, enclosure shading,
voicing and the room reverb bypassed, is the **Bypass all effects** box on the
**Loading** tab of Organ settings, in the **Advanced** display mode. It is saved
per organ.

On a phone or tablet the sequencer buttons are 44 px wide, the **Panic** and
**Save state** buttons 46 px, **Set** 64 px and each menu button at least 84 px.
On a computer they are 30 px, 38 px, 56 px and 52 px.

### The status line

The strip along the bottom edge shows, from left to right and separated by
`|`:

1. The result of the last load: the number of stops and ranks, the samples
   loaded with their size in MB, and any of `N missing (named in the log)`,
   `N unreadable (named in the log)`, `N encrypted` and `N awaiting a licence`.
   A load that stopped partway adds `INCOMPLETE (memory limit)`,
   `INCOMPLETE (memory low)` or `INCOMPLETE (load cancelled)`. A load with no
   audio reads `graphics only - no audio loaded`.
2. `voices N`: the number of voices sounding now.
3. While the sample cache is being written in silence, `creating the sample
   cache`, with a percentage and, after a few seconds, the time left. A cache
   that could not be written reads `sample cache not saved` or `sample cache
   not saved: the disk is full` until the next load.
4. When non-zero, each after a comma: `dropped N` (note starts that found no
   free voice), `no audio N` (starts whose samples were not in memory),
   `late blocks N` (audio blocks that took longer than the audio they produced)
   and, with streamed release tails, `streamed releases fell behind Nx: turn
   off "Stream release tails" in Organ settings`.

After a load with no audio, the line reads `65 stops, 312 ranks, graphics only
- no audio loaded | voices 0, no audio 24`. The line is cut short in a narrow
window; the whole text appears as a tooltip. The window has no CPU readout.
The [update notice](#the-update-notice) takes the right end of the line while
it is shown.

## Page tabs

A console that ships more than one page shows one tab per page. A page is named
as the organ names it, or **Page 1**, **Page 2** and so on. Panels and the
score shown as tabs follow the console pages. A click on a tab shows that page.
A page, panel or score stays on screen when the tabs are rebuilt, for example
when a window of its own closes or a panel is added.

In the standalone application each console page tab carries a small button,
a square with an arrow. A click on it opens the page in its own window; the button is
filled while the window is open, and a click then closes the window and brings
the page back to its tab. With a mouse the button takes 16 px of the tab; on a
touch screen it takes 44 px and the tab's full height, and the tab widens to
keep its name.

A right-click on a console page tab opens a menu:

| Item | Shown when |
|---|---|
| **Open in its own window** | The page has no window. |
| **Open in its own window, main layout**, **Open in its own window, alternate layout 1 (portrait)** and so on | The page has no window and the organ offers more than one layout. Opens the window in that layout. |
| **Bring its window to the front** | The page has a window. |
| **Close its window** | The page has a window. |

A right-click on a panel's tab offers **Rename \<name\>...**. In the plugins the
pages stay tabs and the icon and menu are absent.

## The console

Where an organ ships console artwork, the console is the organ's own: the
bitmaps of its drawstops, pistons, couplers, shoes, manuals and engraved
labels, placed as the set places them. A click on a drawstop moves the switch
the drawstop is bound to and changes its picture in one step.

### Scaling

The window shows a whole page at once, so the console has no scroll bars. The
page is scaled by one factor for both directions, and the factor is raised as
well as lowered: a console drawn smaller than the window is enlarged. The
factor is the largest at which the whole page fits. The page is measured from
where its drawing starts, so a page drawn away from its top-left corner keeps
no empty band beside it. The page is centred and the spare room is split
evenly on both sides. The proportions are kept, so the case and the keys keep
their shape.

A page much smaller than the organ's first page (a settings panel a few
hundred pixels wide, for example) is shown at the scale of the first page,
which is smaller than the largest scale that fits. This applies when the first
page is at least twice as wide and twice as tall as the page shown.
**View > Scale small pages to fit** lifts the rule: every page then fills the
window. The choice is kept for every organ. A page in its own window always
fits the window.

**View > Fit window to page** sizes the window to the page shown: at 100 %
when the screen has room for it, otherwise at the largest size that fits the
screen the window is on, with the bars and strips the window shows around it.
The window is centred on that screen.

Clicks, drags and right-clicks land on the picture at any scale.

![A narrow, tall window showing a portrait layout](../images/console-portrait.jpg)

### Layouts

A set can draw its pages again for another screen shape, with up to three
alternate layouts beside the main one. A layout counts as portrait when its
console is taller than it is wide. **View > Console layout** lists them as **Console: main**, **Console: alt 1**, **Console: alt 2 (portrait)**
and so on.

The window follows its own shape. When the area for the console turns taller
than it is wide, the first portrait layout is chosen; when it turns wider
again, the first landscape layout is chosen. A layout picked by hand holds
until the shape turns. A page that declares nothing for the chosen layout is
drawn in the main layout, whole.

### Clicking and dragging

| Object | Action |
|---|---|
| Drawstop, rocker, coupler, tremulant | A click toggles the switch. A drag that starts on one and crosses others sets each one it enters to the state the first took, so one stroke along a row draws or retires the whole row. |
| Piston, setter, or any button the organ marks as momentary | The button is pressed while the mouse button is down and released with it. A piston fires on the press. |
| Drawn key of a drawn manual | A click plays the key, on the channel of the keyboard that the manual belongs to, through the same path as a MIDI key. The key sounds while the button is held. Sliding along the manual plays the keys under the pointer in turn; sliding from one manual onto another moves the note to the other manual. |
| Swell shoe, crescendo pedal, dial or slider | A press takes hold of the control and moves nothing. A drag then moves the value from where it was. |
| Stop with no pipes in the set | A click on it is refused while it is pushed in. See [Stops that make no sound](#stops-that-make-no-sound). |

A control is dragged up and down, with the pointer's movement scaled to half a
unit of the 0 to 127 range per pixel. A control drawn lying down, such as a
slider labelled from **Original** to **Silent**, is dragged sideways: right is
more, and the movement is scaled to the drawn width, between 0.3 and 1 unit per
pixel. The axis is whichever the pointer has moved further along from the press.
A control whose artwork says that clicking higher decreases the value is
reversed. The drag keeps working when the pointer leaves the picture, and the
value stops at the ends of its travel.

A drag of the crescendo pedal fires each step it crosses, in order. See
[Crescendo](playing.md#crescendo).

The console has no mouse-wheel handler: the wheel does not move a drawstop,
a piston, a shoe or the crescendo.

On a phone or tablet a finger held still for half a second opens what a
right-click opens, on the drawn console and on the panels. A finger that moves
more than 8 pixels is sliding and opens nothing. A stop changes as the finger
lifts, so a stop held for its menu stays as it was. The environment variable
`MASTERPIECE_TOUCH=1` works presses the same way on a computer.

### Stops that make no sound

Two kinds of stop are drawn and move but make no sound. A tooltip names the
cause when the pointer rests on the drawstop for 700 ms.

| Cause | On the console | Tooltip |
|---|---|---|
| The stop was left out of the load in **Organ > Organ settings...**, tab **Stops, ranks and perspectives** | The drawstop is veiled: the picture is darkened by 55 % black, in the shape of the picture. The stop still draws and is stored in pistons; it makes no sound. Copies of the drawstop on other pages are veiled too. Every other picture is drawn at full strength. | **\<stop\> is not loaded: chosen in Organ settings, Stops** |
| The stop's ranks ship no pipes in this sample set (a demo set's locked stops) | The drawstop is drawn as the set draws it, with no veil. A click on it is refused. | **\<stop\> is not available** |

A stop with no pipes is also held in. A general, the crescendo, a tutti, a MIDI
message and the organ's own wiring leave it pushed in. A switch that also draws
a stop with pipes stays free to draw it, and so does a switch that the organ
starts engaged. A stop whose pipes are reached only through a pallet that a
key opens counts as having pipes. When none of the organ's stops has pipes, the
organ is not a demo and its stops are left as they are.

A stop that was left out of the load has to be loaded again to sound: change the
choice in Organ settings and load the organ again.

### Context menus

A right-click opens the menu of the object under the pointer. The mappings
themselves are described on [MIDI and consoles](midi.md).

| Object | Menu |
|---|---|
| Drawstop, coupler, piston | The object's name, then its current MIDI mappings (`Note 36 ch 1 (toggles)`, `CC 20 (held)`) or **Not mapped**, **MIDI window...**, **Learn** (**Learn (held while pressed)** for a momentary button), a submenu **Learn another way** with **Toggle: each press draws or cancels**, **Held while pressed**, **Draws it only** and **Cancels it only**, and **Clear mapping**. |
| Swell shoe, crescendo, dial, slider; expression pedal fader | The control's name, its mappings (`CC 7 ch 1  (closed at 0, open at 127)`) or **Not mapped**, **MIDI window...**, **Learn: move a pedal, knob or fader**, **Closed position: where the pedal is now**, **Open position: where the pedal is now**, **Use the pedal's whole travel** and **Clear mapping**. |
| Key of a drawn manual | The MIDI window of that manual: which keyboard plays it and the pistons that bring it to a keyboard. |
| Stop, coupler, tremulant, piston, shoe or slider on a panel; stop in the plain stop list | The same menu as the object on the console. A stop reaches the drawstop the console draws for it. |
| Page tab, panel tab | See [Page tabs](#page-tabs). |
| Empty part of the console | Cancels a learn that is armed. |

### The console without sight

Every drawn stop, coupler and piston carries an invisible component over it,
where the picture is. A screen reader finds the control there, reads its
name, and reads whether it is drawn. A stop's name carries its division,
**Principal 8, Great**; a stop that ships no pipes reads as **not available**.

| Key | What it does |
|---|---|
| Tab | Moves to the next control, and back with Shift and Tab. |
| Space or Enter | Draws the stop or retires it. A piston is fired. |

The console keeps its painting and its clicks: the components over the
controls take no clicks of their own.

## A page in its own window

A console surrounds the player: jambs to either side, the key desk in front, a
combination panel above. A set draws each on a page of its own, and each page
can open in a window of its own to spread the console over several screens. The
windows are available in the standalone application. A page is opened from the
icon on its tab, from the right-click menu of its tab, or from **View > Open
page in new window**.

A page window:

- has the title `<page name> - <organ name>`, a native title bar with minimise,
  maximise and close buttons, and can be resized;
- opens with the page's own size and shape, scaled down to fit 1600 x 1000
  pixels at most, and never above its own size; a page that draws nothing opens
  at 1000 x 700;
- opens on another display when there is one, and on a portrait display when
  its layout is portrait and one exists. With one display it opens beside the
  main window, offset 40 px down and across for each window already open;
- shows its page whole at the largest scale that fits, centred;
- opens in the layout chosen in the tab menu, or follows the shape of its own
  window as the main window does (a portrait layout when the window is taller
  than it is wide);
- passes the computer-keyboard shortcuts of the MIDI windows to the program;
- on close, returns the page to its tab in the main window.

The windows are remembered per organ: which pages are open, where, at what size
and in which layout. They close when another organ loads and reopen where they
were once the organ is in. A position that no display shows any longer is moved
onto a display that exists.

## Panels

![A panel](../images/panel.png)

A panel shows the organ as equal rectangles with the name in the middle, for a
touch screen or for a console that is hard to read at a distance. An organ has up to two
panels. Each panel is a tab beside the console pages, or a window of its own on
any screen. Every rectangle stands for something the organ already has, so its
MIDI mapping is the stop's, piston's or control's own.

**View > Panel of the whole organ** makes the first panel; **View > New
panel** makes the second. The panels are named **Panel 1** and **Panel 2**
until renamed.

### Sections

The panel is divided into columns, one per section, in this order:

| Section | Holds |
|---|---|
| One per division | The division's stops, then the couplers that feed it, then its tremulant. A stop that ships no pipes is left out. A stop with no ranks at all, which is a coupler or a tremulant, stays. |
| **Extra couplers** | The program's own couplers, while **Organ > Extra couplers** is on. |
| **Other** | Couplers and tremulants that cannot be placed in a division. |
| **Pistons** | The organ's own pistons that it draws on its console. An organ that draws none has all of its pistons listed. |
| **Expression** | One fader for each swell shoe, and a **Crescendo** fader when the organ has a control with at least 3 crescendo steps. |

A division's own prefix, repeated on its stops (`HW Principal`, `HW Flute`), is
dropped from the names in that column when most of the column's names carry it.
A footage at the end of a name (`8'`, `2 2/3'`, `IV`) is set on a line of its
own below the name.

### Using a panel

| Element | Action |
|---|---|
| Stop, coupler, tremulant | A click toggles it. The rectangle is lit while drawn. A drag across several sets each to the state the first took. |
| Piston | Fires on the press and is released with the mouse button or the finger. |
| Fader | A press or a drag sets the value from the position in the track, 0 at the bottom and 127 at the top. |
| Any element, right-click or long press | The element's MIDI menu, as on the console. See [Context menus](#context-menus). |

### The bar

The bar along the top of the panel holds, from left to right:

| Control | What it does |
|---|---|
| One button per section | Shows or hides that section's column. All sections are on in a new panel. |
| **-**, **+** and the size | The size of the stops, as a percentage: 100 % is the standard size, and each press moves it by 10, from 50 % to 250 %. Ctrl with + and Ctrl with - do the same (Cmd on a Mac), and Ctrl with 0 returns to 100 %. |
| **Fit** | Sizes the stops to the window: the largest size, in steps of 5 %, at which every shown section fits. While **Fit** is on, the size follows the window and **-** and **+** are disabled; a size set with the keys ends the fit and steps from the size on show. |
| Colour selector | **Dark**, **Light**, **High contrast** or **By division**. The text contrasts with its rectangle, lit or not, in every scheme. |
| **Detach** | Moves the panel into a window of its own. The button then reads **Attach** and moves the panel back among the tabs. Absent on phones and tablets. |
| **Rename** | Asks for the panel's name, used on its tab and in its window's title. |
| **Edit** | Switches edit mode on and off. |
| **Save** | Keeps the sections, hidden elements, names, colours, size and window. The button reads **Save \*** while there are unsaved changes. |

In edit mode every element shows a tick box at its top-right corner. A click on
the tick box shows or hides the element; hidden elements are dimmed in edit
mode and absent otherwise. A click anywhere else on the element, or a
right-click, opens a dialog with the element's name. The dialog has **OK**,
**Organ's name**, which gives the organ's own name back, and **Cancel**. On a
fader the title bar names it and the rest of the fader shows or hides it.

A panel made, deleted or renamed through the View menu is written at once.
Other changes are written by **Save**. The panels of an organ are kept in
`organs/<organ>.mppanels`, in the data folder described on
[Files and folders](files.md). A panel window opens at the position and size
saved with the panel, or 1024 x 600 pixels centred.

New stops and sections that an organ update brings show by default. The file
lists only what was switched off.

## The score

![The score](../images/score.png)

**View > Score** adds a **Score** tab after the console pages and the panels.
It shows a MusicXML score (`.musicxml`, `.xml` or the compressed `.mxl`) and
plays it through the organ, with a cursor on the notes being played. Until a
score is open the tab reads **Open a MusicXML score (.musicxml, .xml or
.mxl).**

The page is engraved to the size of the tab, so the staves fill its width. A
larger zoom makes the notation larger and breaks the lines again, which gives
more pages. While the score plays, the pages turn by themselves.

| Control | What it does |
|---|---|
| **Open score...** | Opens a score file. |
| **<** and **>**, page number | Turn the page; the number reads page / pages. |
| **-**, **+** and the zoom | Zoom in steps of 10 %, from 50 % to 250 %. The button shows the zoom the page is engraved at and returns it to 100 %. |
| **Two pages** | Shows two pages side by side. |
| **Play** / **Stop** | Plays from the cursor: where playing stopped, the note last clicked, or the first note of the page turned to. |
| **Manuals...** | One row per staff with the manual it plays, or **None**. |
| **Invert** | Inverts the page's colours. |
| **Detach** / **Attach** | Moves the score into a window of its own, 1100 x 760 pixels centred, and back. Absent on phones and tablets. |

A click on a note moves the cursor there. Each staff plays on the channel a
click on its manual's drawn keys would use, so the couplers act and the drawn
keys move with the playing. By default the last of three or more staves plays
the pedal and the other staves play the organ's preferred manual. A manual
changed during playing takes effect at once.

The score fonts are written once to a **Score fonts** folder in the data
folder. When that fails, the tab reads **The score fonts could not be
prepared.** A file that cannot be read gives **The file could not be read.**,
and one with no score in it gives **No score was found in this file.**

## The plain stop list

An organ that ships no console artwork opens on the plain stop list. It shows
the organ's stops under a heading for each division, as tiles 190 px wide in as
many columns as fit, with a slim scroll bar. A drawn stop is ivory with dark
lettering; a pushed-in stop is dark. The list follows the engine, so a stop
drawn by a piston, a combination or MIDI shows drawn here too.

A stop that was left out of the load is dimmed and carries the tooltip **Not
loaded: chosen in Organ settings, Stops**. A stop whose ranks ship no pipes is
disabled and carries the tooltip **This stop's ranks ship no pipes in this
sample set**.

The list holds stops. Couplers, tremulants and pistons that the organ has as
switches alone are reached through a panel.

### The list without sight

**View > Stop list** shows the plain list over the console of an organ that
ships console artwork, and Ctrl with L (Cmd with L on a Mac) shows it and hides
it again. The keyboard focus goes to the first stop.

A screen reader reads each stop as a button that can be drawn or retired. The
name it reads is the stop's own name and the division it is in, **Principal 8,
Great**. A stop whose ranks ship no pipes reads as **not available**.

| Key | What it does |
|---|---|
| Tab | Moves to the next stop, and back with Shift and Tab. |
| Left and right arrows | Move one stop along the list. |
| Up and down arrows | Move a row of the grid, so the number of stops moved follows the width of the window. |
| Space or Enter | Draws the stop, or retires it. |

A stop that changes by a piston, a combination or MIDI is announced, with its
name and its new state. The announcement is made only while a screen reader is
listening.

## Expression pedals

![The expression pedals](../images/swell-faders.jpg)

**View > Expression pedals** shows a strip 120 px wide at the right of the window
with one vertical fader for each swell shoe of the organ. The item is present
when the organ has at least one shoe. The strip is off at every start.

- Each fader runs from 0 (closed) to 127 (open) in steps of 1. Every fader
  starts open, at 127, when an organ is loaded.
- The label under a fader is the shoe's name with the words `Enclosure`,
  `Swell box` or `Swell` taken off the front, so that two boxes called
  `Enclosure R` and `Enclosure PO` read `R` and `PO`. A shoe with no name left
  reads **Swell**. The organ's own full name appears as a tooltip.
- A shoe moved over MIDI moves its fader.
- Two enclosures worked by one shoe share one fader.
- A control that sets a level, and that the organ asks to remember, has no
  fader here; the organ draws it on a page of its own.
- The crescendo has a fader in the **Expression** section of a panel, or the
  organ's own drawn control.
- A right-click on a fader opens the menu of a continuous control described
  under [Context menus](#context-menus).

## On-screen keyboard

![The on-screen keyboard](../images/keyboard.jpg)

**View > Keyboard** shows a keyboard along the bottom of the window, 96 px
high. It spans MIDI notes 24 to 108 (C1 to C8, with middle C as C4), and the
key width follows the window width. A click plays the key through the same
path as a MIDI keyboard, so stops and couplers apply.

When the organ has more than one playable keyboard, a selector above the
keyboard names the manual that the keys play. It starts on the organ's
preferred manual, which is the widest compass when declared and otherwise the
unenclosed manual with the most pipework. The keys play on the channel assigned
to that manual in **Settings > MIDI...**, and follow a change
of channel made there while the organ is loaded. The keyboard is off at every
start. The keys are played with the mouse.

## Highlight played keys

**View > Highlight played keys** is ticked by default. While it is on, a key of a
drawn manual lights when the key is down on that manual, whether it was pressed
on the screen, on a console or by a recorded performance. A key lights on the
manual it reached. Switching the item off stops the console from looking at the
keys and from drawing them; any key lit goes dark once. Drawing the keys is the
most costly part of repainting the window in a fast passage, so the item helps
on a slow machine. The choice is kept for every organ.

## Keyboard shortcuts

The window has one shortcut of its own: Ctrl with L (Cmd with L on a Mac)
shows the plain stop list over the console, and hides it again. On a panel,
Ctrl with +, - and 0 size the stops; see [The bar](#the-bar). Any stop,
coupler, piston, setter and other object that has a MIDI window can be given a
computer key there. The key then works from the main window, from page windows
and from the Combinations window, unless the control with the focus uses the
key itself. See [MIDI and consoles](midi.md).

## Tooltips

A tooltip appears after the pointer rests on a control for 700 ms.

| Where | Tooltip |
|---|---|
| Console drawstop with no sound | **\<stop\> is not available** or **\<stop\> is not loaded: chosen in Organ settings, Stops** |
| Plain stop list tile | **This stop's ranks ship no pipes in this sample set** or **Not loaded: chosen in Organ settings, Stops** |
| Tab icon | **Open this page in its own window, to move to another screen** or **Close this page's window and bring it back here** |
| **Set** | Names the target: **Set: store what is drawn into \<name\>**, or **Set: choose a piston or a step, draw the stops, then press Set to store them there**. While Set is held from the console or the Combinations window: **Set is on from the console or the combinations window: pistons store. Press to let it go.** |
| Frame readout | **The sequencer's frame, out of the frames that hold a registration. Click to go to a frame by its number** |
| Tuning button | **Temperament, pitch and transposer for this organ** |
| **Panic** | **Panic: silence every note still sounding** |
| **Save state** | **Save the stops as they are now: the organ starts with them** |
| Expression pedal fader | The organ's full name for the shoe |
| Panel bar | **Make the stops smaller**, **Make the stops bigger**, **Fit the stops to the window**, **Give this panel a name of its own**, **Tick the elements this panel shows; touch a name to change it**, **Keep the sections, hidden elements, window and colours**, **Show this panel in a window of its own, or back among the tabs** |
| Status line | The whole text of the line |

A dialog shows its tooltips inside itself, so a tip comes up in front of the
dialog it belongs to.

## Where the plugins and phones differ

| Feature | VST3, LV2, AU | Phones and tablets |
|---|---|---|
| Pages in windows of their own | Absent; the pages stay tabs | Windows fill the screen; see [Phones and tablets](mobile.md) |
| Panel and score **Detach** | Present | Absent |
| **File > Exit** or **Quit** | Absent; the host quits | Absent |
| **Import settings...**, **Export settings...**, **Export organ settings...** | Present | Absent |
| **Organ > Copy settings from another organ...** | Present | Disabled |
| **Settings** menu | General, Performance, Reverb, Metronome, Combination sets, Console display, Language | Every tab |
| **Help > Diagnostics...** | Disabled | Present |
| **File > Open organ...** file dialog | Definition files and packages | Packages: `.rar` and `.orgue` |
| Long press for the context menu | Absent | Present |
| Top-bar buttons | Desktop sizes | At least 84 px menus, 44 px steppers, 46 px icons, 64 px **Set** |
