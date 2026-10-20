# Predictive Input Buffer

Doesn't matter if Click Between Frames is on.

- The stage hotkey (default **B**, configurable) cycles: 2/2 -> 4/4 -> 8/8 -> 16/8 -> CUSTOM -> OFF, with an on-screen notice.
- CUSTOM stage, lookahead and wave spam guard are configurable in the mod settings.
- **Panel hotkey (default V)** pauses the level and opens the panel during a run. Ship assist is configurable (0 = off).
- **Pause menu → "Buffer" button** (bottom right) opens an in-game panel to change stage, custom delays, lookahead and spam streak live.
- Debug output goes to the Geode log.

## Build

Local: install the Geode SDK + CLI and run `geode build`.
Without installing anything: push this folder to a GitHub repo; the included workflow
(`.github/workflows/build.yml`) builds it. Download the `.geode` from the run's
**Artifacts -> Build Output**.
- **Rescue mode** (experimental, off by default): set "Rescue" in the panel (try 24). The mod injects its own input when it predicts a death; a real click hands control back to you.
