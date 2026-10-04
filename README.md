# DukesOfDuchesca-napoli97

A [PRG32](https://github.com/riscv-prg32/PRG32) cartridge: a top-down
car-chase game freely inspired by *The Dukes of Hazzard*, relocated to
Napoli in the summer of 1997.

You drive a pimped-up white Fiat 500 across a city many times larger than the
screen, chased by scooter gangs who want to steal your ride and police cars
who are curious about it. Collect the eight things no party can do without —
beer, wine, Sangria Papelis, an amplifier, loudspeakers, a mixer, disco
lights and nice girls — keep an eye on the fuel gauge, and reach the party
villa at Posillipo before the night is over.

![screens](release-artifacts/contact-sheet.png)

## Controls

| Input            | Action                                             |
|------------------|----------------------------------------------------|
| D-Pad UP         | Accelerate                                         |
| D-Pad DOWN       | Brake / reverse                                    |
| D-Pad LEFT/RIGHT | Steer                                              |
| A                | Horn: scooters nearby turn tail (1.5 s to recharge) |
| START            | Start, pause                                       |

The yellow arrow orbiting the car points at the nearest item still missing;
it turns magenta and points at the villa once the boot is full. The radar in
the corner shows the whole gulf: items blink yellow, gas stations are orange,
the villa is magenta. Three brushes with a scooter or the police and the
night is over; so is an empty tank. Chasers that cannot catch you in twenty
seconds give up.

## The city

The map is a stylised Napoli laid out after the overview plate of a city
street atlas — drawn from memory of it, not copied and not to scale:

- **the coast of the gulf**: the bay of Bagnoli, the Posillipo promontory
  (the party villa, Parco Virgiliano at the cape), Mergellina and Via
  Caracciolo with the Villa Comunale, the bump of Santa Lucia with Castel
  dell'Ovo on its islet, Molo Beverello and the port, and the shore falling
  away south-east towards San Giovanni; a lungomare follows all of it;
- **the districts**, west to east, each with its own block size: Fuorigrotta
  (the Stadio San Paolo), Posillipo, the Vomero (Piazza Vanvitelli) and
  Chiaia (Piazza dei Martiri), the tight Greek grid of the centro storico
  with three decumani where the rest of the city has one (Piazza Dante,
  Piazza Bellini, Largo Corpo di Napoli, Piazza del Plebiscito), the
  diagonal Rettifilo down from Piazza Garibaldi, Piazza Mercato, and the
  grey sheds of the industrial east;
- **the north**: the Camaldoli hill, the woods and the Reggia of Capodimonte,
  the runway of Capodichino.

It is 220x130 tiles (1760x1040 px, 29 screens) and stores no tile array:
[`citymap.c`](citymap.c) computes every tile on demand from a coastline of 14
corner points, 29 named rectangles and modulo arithmetic.

## How it uses the PRG32 firmware

- **Indexed colours.** On the ESP32-C6 the framebuffer is one byte per pixel
  and the firmware maps every RGB565 colour to a cell of its 6x6x6 cube. The
  city is drawn with `prg32_gfx_rect_indexed()` through the 32 palette
  entries the cube never uses, and every sprite colour is written into the
  entry of its own cell, so the board shows the authored colours exactly as
  QEMU does. The host harness checks this on every frame.
- **No tile engine.** v1 used the firmware's playfields, which fill every 8x8
  tile with RGB565 rectangles, quantised pixel by pixel on the board. v2
  draws one indexed rectangle per run of equal ground (a `memset` on the
  board) plus small details: roofs lit from the north-west, windows,
  trees, the parapet of the lungomare.
- **Palette effects.** The picture is recoloured by rewriting palette
  entries: golden hour turning into night over two minutes, windows lighting
  up group by group, the shimmering gulf, the villa's dance floor and string
  lights, police light bars, a white flash and a screen shake on a crash,
  fades between screens, a banded sunset on the title.
- **Sprites.** The vehicles are 4-bpp indexed sprites at 8 headings and the
  villa a 4-bpp picture (`prg32_sprite_draw_indexed`); item icons and compass
  arrows are 1-bpp indexed sprites with a transparent background.
- **SID-like stereo audio.** Eleven procedural instruments and four original
  tracks (a serenade on the title, a 6/8 tarantella while driving, a 90s
  dance loop at the villa, a lament when it goes wrong) on tracker voices
  0-4. Voices 5-7 are the game's: an engine note that follows the speed, the
  horn, pick-ups, crashes, and a two-tone siren panned to where the police
  car is. The tarantella speeds up while someone is on your tail
  (`prg32_audio_set_tempo`).
- **Fixed 33 ms simulation steps**, so the pace holds when the board draws
  slower than 30 fps, and a press between two steps is never lost.
- **Scores** go to the firmware scoreboard
  (`prg32_score_submit_current_player`).

The cartridge needs 24 KiB of cartridge RAM (the default profile is 64 KiB)
and packs to 32 KiB including the soundtrack, icon and screenshot.

## Project layout

```
game.c                     cartridge entry points: dukes_init/update/draw
citymap.h / citymap.c      pure map logic, no prg32.h dependency
assets.h                   generated sprites, icons and palettes
audio.json                 generated instruments and tracker events
metadata.json, colophon.json, icon.png, screenshot.png   Store metadata
scripts/gen_assets.py      regenerates assets.h
scripts/gen_music.py       regenerates audio.json
scripts/render_screens.py  renders the real screens on the host; Store media
scripts/qemu_capture.py    runs the cartridge in the QEMU firmware
scripts/store_manifest.py, scripts/check_store_bundle.py   Store bundle
tests/test_citymap.c       map unit tests (reachability by flood fill)
tests/prg32_stub.c         host model of both PRG32 display back ends
tests/host_harness.c       autopilot, traffic and fuzz runs of the real game
test.sh, build.sh          host checks; cartridge and Store bundle
dist/                      the released Store bundle (tracked)
release-artifacts/         screens from the host model and from QEMU
```

## Testing

```sh
PRG32_REPO=/path/to/PRG32 ./test.sh
```

- `tests/test_citymap.c` flood-fills every position the car's collision box
  can occupy and asserts that every item, gas station, the villa and every
  open tile is connected to the start, and that the coast has the shape of
  the gulf.
- `tests/host_harness.c` runs the real `game.c`: an autopilot wins the game
  on empty streets (all eight items, refuelling on the way), plays twelve
  nights in traffic, then 60,000 fuzzed frames. Every frame asserts that the
  car and the chasers never overlap solid ground and that the QEMU and
  ESP32-C6 display models show the same picture, pixel for pixel. The build
  uses AddressSanitizer and UBSan.
- `game.c` is compiled with `-Wall -Wextra -Werror` against the real
  `prg32.h`.
- `scripts/qemu_capture.py` boots the built cartridge in the QEMU firmware,
  starts a game, drives, and saves real frames, the mixer's output and the
  console log to `release-artifacts/qemu/`.

It has not been run on an ESP32-C6 board: do that before trusting the frame
rate there.

## Building

Requires a checkout of [riscv-prg32/PRG32](https://github.com/riscv-prg32/PRG32)
`main` and the RISC-V toolchain installed by ESP-IDF:

```sh
PRG32_REPO=/path/to/PRG32 ./build.sh
```

This runs the tests, builds the portable cartridge, attaches the Store
metadata for `esp32c6` and `qemu`, packs
`dist/dukesofduchesca-napoli97-<version>-store.zip` and, when a
[CartridgeStore](https://github.com/riscv-prg32/CartridgeStore) checkout is
next to this one (or `CARTRIDGE_STORE_ROOT` is set), validates the bundle
with the Store's own intake code.

To play it in QEMU, from the PRG32 checkout:

```sh
python3 -m prg32 qemu build
python3 -m prg32 qemu upload /path/to/dist/store/dukesofduchesca-napoli97-qemu.prg32
python3 -m prg32 qemu start
```

## License

MIT, see [LICENSE](LICENSE). Freely inspired by a television series it is not
affiliated with; code, art and music are original.
