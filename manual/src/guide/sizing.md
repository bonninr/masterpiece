# Small, medium and large organs

This page gives recommended loading settings for three sizes of organ on computers from 8 GB of RAM to 64 GB, on the Raspberry Pi 4 and 5, and on phones and tablets. The settings are the resident format, mono folding, release streaming, the sample cache, the polyphony limit and the audio buffer. The figures come from the mechanisms and measurements on [Memory, loading and streaming](memory.md); read that page for what each setting does. The recommendations are advice, and the page marks where a figure is derived and where a choice is a judgement.

## How the sizes are defined

An organ's size for this page is the memory it takes at 24-bit with every sample resident, which for a set of WAV files is about the size of the files on disk. Use the **Stops and perspectives** tab of **Settings > Organ settings** to read the figure for a particular organ: it shows **About %1 for this choice** at the settings on the Loading tab, before the organ is loaded.

| Size | Organ | Taken as |
|---|---|---|
| Small | Fewer than about 10 stops, or a few GB | 3 GB |
| Medium | About 10 to 50 stops, up to about 20 GB | 12 GB |
| Large | 50 or more stops, or more than 20 GB | 30 GB |

The three sizes in the last column are the examples used in the tables below. A WavPack set is about 55% of its uncompressed size on disk (the estimate's assumption for a file without a readable header), so its size in this table is its disk size divided by 0.55.

## What each configuration takes

The ratios were measured on Friesach (44 stops, 17.0 GB on disk) with Masterpiece 0.3.3 and are applied here to the three example sizes. A set whose releases are a larger or smaller share of its data than Friesach's gains more or less from streaming. Check the real figure on the Stops and perspectives tab. The Nancy demo, which the Loading tab gives as 12.2 GB down to 5.6 GB with streaming (a ratio of 0.46), is close to the 24-bit streamed ratio of 0.43.

| Configuration | Ratio to 24-bit resident | Small (3 GB) | Medium (12 GB) | Large (30 GB) |
|---|---:|---:|---:|---:|
| 32-bit float, resident | 1.33 | 4.0 GB | 16.0 GB | 40.0 GB |
| 24-bit, resident | 1.00 | 3.0 GB | 12.0 GB | 30.0 GB |
| 16-bit, resident | 0.67 | 2.0 GB | 8.0 GB | 20.0 GB |
| 24-bit, releases streamed | 0.43 | 1.3 GB | 5.2 GB | 12.9 GB |
| 16-bit, releases streamed | 0.29 | 0.9 GB | 3.4 GB | 8.6 GB |
| 16-bit mono, resident | 0.33 | 1.0 GB | 4.0 GB | 10.0 GB |
| 16-bit mono, releases streamed | 0.14 | 0.4 GB | 1.7 GB | 4.3 GB |

The 16-bit resident and the 16-bit mono resident rows follow from the exact ratios 0.667 (24-bit to 16-bit) and 0.500 (stereo to mono). Those figures are the samples only. The voice pool adds to them, and streaming makes the pool larger, because every voice carries a ring of 768,000 bytes at 48 kHz.

| Polyphony | Rings with release streaming on |
|---:|---:|
| 512 voices | about 0.4 GB |
| 1,024 voices | about 0.8 GB |
| 1,536 voices (the default with streaming) | about 1.1 GB |
| 3,072 voices | about 2.2 GB |

Without streaming there are no rings, and the pool takes much less. The **Polyphony limit** box shows the exact figure for every choice.

## What each computer can hold

The memory limit is 80% of the RAM by default. The load budget is the limit minus the pool, and the samples have to fit in it. The columns below use the default polyphony with streaming, 1.1 GB of rings.

| RAM | Memory limit | Room for samples with streaming at 1,536 voices | Room for samples at 512 voices |
|---:|---:|---:|---:|
| 4 GB | 3.2 GB | 2.1 GB | 2.8 GB |
| 8 GB | 6.4 GB | 5.3 GB | 6.0 GB |
| 16 GB | 12.8 GB | 11.7 GB | 12.4 GB |
| 32 GB | 25.6 GB | 24.5 GB | 25.2 GB |
| 64 GB | 51.2 GB | 50.1 GB | 50.8 GB |

The operating system reports somewhat less than the nominal RAM (the Friesach test computer, a 16 GB machine, reported 15.9 GB), so the real limit is a little lower. On Friesach the process also took about 1.3 GB more than the samples in both streamed runs (Masterpiece 0.3.3, Windows).

> [!TIP]
> Advice: keep the whole process at about 70% of the RAM or lower, so that the operating system and other programs keep room. The default limit of 80% is a ceiling for the samples and the pool, and the program takes more memory on top. On a computer that also runs other programs (a DAW with the plugin, for example), choose the 60% or 50% entry of **Memory limit for samples**.

## Recommended settings

Each cell gives the resident format, mono folding and release streaming. They are advice for the example sizes. The label in bold is a **Memory profile** where one matches.

| Computer | Small (3 GB) | Medium (12 GB) | Large (30 GB) |
|---|---|---|---|
| 8 GB | 24-bit, stereo, resident (**Best quality**) | 16-bit, stereo, streamed (**Recommended**): 3.4 GB | 16-bit, mono, streamed (**Smallest**): 4.3 GB; leave out perspectives or ranks if the Stops and perspectives tab shows a total near the limit |
| 16 GB | 24-bit, stereo, resident (**Best quality**) | 24-bit, stereo, streamed: 5.2 GB; or 16-bit, stereo, resident (8.0 GB) when the disk is slow | 16-bit, stereo, streamed (**Recommended**): 8.6 GB |
| 32 GB | 24-bit, stereo, resident (**Best quality**) | 24-bit, stereo, resident (**Best quality**): 12 GB | 24-bit, stereo, streamed: 12.9 GB; or 16-bit, stereo, resident (20 GB) when the disk is slow |
| 64 GB or more | 24-bit, stereo, resident (**Best quality**) | 24-bit, stereo, resident (**Best quality**) | 24-bit, stereo, resident (**Best quality**): 30 GB |
| Raspberry Pi 4 or 5, 4 GB | 16-bit, mono, resident: 1.0 GB | 16-bit, mono, streamed: 1.7 GB, on a solid-state drive, at 512 voices | Does not fit. Leave out perspectives and ranks until the total is under 2 GB, with 16-bit mono streamed |
| Raspberry Pi 4 or 5, 8 GB | 24-bit, stereo, resident | 16-bit, stereo, streamed: 3.4 GB, on a solid-state drive | 16-bit, mono, streamed: 4.3 GB, on a solid-state drive, with perspectives left out when the total nears 5 GB |
| Phone or tablet | 16-bit, stereo, resident (2.0 GB), or mono (1.0 GB) when the device has 4 GB | 16-bit, mono, streamed: 1.7 GB | Load a part: leave out perspectives and ranks until the total fits, with 16-bit mono streamed |

A Raspberry Pi with 16 GB of RAM follows the 16 GB row for memory; its processor still decides polyphony and buffer. The rows for the Raspberry Pi and for phones and tablets are advice derived from the ratios above; they have not been measured on those devices.

### Reasoning

**Small organs.** A small organ fits at 24-bit on any computer with 8 GB or more, and 24-bit keeps the audio bit-for-bit as recorded. Streaming saves little on a small organ and adds the memory of the rings, so leave it off. On a Pi with 4 GB, 16-bit mono resident takes 1.0 GB and leaves room for the system; 24-bit stereo takes 3.0 GB against a limit of 3.2 GB.

**Medium organs.** An organ of 12 GB does not fit resident at 24-bit on 8 GB. The two lines of attack are 16-bit with streaming (3.4 GB) and, on a slow disk, 16-bit resident (8.0 GB, which fits only from 16 GB). At 16 GB the choice is between 24-bit with streaming (5.2 GB) and 16-bit resident (8.0 GB). The first keeps the 24-bit audio of the attacks and needs a solid-state drive; the second reads nothing from disk while playing. Advice: take streaming on a solid-state drive, and 16-bit resident on a hard disk, a network drive or an SD card. This is a judgement; the two have not been compared by measurement.

**Large organs.** An organ of 30 GB needs streaming on every computer under 64 GB, or 16-bit resident (20 GB) on 32 GB. At 8 GB only 16-bit mono streamed (4.3 GB) fits within the 5.3 GB of room, so a large organ on 8 GB is a trade of the stereo image for the whole organ. The alternative is the Stops and perspectives tab: a set recorded from several positions holds the whole organ for each, and leaving out one or two perspectives cuts the total in proportion while keeping the stereo image of the others. Advice: try leaving out perspectives before folding to mono, because the other perspectives are complete stereo organs.

**Mono.** Folding saves exactly half on stereo files and gives up the recorded image, so it is the last choice for a set recorded in a building. It also leaves the disk reads of streaming unchanged: a streamed tail is read from its stereo file and folded while playing.

**Streaming.** The disk has to deliver about 288 kB per second for each sounding release of a 24-bit stereo file at 48 kHz, and deliver the first read of every release within the resident first second. A chord of 100 pipes let go together is about 29 MB per second from 100 files. A solid-state drive meets that. A hard disk, an SD card and a network drive do not, in the derivation on the memory page; the Loading tab then shows **DISK TOO SLOW** and the status line says the releases fell behind. Advice: where the disk is slow, raise the hold to **Hold 50% of each release** or **Hold 75% of each release**, or choose 16-bit resident. A **Release length** shorter than **Whole** also holds the releases in memory and turns streaming off for them.

**Cache.** Keep the cache on for every large and medium organ: the first load writes it and later loads read one file. On Friesach sample loading fell from 13.0 s to 2.6 s. Use **One cache, replaced as organs change** when a computer plays one organ at a time. Use **One cache per organ (uses more disk)** when several organs alternate; each cache has about the size of the resident memory, so three large organs at 16-bit streamed take about 26 GB of disk. On a computer with a small disk (a phone, a Pi with a small card) leave **One cache, replaced as organs change** and place the **Cache folder** on the largest drive. Choose **In silence, while it plays** for **Save the sample cache** when the first load must finish quickly, and **Before the organ plays** otherwise; Advice: on a Pi choose **In silence, while it plays**, because the write at the system's lowest priority then waits while any voice sounds.

**Polyphony.** The default is 725 voices per processor thread with at least 1,536, and 1,536 when streaming is on. A four-thread Pi therefore defaults to 2,900 voices without streaming and 1,536 with it. Advice: lower the limit on a Pi and on a phone to 512 or 1,024 voices, which cuts the rings with streaming by 0.4 to 0.8 GB and keeps the processor from falling behind on a full organ; raise it above 1,536 on a large computer only when a full chord is heard to lose pipes. The voices taken over first are the releases that decayed longest ago and then the quietest voices; **dropped %1** on the status line counts the pipes that found no voice to take. The figures of 512 and 1,024 voices are advice; the rendering capacity of a Raspberry Pi 4 or 5 has not been measured.

**Buffer size.** The audio buffer is set in the audio device settings of the standalone application (see [Audio, room and mixer](audio.md)); a plugin takes the host's buffer. A larger buffer gives the processor more time per block at the price of latency. Advice: start with the device's default on a PC, Mac or Linux computer; on a Raspberry Pi and on a phone or tablet start at 1,024 frames and raise it to 2,048 when the status line shows **late blocks %1**. The status line is the test: play a full-organ chord with the stops of the heaviest registration and watch it. The **Faster audio engine** toggle is on by default and uses NEON on ARM and AVX2 or SSE2 on a PC. The figures of 1,024 and 2,048 frames are advice.

**Phones and tablets.** The memory limit follows the device's RAM. Advice: choose the 50% entry of **Memory limit for samples**, because the device keeps its system and other apps in the rest. Load a small or medium organ, or load a part of a large one. The sample files are read where they are, and an iPhone or iPad lends the Files app's folder to Masterpiece. See [Phones and tablets](mobile.md).

**Plugins.** Each plugin instance holds its own organ, so two instances take twice the memory. Choose the loading settings in the standalone program and save them for the organ; the plugin loads the organ with the saved settings from the same folder. In the plugin editor, **Settings > Organ settings...** opens the Organ settings window without its **MIDI** tab.

## Checking a choice

1. Open **Settings > Organ settings > Stops and perspectives**. The footer shows **About %1 for this choice (the limit is %2)**. It turns red when the total passes the limit. Keep the total under the room figure in the table above, because the load itself is checked against the limit minus the voice pool.
2. Click **Load the organ again** on that tab, or **Load the organ again to apply** on the Loading tab.
3. When the organ is loaded, the readout line of the Loading tab shows the resident samples, the streamed count and the megabytes left on disk, and the voice pool. Compare it with the estimate.
4. Play a full-organ chord and a fast passage. Watch the status line for **late blocks**, **dropped** and **streamed releases fell behind**.

A load that passes the limit stops, keeps what it read, and shows **INCOMPLETE (memory limit)**; change the settings and load again. The page [Memory, loading and streaming](memory.md#the-memory-limit) lists what to change in order of cost to the sound.

Related pages: [Organ settings](organ-settings.md), [Troubleshooting and caveats](troubleshooting.md).
