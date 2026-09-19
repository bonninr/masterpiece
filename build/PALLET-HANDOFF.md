# Pallet-switch fix: handoff (branch feature/pallet-switches, WIP commit)

Done:
- `Pipe.palletSwitchId` is parsed (ControllingPalletSwitchID, compact code `c`).
- `MasterpieceProcessor::buildPalletIndex` / `palletMoved`:
  - keyboard keys engage their KeyboardKey switches;
  - the SwitchNetwork opens and closes pallets, and pipes start and stop with their pallet;
  - only ranks that no StopRank reaches are indexed.
- `mp-render --load-ranks 14,2` loads only those ranks, for fast tests.
- SampleLibrary: a rank filter that matches nothing no longer loads the whole organ.
- Test `functional.odf.pallet-switch` added; 83/83 tests pass.

Verified (Alessandria, MP_PALLET_TRACE=1):
- keys open the right pallets (channel 1 is the pedal, channel 2 the Grande Organo);
- pallets close on key-up;
- the voice starts: "started rank 14 pipe 1425".

Open bug: the pipe voice is inaudible; only the key-action noise is heard.
- Layer 1425 has AmpLvl_ScalingContinuousControlID=1351.
- 1351 = DoubleLinkage op 3 of controls 1401 (default 127) and 41 ("TremulantInv"), with DestControl_Coefficient 0.00787401. The field names use underscores: FirstSourceControl_ID, DestControl_ID.
- 41 is fed by ContinuousControlLinkage from 23 and 24, LinkTypeCode 2, conditioned on switch 69 (tremulant).
- The last run traces `layerLevel` and the values of controls 41, 1401, 23 and 24 when a pipe starts. Check whether the double-linkage loader reads the underscore field names and op 3, and whether control 41 resolves to 0.
- The user suggests looking at how OdfEdit reads these.

Then:
1. Remove the MP_PALLET_TRACE fprintf lines.
2. Verify Swieta Lipka and Erfurt Predigerkirche (both use rank 3; use --load-ranks).
3. PR, merge, bump to 0.5.1, release in the v0.5.0 notes style.
4. Record the recital per build/RECITAL-NEW.md, two works per organ from build/midi, chosen to suit each organ's style.

Commits: no AI or tool attribution.

## Update (latest trace)
- Ruled out control 1351: the layer level is 0.236 (c41=1.0, c1401=0.236 [why not the default 127?], c23=0, c24=1). That is about -12.5 dB, not silence.
- Ruled out a missing file: pipe 1425's sample `GO Principale 8/A0_D/060-c.wav` exists (1.6 MB).
- Ruled out an enclosure: the pipe has no EnclosurePipe row.
- Output after the key noise is about -70 dBFS while the pallet is open, so the voice sounds about 60 dB too quietly.
- Next suspect is wind. Switch 141 "Start blower on organ load" (default on) starts blower 101 through "Blower init 1/2" (108/109), which are probably timed. If the wind model scales amplitude by pressure, no blower means silence. Try:
  - engaging switch 101 or 100 before playing (mp-render --script, or setSwitchEngaged);
  - checking pipeWindIndex_ for pipe 1425 and what the wind solver reports (`mp-render --wind`).
- Also check `mixBusForPipe(14, 60)`.
- OdfEdit (it converts Hauptwerk ODFs to GrandOrgue) resolves pallet chains and wind: its source is worth reading for how pallet-driven ranks map to GrandOrgue stops.

- Ruled out wind and DSP: `--simple` gives an identical render (peak -14.3 dBFS, rms 0.00398). Next suspects: VoiceEngine start (ratio or pitch, a sample-rate mismatch, missing audio for the pipe index), mixBusForPipe, and what "300 missing audio" counts. Compare against a Begard StopRank pipe that is known to sound.
