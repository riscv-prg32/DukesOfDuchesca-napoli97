# Changelog

## 2.0.0 — 2026-10-04 — a map of Napoli, indexed colours, palette effects, SID-like stereo

### Fixed

- **The game could not be won.** North-south streets were one tile (8 px)
  wide and the car's collision box 11 px, so the car could never leave the
  street it started on: one item of eight was reachable, the villa was not.
  Streets are now two and three tiles wide and the box exactly one tile; a
  flood fill in the unit tests proves every landmark reachable.
- The collision box was wider than a tile but only its corners were tested,
  so it could straddle a lone solid tile.
- The music was written with `delta` as the wait before an event; the
  firmware's tracker reads it as the wait after, so every note was shifted
  onto its neighbour's timing. The tracker also ignores instrument default
  volumes, so all voices played flat out. The score is regenerated for
  delta-after timing and sets each channel's volume and pan itself.
- Chasers drove in a straight line at the car and stuck on the first
  building. They now follow the streets.
- The horn deleted every scooter around with no cooldown.
- The car could not steer while standing still, so it could wedge itself in
  a corner.
- The pace depended on the frame rate; a button pressed between two frames
  could be lost.
- Colours on the ESP32-C6 were quantised to the firmware's 216-colour cube.
- `duke_item_icons` was a table of pointers in static data, which a portable
  (relocated) cartridge must not have.
- `manifest.json` pointed at files that were never built, and the screenshot
  was a mock-up. The Store bundle is now built, checked with the Store's
  intake code and tracked in `dist/`; the screenshot is a real frame.

### New

- **The city** is a stylised Napoli laid out after the overview plate of a
  street atlas: the coast of the gulf with the Posillipo promontory,
  Mergellina, Via Caracciolo, Castel dell'Ovo on its islet, Molo Beverello
  and the port; Fuorigrotta with the stadium, the Vomero, Chiaia, the Greek
  grid of the centro storico, the Rettifilo, Piazza Garibaldi, the
  industrial east; Capodimonte, the Camaldoli and Capodichino.
- Indexed rendering without the tile engine; roofs in four plaster colours
  lit from the north-west, courtyards, windows, the parapet of the lungomare.
- Palette effects: sunset to night, windows lighting up, shimmering sea,
  disco floor and string lights at the villa, police light bars, crash flash
  and screen shake, fades, a banded sunset on the title.
- Scooters and police that drive the street grid, cut the car off, give up
  after twenty seconds; scooters flee from the horn.
- A compass arrow, a radar of the gulf, messages, a score sent to the
  firmware scoreboard, particles (exhaust, sparks, pick-ups, confetti).
- Four original tracks and eleven SID-like instruments; an engine voice, a
  panned siren; the tarantella speeds up during a chase.
- Host model of both display back ends, an autopilot that wins the game,
  `scripts/qemu_capture.py`, `test.sh`.

### Changed

- `assets_generated.h` and `assets_icons.h` are one generated `assets.h`.
- `scripts/gen_marketing_assets.py` and `scripts/render_preview_wav.py` are
  replaced by `scripts/render_screens.py` and `scripts/qemu_capture.py`.
- `icon.png` and `screenshot.png` moved to the repository root.

## 1.0.0 — 2026-10-04

- First release: tile-engine city, 4-bpp vehicle sprites, one music track.
