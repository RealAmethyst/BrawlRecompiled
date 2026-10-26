# Audio description for the game's movies

How a movie gets an audio description, from the disc file to the in-game track. Decisions and status live in
`TICKETS.md` (A11Y-20); verified game facts in `RESEARCH.md` ("Movies and Subspace Emissary name cards").

## How it works in game

- Every movie plays through main.dol's THPPlayer. `wiicompiled/runtime/src/accessibility/audio_description.cpp` hooks
  its open, play, pause and stop, and looks for the movie's description in the pack beside the executable:
  `accessibility/audio-description/en/<movie>/cues.toml` with the clips it names (`<movie>` is the THP file name
  without `.thp`, for example `OPMovie` or `01-00`). A movie without one plays unchanged.
- Each clip starts when the movie reaches its cue, timed by the movie's own audio clock. A pause holds the clip and
  resume continues it; skipping or closing the movie stops it.
- Levels match the mix the designer approved: game sound 6 dB down for the whole described movie, 2 dB further while a
  clip plays, clips at +2 dB per channel, a limiter catching peaks (`audio_description_mix.h`).
- On by default; `[accessibility] audio_description = false` in Config.toml turns it off.
- The pack is distributed separately, never committed: the clips are synthesized speech made per release.

## Making a description

1. **Movie file.** The converted movies are MKV files (H.264 video, FLAC audio decoded the way the game decodes it,
   character name cards as an SRT track). They are game assets: keep them out of the repository.
2. **Brief the audio-describer agent** with the MKV, an output folder beside it (`<movie>.audio-description/`), and
   context: the game, the movie's place in the story, the characters already introduced (the name-card schedule
   shows when the game introduces each one), the game's names for the fighters, and the pronunciation file. The
   agent's own definition holds the voice, rate and description rules.
3. **Listening review** by the accessibility designer on the agent's preview (`<movie>.described.mkv`). Feed changes
   back to the agent; a mispronounced name goes into the pronunciation file and the affected clips are synthesized
   again.
4. **Pack it** once approved:
   `python3 accessibility/tools/ad_pack.py <movie>.audio-description <movie> <pack folder>`
   It checks the cues (order, clips present, no clip running into the next) and writes `en/<movie>/`.
5. **Install** the pack folder as `accessibility/audio-description/` beside the executable, then play the movie in
   game. The log (`[accessibility] audio description: ...`) names the movie loaded and any reason it was skipped.

## Checking the mixer offline

`accessibility/tools/ad_mix_check.cpp` runs the runtime's mixer without the game:

- `ad_mix_check selftest`: pause, resume, loop and stop behaviour on synthetic signals.
- `ad_mix_check mix <movie.s16> <manifest> <fps> <out.s16>`: mixes a movie's decoded sound (S16LE stereo, 32 kHz)
  with clips listed as `start path` (S16LE mono, 32 kHz), using the same block size and clock as the runtime, for
  comparison with the approved preview.

Build: `g++ -std=c++20 -O2 -I wiicompiled/runtime/include accessibility/tools/ad_mix_check.cpp -o ad_mix_check`.
