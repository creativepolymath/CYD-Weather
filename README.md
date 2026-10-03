# CYD-Weather

`curl wttr.in` on a Cheap Yellow Display (ESP32-2432S028R, 320×240 ILI9341 + XPT2046 touch).

The ESP32 fetches `http://wttr.in/<location>?0` with a `curl` User-Agent, so wttr.in replies with
the same ANSI-coloured terminal text you see in a shell. The firmware parses the ANSI colours and
UTF-8, then draws it in a scaled-up 5×7 pixel font. The few Unicode symbols wttr.in uses (wind
arrows `↑↗→`, `°`, rain ticks `‘`, sun rays `―`, fog `‾`, and lightning `⚡`) are drawn by hand.

```
┌────────────────────────────────┐
│           Reykjavik            │  location bar
│             .-.                │
│            (   ).              │  ASCII-art icon, 2× size, in wttr.in colours
│           (___(__)             │
│           ‘ ‘ ‘ ‘              │
│          ‘ ‘ ‘ ‘               │
│          Light rain            │
│         +51(46) °F             │  3× size
│           ↑ 18 mph             │
│        6 mi   0.1 in           │
│ updated 3 min ago          1/3 │
└────────────────────────────────┘
```

If wttr.in sends anything else, like "Unknown location" or a rate-limit notice, the reply is shown
verbatim in the small font.

## Setup

1. Install [PlatformIO](https://platformio.org/) (the VS Code extension is easiest).
2. Edit `include/secrets.h` (copied from `secrets.example.h`): Wi-Fi details, locations, units and refresh interval.
3. Build and flash:

```bash
pio run -t upload
```

4. Optional: watch the serial log. It prints each request and the raw wttr.in reply.

```bash
pio device monitor
```

## Controls

| Input | Action |
|---|---|
| Tap screen | Next location (or refresh if only one is configured) |
| BOOT button | Refresh now |
| — | Auto-refresh every `REFRESH_MINUTES` (default 15) |

## Troubleshooting

- **Colours are wrong or inverted, or the screen stays blank:** you probably have the "2 USB" CYD variant. Flash `pio run -e cyd2usb -t upload` instead.
- **Picture is upside down:** change `tft.setRotation(1)` to `3` in `src/main.cpp`.
- All display pins are set as `build_flags` in `platformio.ini`, so you don't need to edit TFT_eSPI's `User_Setup.h`.
