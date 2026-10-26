#!/usr/bin/env python3
"""Add one movie's approved audio description to an audio description pack.

Reads the audio-describer's output folder (cues.json and clips/), checks it, and writes
<pack>/en/<movie>/cues.toml plus the clips, the layout the runtime reads from
accessibility/audio-description/ beside the executable (src/accessibility/audio_description.cpp).

Usage:
  ad_pack.py <describer output folder> <movie name, e.g. OPMovie> <pack folder>
"""

import json
import shutil
import sys
import tomllib
import wave
from pathlib import Path

LANGUAGE = "en"


def fail(message):
    sys.exit(f"ad_pack: {message}")


def toml_string(text):
    # JSON string escapes are valid TOML basic-string escapes; DEL is the one control character JSON leaves raw.
    if "\x7f" in text:
        fail(f"text {text!r} contains a DEL character")
    return json.dumps(text, ensure_ascii=False)


def clip_seconds(path):
    try:
        with wave.open(str(path), "rb") as clip:
            if clip.getsampwidth() != 2:
                fail(f"{path}: expected 16-bit PCM")
            return clip.getnframes() / clip.getframerate()
    except wave.Error as error:
        fail(f"{path}: {error}")


def main():
    if len(sys.argv) != 4:
        fail("usage: ad_pack.py <describer output folder> <movie name> <pack folder>")
    source, movie, pack = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
    if not movie or not all(c.isascii() and (c.isalnum() or c in "_-") for c in movie):
        fail(f"movie name {movie!r}: use the THP file name without .thp")

    cues = json.loads((source / "cues.json").read_text(encoding="utf-8"))
    if isinstance(cues, dict):
        cues = cues["cues"]
    if not cues:
        fail("cues.json has no cues")

    rows = []
    for index, cue in enumerate(cues):
        clip = source / cue["clip"]
        if not clip.is_file():
            fail(f"{cue['id']}: missing clip {clip}")
        start = float(cue["start"])
        seconds = clip_seconds(clip)
        if start < 0:
            fail(f"{cue['id']}: starts before the movie ({start} s)")
        if seconds <= 0:
            fail(f"{cue['id']}: empty clip {clip}")
        rows.append((start, seconds, clip, cue["text"], cue["id"]))
    for (start, seconds, _, _, cue_id), (next_start, _, _, _, next_id) in zip(rows, rows[1:]):
        if next_start <= start:
            fail(f"{next_id} does not start after {cue_id}")
        if start + seconds > next_start:
            fail(f"{cue_id} ({start:.2f} s + {seconds:.2f} s) runs into {next_id} ({next_start:.2f} s)")

    target = pack / LANGUAGE / movie
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    lines = [f"# Audio description for movie/{movie}.thp: each cue's clip starts at `start` seconds on the movie's",
             "# timeline. `text` is what the clip says.", ""]
    for number, (start, seconds, clip, text, _) in enumerate(rows, 1):
        name = f"cue{number:03d}.wav"
        shutil.copyfile(clip, target / name)
        lines += ["[[cue]]", f"start = {start:.3f}", f"clip = {toml_string(name)}", f"text = {toml_string(text)}", ""]
    (target / "cues.toml").write_text("\n".join(lines), encoding="utf-8")
    written = tomllib.loads((target / "cues.toml").read_text(encoding="utf-8"))["cue"]
    expected = [(round(start, 3), f"cue{number:03d}.wav", text)
                for number, (start, _, _, text, _) in enumerate(rows, 1)]
    if [(c["start"], c["clip"], c["text"]) for c in written] != expected:
        fail(f"{target / 'cues.toml'} does not read back as written")
    print(f"{target}: {len(rows)} cues, {sum(r[1] for r in rows):.1f} s of description")


if __name__ == "__main__":
    main()
