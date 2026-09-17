# Masterpiece tests

Run: `ctest --preset dev` (all) / `-R MpFunctional` / `-R MpPerf`.
CI: functional with ASan+LSan+coverage (`ci-functional`, Debug), perf
(`ci-perf`, Release) — see ADR-009 in `docs/decisions/DECISIONS.md`.

## Writing tests

Derive from `mp::test::Test`, instantiate as a static at file scope — it
self-registers. Categories: `Functional`, `Perf`. Use `MP_CHECK(cond, msg)`;
throw `mp::test::Failure` for custom cases. Perf tests print their metric
and assert a floor (relaxed in Debug via `#ifdef NDEBUG`).

## Fixture policy (binding)

- All fixtures are **our own synthetic** `*.Organ_Hauptwerk_xml` /
  `*.CustomOrgan_Hauptwerk_xml` in `tests/fixtures/` (ADR-007: HW-only).
- Never copy GrandOrgue's fixtures: wrong format (`.organ`) AND GPL-2
  content. Their fixture *patterns* are ported, not their files.
- Never commit licensed HW sets (St. Anne's etc.) — user-provided only.
- WAV perf fixtures (M1.5+) are synthesized at test time, never committed
  (GO ships `perftests/0*.wav`; we generate instead).

## GrandOrgue test audit → Masterpiece mapping

| GrandOrgue test | Ported as | Status |
|---|---|---|
| GOTestOrganReader (+`minimal.organ`) | `functional.fixtures.corpus` + `tests/fixtures/minimal.Organ_Hauptwerk_xml` | now (header-level); full parse M1.2 |
| GOTestOrganModel / DrawStop / Switch / Windchest | model tests against `OrganModel` population | M1.2 (parser lands first) |
| GOTestNameMap | naming/ID-map tests | M1.2 |
| GOTestDivisionalSetter (+3 fixtures) | combination capture/recall round-trip on HW-XML combination fixtures | M3.3 |
| GOTestMidiSendProxy / GOTestMidiPlayerContent | MIDI fixed-implementation tests | M4.3 |
| GOTestSoundBuffer* (+ Perf variant) | ring-buffer unit + throughput tests | M2.2 |
| GOTestSoundStream / GOTestReleaseAlignTable | loop/release crossfade + phase-align tests | M2.1/M2.2 |
| GOTestSoundOrganEngine / CallbackConnector | engine render + callback lifecycle tests | M2.2 |
| GOTestSoundOrganEngineStress | retrigger + connect/disconnect stress cycles | M2.2 (ROADMAP) |
| GOPerfTest tool (+ perftests wavs) | MpPerf voice-render benches (61-pipe rank, 64/512/1536 voices, 256-frame buffer) | M1.5; WAVs synthesized at test time |
| GOOrganLoadTest tool | `mp-validate-odf` CLI | exists |

Adapted already (testable against real current code): ODF type detection,
unknown-type rejection, unknown-table tolerance (ADR-002), encrypted
detection case-insensitive (ADR-003 — found + fixed a real case-sensitivity
bug via the uppercase test), CODM code mapping + defaults, temperament
ratios, SampleHandle path validation (WAV/WavPack-only per ADR-011), disk
tier classification + head scaling (ADR-012, pure logic — the probe itself
does real IO and is exercised by hand, not in CI), routing allocation
(determinism, semitone spread, rank salt, empty-group silence, simple
defaults; `src/mp_audio/AudioGraph.h` is JUCE-free so tests need no audio),
DSP simple-wav fast path (ADR-005), ODF scan + temperament throughput perf.
