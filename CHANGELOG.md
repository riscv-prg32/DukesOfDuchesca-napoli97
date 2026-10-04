# Changelog

## 2.3.0 — 2026-10-05 — the party is on the beach of the Gaiola; traffic

### New

- **Traffic.** Other cars (the Fiat's shape in red, blue, green and grey
  paint) and the orange city buses drive the streets, mostly straight on.
  They are solid: the Fiat has to go round them, and the steering nudge takes
  it into the free lane by itself. They never drive into the Fiat: they wait,
  and turn back if held up too long. Scooters and police weave through them.

### Changed

- **The ending point is the beach of the Gaiola**, in its cove under Capo
  Posillipo, with the two islets, the villa and the little bridge standing in
  the sea off it, the white house and the boats on the sand. It replaces the
  villa as the place to bring the gear to.
- **The game closes with people dancing on the beach**: a night view of the
  cove with the islets, the moon on the water, rockets, a string of lights,
  the loudspeakers thumping, the Fiat parked on the sand and fifteen dancers
  round the fire.
- Villa Doria d'Angri stays, as a monument on the hill of Posillipo.
  Twenty-six points of interest in all.
- The villa picture is gone from the cartridge, which pays for the ending,
  the islets and the traffic: the package is about the size of 2.2.0's. The
  buses are drawn from rectangles and the cars reuse the Fiat's sprite, so
  traffic adds no picture data.
- The steering nudge reaches one pixel further (eight), enough to change lane.

## 2.2.0 — 2026-10-04 — the Rock Garden, and a police force you have to drive past

### New

- **The start is the Rock Garden**, the rock club of Via San Giovanni
  Maggiore Pignatelli 26, placed in the vicoli beside the Rettifilo and drawn
  under its neon sign. La Duchesca is named on the map, between it and the
  station. Twenty-four points of interest in all.
- **Checkpoints.** Seven posti di blocco. Crossed over the limit, the Fiat is
  halted and searched; rolled through, it is waved on.
- **Searches.** Halted for a few seconds; the rauti are confiscated three
  times out of four and each party item one time in four (two at most).
  Confiscated items go back to where they were found.
- A speedometer under the fuel gauge, red above the limit; police and
  checkpoints on the radar.

### Changed

- **The police are on your side until you give them a reason.** Patrol cars
  cruise instead of hunting the Fiat, there are more of them (up to three
  around, from the first minute), and scooters run from a patrol nearby.
  Speeding in sight of a patrol, or lighting a rauto within its earshot,
  starts a chase that ends in a search. The police no longer damage the car
  and can no longer end the game.
- A rauto no longer removes police cars.

## 2.1.0 — 2026-10-04 — zoom, rauti, the monuments of Napoli, easier steering

### New

- **Zoom.** The world is drawn at two screen pixels per world pixel. The Fiat,
  the scooters and the police cars are 40x40 sprites redrawn at that size
  (not pixel-doubled); items, pumps and the compass arrow are 16x16.
- **Rauti.** Seven boxes of twenty bangers around the city. B lights one and
  leaves it on the road; it goes off two seconds later, takes out the
  chasers within 30 pixels and damages the Fiat if it is still there.
- **Points of interest.** Twenty-two real places, named on screen as the car
  reaches them, the monuments drawn each in its own shape: Piazza del
  Plebiscito, San Francesco di Paola, Palazzo Reale, Maschio Angioino, Castel
  dell'Ovo, Castel Sant'Elmo, Stadio San Paolo, Duomo, Museo Nazionale,
  Reggia di Capodimonte, Stazione Centrale, Galleria Umberto I, Centro
  Direzionale, Mostra d'Oltremare, Capodichino, Villa Comunale, Molo
  Beverello and four more piazzas. The party villa is Villa Doria d'Angri.
  `release-artifacts/map.png` shows them all.

### Changed

- **Steering.** The D-pad now points where the car should go; the car turns
  that way and accelerates by itself, brakes through a U-turn, and is nudged
  into a street opening it would otherwise have clipped. There is no reverse
  any more: it is not needed. v2.0 steered like a wheel (LEFT/RIGHT relative
  to the car, UP to accelerate), which was hard on a D-pad in a grid of
  streets.
- Top speed is a little lower and chasers appear and give up closer to the
  car, to suit the smaller field of view.
- The message line moved up; the bottom-left corner shows where you are.

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
