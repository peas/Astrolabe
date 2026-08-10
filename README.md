# Astrolabe

[![CI](https://github.com/Khronos31/Astrolabe/actions/workflows/ci.yml/badge.svg)](https://github.com/Khronos31/Astrolabe/actions/workflows/ci.yml)

Turn an [M5Dial](https://docs.m5stack.com/en/core/M5Dial) into a physical
Home Assistant controller: a ring of icons you turn through with the knob,
tap to open, and turn again to dim.

Astrolabe is an ESPHome [external component](https://esphome.io/components/external_components.html).
Your slots are declared in YAML and compiled into the firmware, so the device
draws its whole face before it has talked to anything, and there is no custom
Home Assistant integration to install.

**This release handles `light` entities.** Other domains are planned; see
[Scope](#scope).

## Requirements

- An M5Dial (ESP32-S3, 8 MB flash)
- ESPHome 2025.11.0 or newer — the ESPHome Device Builder add-on is fine
- Home Assistant, reachable over your network

This repository is source-only. You build the firmware yourself; no prebuilt
images are published, and your Wi-Fi and API credentials never leave your
machine.

### About versions

Three different claims, which are worth keeping apart.

**It will not build on anything older than 2025.11.0.** `min_version` enforces
that.

**It builds on the current ESPHome.** There is no way to declare an upper bound,
so a future release could break this. CI builds the example against whatever
ESPHome is current, on every push and once a week; the badge above is that
result, and a red badge means a release has broken something.

**It was last run on hardware with ESPHome 2025.11.0.** Only somebody holding an
M5Dial can make that claim, so it is not checked by CI and it will go stale. If
that version looks old to you, treat newer ESPHome releases as building but
unproven on a real device.

Two dependencies are pinned exactly and fetched at build time: LovyanGFX 1.2.26
and the Material Design Icons webfont 7.4.47. ESP-IDF is whatever your ESPHome
release selects and is not pinned here.

## Quick start

1. Copy the YAML below into your ESPHome configuration directory as
   `astrolabe.yaml`.
2. Put `wifi_ssid`, `wifi_password`, `api_encryption_key` and `ota_password`
   in your `secrets.yaml`.
3. Replace the entries under `slots:` with your own light entities.
4. Build and install. The first install needs USB; after that, over the air.
5. Add the discovered device to Home Assistant.
6. **Open the ESPHome integration's options for the device and enable
   "Allow the device to perform Home Assistant actions".**

Step 6 is not optional and is easy to miss. New ESPHome devices have it off by
default. Until you turn it on the device looks completely healthy — the ring
appears, the knob turns, the screen reacts — but nothing you do reaches Home
Assistant, and the device is never told why. Home Assistant does say so, under
**Settings → System → Repairs**.

## The YAML

```yaml
# Astrolabe — turn an M5Dial into a Home Assistant controller.
#
# 1. Copy this file into your ESPHome configuration directory.
# 2. Put wifi_ssid, wifi_password, api_encryption_key and ota_password in secrets.yaml.
# 3. Replace the entries under `slots:` with your own entities.
# 4. Build and install with ESPHome.
# 5. After adding the device to Home Assistant, open the ESPHome integration's
#    options for it and enable "Allow the device to perform Home Assistant actions".
#    Without this the screen looks fine but nothing happens when you use it, and
#    the device is not told why. Home Assistant reports it under Repairs.

substitutions:
  name: astrolabe
  friendly_name: Astrolabe

packages:
  astrolabe:
    url: https://github.com/Khronos31/Astrolabe
    ref: v0.1.0
    file: packages/m5dial.yaml
    # Pinned to a tag, so there is nothing to re-fetch. Keep `never`.
    refresh: never

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  # This device is mains-powered and always on, so there is nothing to save.
  # With the default (WIFI_PS_MIN_MODEM) the radio sleeps between DTIM beacons
  # and the dial feels noticeably sluggish: round-trip median 125-180 ms and
  # peaks over 2 s, against 8 ms and 51 ms with power saving off.
  # Change this if you build a battery-powered variant.
  power_save_mode: none

api:
  # homeassistant_states and homeassistant_services are enabled by the package.
  encryption:
    key: !secret api_encryption_key

ota:
  - platform: esphome
    password: !secret ota_password

astrolabe_ui:
  # Whether to beep on input. Set to false if the device sits by your bed.
  sound: true

  # The order here is the order around the ring. Ten slots maximum;
  # more than that fails at build time rather than overlapping on screen.
  slots:
    - type: light
      entity_id: light.living_room
      tag_up: "LIVING"
      tag_down: "LIGHT"
      icon: mdi:lightbulb
    - type: light
      entity_id: light.bedroom
      tag_up: "BED"
      tag_down: "ROOM"
      icon: mdi:bed

# ── Optional: diagnostics ──
# Add one more entry under `packages:` to expose heap, loop time, touch health
# and verbose logs. Leave it out for normal use; the verbose log strings are
# removed at compile time, so this is smaller as well as quieter.
#
#     astrolabe_diagnostics:
#       url: https://github.com/Khronos31/Astrolabe
#       ref: v0.1.0
#       file: packages/diagnostics.yaml
#       refresh: never
```

## How it works

Three screens.

| | |
|---|---|
| **Launcher** | The ring. Turn the knob to move between slots; the selected icon grows. Press the knob, or tap the middle of the screen, to open it. |
| **App** | For a light: an arc showing brightness, and ON/OFF. Turn to dim, tap to toggle, press the knob to go back. |
| **Clock** | After twenty seconds of no input. Turn or tap to wake into the first slot; press the knob for the launcher. |

Only the middle of the launcher opens a slot, so brushing an icon on the ring
does nothing.

Sounds tell you what the device thinks you did, without looking at it:

| Input | |
|---|---|
| Knob, clockwise / anticlockwise | 6 kHz / 7 kHz — different, so you can hear which way you turned |
| Knob pressed, or a tap that opens something | 2 kHz |
| Something changed (toggled, woken) | 4 kHz |

Set `sound: false` if the device sits somewhere you would rather it stayed
quiet.

## Configuration

### `astrolabe_ui`

| Option | | |
|---|---|---|
| `slots` | **required** | One to ten entries. The order is the order around the ring. |
| `sound` | `true` | Beep on input. |

Eleven or more slots, or a slot whose `type` and `entity_id` disagree, fails
the build rather than producing a face you cannot read.

### Slots

| Option | | |
|---|---|---|
| `type` | **required** | `light` |
| `entity_id` | **required** | Must match `type` — a `light` slot needs a `light.` entity. |
| `tag_up` | **required** | Upper line of the label shown in the middle. |
| `tag_down` | `""` | Lower line. |
| `icon` | per type | Any [Material Design Icons](https://pictogrammers.com/library/mdi/) name, as `mdi:name`. |
| `icon_bg` / `icon_fg` | `#9E9E9E` / `#141414` | Icon colours, as `#RRGGBB`. |

Icons are rendered at build time from the Material Design Icons webfont,
pinned to version 7.4.47 and fetched once, then cached. Any of the 7,447 icons
in that release works; a name that does not exist fails the build.

### Diagnostics

Heap, loop time and touch-panel health, plus verbose logs. Off by default.
See [`packages/diagnostics.yaml`](packages/diagnostics.yaml) for how to add it
and what each value means.

## Scope

This release does `light`. The remaining domains from the device this grew out
of — `climate`, `cover`, `media_player`, and a `generic` slot whose gestures
call whatever entities you name — are planned, along with colour temperature
control for lights.

If you want to change something this does not expose — pin assignments, the
idle timeout, the geometry of the ring, the sounds — copy
[`packages/m5dial.yaml`](packages/m5dial.yaml) into your own repository and
point `packages:` at that. Distributing source rather than binaries is what
makes that cheap, and it is a better answer than growing an option for every
preference.

## Known issues

**Touch input can stop working after a few days idle.** The display, the knob
and the button keep working; only touch stops. Unplugging and replugging power
brings it back. This is unresolved. Adding the diagnostics package exposes
`Touch G_CTRL Bad Count`, which rises when it happens.

**The clock shows `--:--` until Home Assistant is reachable.** Time comes from
Home Assistant, and the device has no battery-backed clock. Everything else —
the ring, the icons, the labels, the knob, the beeper — works with no network
at all, because the slots are compiled into the firmware.

## Licence

MIT. See [LICENSE](LICENSE).

The menu, selector, viewport and animation code under `components/astrolabe_ui/`
(`smooth_menu`, `menu`, `selector`, `simple_menu`, `lv_anim`, `camera`) is from
smooth_menu, by Forairaaaaa. The display definition, pin map and touch driver
(`hal_display.hpp`, `hal_pins.h`, `hal_tp.hpp`) are derived from M5Dial-UserDemo.
Both are MIT; see [LICENSE](LICENSE) for the details and
[`smooth_menu.LICENSE`](components/astrolabe_ui/smooth_menu.LICENSE) for the
full smooth_menu text.

Two things are fetched at build time rather than included here:
[LovyanGFX](https://github.com/lovyan03/LovyanGFX) (MIT and BSD-2-Clause), and
the [Material Design Icons](https://pictogrammers.com/library/mdi/) webfont
(Pictogrammers Free License), pinned to 7.4.47.
