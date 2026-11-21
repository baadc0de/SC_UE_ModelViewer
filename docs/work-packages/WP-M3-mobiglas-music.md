# WP-M3 — mobiGlas Music

**Depends on** WP-A1 for UE audio and WP-M2 for the mobiGlas shell. **Build locks** Cargo and Unreal.

The Music app lists soundtrack cues from the active local game archive. Select a cue and press Play.
Pause, resume and stop control one non-positional soundtrack voice. Music keeps playing while the
mobiGlas is closed or another app is open. Changing archives or leaving the viewer world stops it.

## Archive source

The soundtrack banks in `Data/Sounds/wwise` identify their category as `MUS_Music_*`. The
`MusicLogicConfig.StarCitizenConfig` DataCore record defines location and star-system trigger prefixes.
The global soundtrack's Wwise music segment names are authored cue markers. The Music library uses
those markers to label the audio, retaining the original name and the exact bank, segment and media IDs
in the FFI response. The row tooltip shows the authored cue marker. The display label uses the marker's
words with the Wwise namespace removed. Where several sources share a label, the library numbers the
listing variants.

The catalogue follows each segment's track and playlist to its source media. It lists named cues with a
single, intact audio source spanning their whole timeline, omitting placeholder, test, and silence names.
Adaptive arrangements with multiple layers, transitions and trimmed source files require a separate
timeline player. This app auditions the named source recordings. The game's adaptive score mix and cue
automation remain outside its transport.

The active session indexes the bank metadata on demand. The player reads and converts only the selected
WEM to Ogg through StarBreaker. A1's procedural Ogg wave plays the source through the UE audio mixer.
The source and converted bytes stay local and no music files ship with the viewer.

## Verification

- `cargo test --locked -p scviewer-core --lib music` passed **2/2**, including rejection of partial and event-only sources.
- `cargo test --locked -p scbridge-ffi --test header` passed **1/1** after generating and staging the ABI header.
- LIVE 4.10.2 indexed **46 cues**. The first catalogue took **0.443 s** after archive open. Biome Coastal,
  Lorville Flight and Microtech New Babbage Interior decoded nonzero samples through the A1 stream as
  stereo 48 kHz, with lengths **295.000 s**, **182.769 s** and **285.800 s**. An unknown cue ID failed
  with an empty return blob.

The procedural Viewer smoke uses a Development game-mode host with D3D12, audio enabled and
`-SCDefaults`. Open the app with `sc.MobiGlas.App Music` and `sc.MobiGlas 1`. The RPC console
`sc.Music.State` reports library readiness and the decoder frame count. Use `sc.Music.List` to find
the bank-qualified cue ID. Use `sc.Music.Control <cue-id>`, `sc.Music.Control pause` and
`sc.Music.Control stop` to exercise the same transport as the widget. Close the app while playing
and verify that decoded frames continue increasing.
