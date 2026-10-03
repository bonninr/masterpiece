# Store packaging

Prepared, not submitted. Each channel below needs a decision and, for most, an
account held by the maintainer.

| Channel | Files | To publish |
|---|---|---|
| AppImage | CI (`build.yml`, PR #145) | nothing: attached to each release |
| Flathub | `flatpak/io.github.bonninr.Masterpiece.yml`, `io.github.bonninr.Masterpiece.metainfo.xml` | test build, then a pull request to flathub/flathub |
| Snap Store | `snap/snapcraft.yaml` | snapcraft account; `snapcraft` build; request the `alsa` auto-connection |
| winget | `winget/manifests/...` | pull request to microsoft/winget-pkgs |
| Google Play | (to do) app bundle task, listing, privacy policy | developer account ($25), 14-day closed test with 12 testers |
| Apple App Store | (to do) UnRAR reader for iOS, privacy manifest, signing | Apple Developer Program ($99/year), JUCE Starter licence |

Per release: update the tag and commit in the Flatpak and Snap recipes, the
`<releases>` entry in the metainfo, and a new winget version folder with the
installer's SHA-256.

Not yet tested: the Flatpak and Snap builds have not been run.
