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

**It was last run on hardware with ESPHome 2026.7.4.** Only somebody holding an
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
    ref: v0.2.1
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

    - type: climate
      entity_id: climate.bedroom
      tag_up: "AIR"
      # Long press cycles this list; nothing is sent until you tap or turn.
      # A mode your device does not have is shown greyed out, not sent, and
      # not removed from the list — the mistake stays visible.
      modes: [cool, heat]

    - type: cover
      entity_id: cover.living_curtain
      tag_up: "CURTAIN"
      # Which side the fabric gathers on: center, left or right. The knob
      # follows the fabric, so this also decides which way to turn.
      opening: center

    - type: media_player
      entity_id: media_player.living
      tag_up: "MEDIA"

    # A generic slot has no entity of its own; each gesture names one.
    # Anything not listed does nothing and stays silent.
    - type: generic
      tag_up: "GOOD"
      tag_down: "NIGHT"
      icon: mdi:weather-night
      tap: script.goodnight
      hold: script.bedroom_off

# ── Optional: diagnostics ──
# Add one more entry under `packages:` to expose heap, loop time, touch health,
# which build is running, and verbose logs. Leave it out for normal use; the
# verbose log strings are removed at compile time, so this is smaller as well
# as quieter.
#
#     astrolabe_diagnostics:
#       url: https://github.com/Khronos31/Astrolabe
#       ref: v0.2.1
#       file: packages/diagnostics.yaml
#       refresh: never
```

## How it works

Three screens.

| | |
|---|---|
| **Launcher** | The ring. Turn the knob to move between slots; the selected icon grows. Press the knob, or tap the middle of the screen, to open it. |
| **App** | For a light: an arc showing brightness, and ON/OFF. Turn to dim, tap to toggle, press the knob to go back. Hold the middle for half a second to switch between brightness and colour temperature. |
| **Clock** | After twenty seconds of no input. Turn or tap to wake into the first slot; press the knob for the launcher. |

Only the middle of the launcher opens a slot, so brushing an icon on the ring
does nothing. Inside an app the touch target is the middle too, so resting a
hand on the rim of the dial cannot switch anything.

### Colour temperature

Holding the middle of a light's screen switches between brightness (`DIM`) and
colour temperature (`CLR`). The switch happens the moment you cross half a
second, not when you let go.

The range and the step come from the light itself: Home Assistant reports
`min_color_temp_kelvin` and `max_color_temp_kelvin`, and Astrolabe turns in
200 K steps between them. Nothing about colour temperature is configured here.

**If a light cannot do colour temperature, holding does nothing and stays
silent, and no `DIM | CLR` marker appears.** That is deliberate: a control you
cannot reach should not look reachable. It follows the light's current
capabilities, so swapping the bulb for one that cannot do colour temperature
takes the mode away without a rebuild.

Colour temperature can only be set by turning a light on, so turning the knob
in `CLR` while the light is off will switch it on.

Some lights report a colour temperature outside their own stated range. One
ceiling light here reports `65280` every time it is switched on. Astrolabe
shows the nearest end of the range, because that is usually what the light has
actually done — but it will not send that number back until you have turned
the knob yourself. Showing a guess is fine; acting on one is not.

If there is no value at all, the screen shows `----K` and draws no arc.

Sounds tell you what the device thinks you did, without looking at it:

| Input | |
|---|---|
| Knob, clockwise / anticlockwise | 6 kHz / 7 kHz — different, so you can hear which way you turned |
| Knob pressed, or a tap that opens something | 2 kHz |
| Something changed (toggled, woken) | 4 kHz |
| Switched between brightness and colour temperature | 3 kHz, slightly longer — the screen now means something different |

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
| `type` | **required** | `light`, `climate`, `cover`, `media_player` or `generic`. |
| `entity_id` | **required** except `generic` | Must match `type` — a `light` slot needs a `light.` entity. |
| `tag_up` | **required** | Upper line of the label shown in the middle. |
| `tag_down` | `""` | Lower line. |
| `icon` | per type | Any [Material Design Icons](https://pictogrammers.com/library/mdi/) name, as `mdi:name`. |
| `icon_bg` / `icon_fg` | `#9E9E9E` / `#141414` | Icon colours, as `#RRGGBB`. |

#### What each type does

| Type | Knob | Tap | Long press |
|---|---|---|---|
| `light` | Brightness, or colour temperature | On/off | Switch the knob between the two |
| `climate` | Target temperature | Send the mode shown, or turn off if it is already running | Show the next mode. **Nothing is sent** |
| `cover` | Target position | Close if it is more than half open, otherwise open | Stop |
| `media_player` | Volume, or the selection on the action ring | Play/pause, or run the selected action | Switch between volume and the ring |
| `generic` | `rotate_right` / `rotate_left` | `tap` | `hold` |

Anything the device cannot currently do is refused in silence and is not
offered on screen. That is not the same as a device that is merely off: a
media player exposes almost nothing while idle and gains it back when
something is playing, so the ring dims and lights up as you use it.

#### Extra options per type

| Type | Option | | |
|---|---|---|---|
| `climate` | `modes` | `[]` | Modes to cycle with a long press, in order — any of `off`, `heat`, `cool`, `heat_cool`, `auto`, `dry`, `fan_only`. Leave it out and long press does nothing. |
| `cover` | `opening` | `center` | Which side the fabric gathers on: `center`, `left` or `right`. |
| `generic` | `tap` `hold` `rotate_right` `rotate_left` | — | An entity to act on. At least one is required. |

`modes` lists what you want to reach, not what the device has. A mode it does
not support is drawn greyed out and never sent — the list is not silently
shortened, so a mistake stays visible instead of disappearing.

A `generic` slot works out the right service from the entity's domain, because
"press this" means different things: `script.turn_on` runs a script where
`script.toggle` would stop one that is already running, and `automation.trigger`
runs an automation where `automation.toggle` would enable or disable it.
Domains where pressing has no clear meaning fail the build.

Icons are rendered at build time from the Material Design Icons webfont,
pinned to version 7.4.47 and fetched once, then cached. Any of the 7,447 icons
in that release works; a name that does not exist fails the build.

### Diagnostics

Heap, loop time, touch-panel health, which build is running and why it last
restarted, plus verbose logs. Off by default. See
[`packages/diagnostics.yaml`](packages/diagnostics.yaml) for how to add it and
what each value means.

`Build` and `Slots` are worth knowing about before you need them: the boot
banner that would answer "which firmware is this, and how many slots does it
have?" is written before a log viewer can attach, so it is gone by the time you
go looking.

## Scope

This release does `light`, `climate`, `cover`, `media_player` and `generic`.

It deliberately does not show text that comes from Home Assistant — no track
titles, no fan speed names. Rendering arbitrary text well would mean shipping a
font, and a source-only project is a poor place to carry one. What is on screen
instead is what the dial can state without help: a number, a position, a
colour, an icon that is either lit or not.

Nothing here reads a value the device has not reported. A knob does not move
until Home Assistant has said what it is moving from, and a value outside the
range the device itself gave is shown rounded to the edge but never sent back.
The screen will sometimes say it does not know; that is the honest answer, and
it is a different answer from zero.

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
