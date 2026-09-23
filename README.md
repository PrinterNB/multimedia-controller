# ESP32-S3 USB-HID Media Controller

A custom USB-HID media controller for an ESP32-S3 DevKitC-1:

- **EC11 rotary encoder** (smooth-mod, push contact not used) → volume up/down
- **Kailh Choc switch #1** → Play/Pause
- **Kailh Choc switch #2** → Mute
- **Switches #3–#5** → configurable from the device web UI

The firmware includes a Wi-Fi setup and configuration UI. It stores settings
in flash, so button and dial mappings survive reboot.

Firmware is `src/main.cpp` (Arduino core). The device enumerates on the
DevKitC-1's **native USB-C port** as a HID consumer-control device. Flashing
and serial logging stay on the second **UART-USB port** (CP210x), so a
plugged-in HID device never blocks firmware updates.

## Wiring diagram

Every input is wired **GPIO → switch → GND**; the ESP32's internal pull-up
does the rest (no external resistors needed). The board has only **four GND
pins** (L22, R1, R21, R22) but there are six things to ground — so all
ground-side terminals join one **GND bus** (a breadboard row or a solder
blob), and that bus runs to the board with a single wire (W14).

### Board side — ESP32-S3 DevKitC-1 (USB-C port pointing up)

Row numbers L1–L22 / R1–R22 count **from the top** of each header, matching
the silkscreen labels printed next to each pin.

```
   row   LEFT header                    RIGHT header
   ────   ──────────────────────         ──────────────────────
    1     3V3                            GND     ◄── W14 (from GND bus)
    2     3V3                            GPIO43
    3     RST                            GPIO44
    4     GPIO4  ◄── W1  (encoder A)     GPIO1   ◄── W8  (SW3)
    5     GPIO5  ◄── W2  (encoder B)     GPIO2   ◄── W10 (SW4)
    6     GPIO6  ◄── W4  (Play/Pause)    GPIO42
    7     GPIO7  ◄── W6  (Mute)          GPIO41
    8     GPIO15 ◄── W12 (SW5)           GPIO40
    …    … (rows 9–20 not used)          … (rows 9–20 not used) …
   21     5V0                            GND
   22     GND                            GND
```

### Component side

```
   EC11 rotary encoder — the THREE-pin side only (the two-pin side is the
   push switch: LEAVE IT COMPLETELY UNCONNECTED)
     middle pin (COM)  ── W3 ──►  GND bus
     left pin  (A)     ── W1 ──►  left header, row 4  (GPIO4)
     right pin (B)     ── W2 ──►  left header, row 5  (GPIO5)
     (A/B order doesn't matter — flipping it only reverses CW/CCW,
      and ENC_DIR in the firmware can flip it too)

   Kailh Choc — PLAY/PAUSE  (2-pin, unmarked — either pin can be W4)
     pin A ──► W4 ──►  left header, row 6  (GPIO6)
     pin B ──► W5 ──►  GND bus

   Kailh Choc — MUTE
     pin A ──► W6 ──►  left header, row 7  (GPIO7)
     pin B ──► W7 ──►  GND bus

   SW3 (reserved)     SW4 (reserved)     SW5 (reserved)
     pin A ──► W8  ──► right header, row 4  (GPIO1)
     pin B ──► W9  ──► GND bus

     pin A ──► W10 ──► right header, row 5  (GPIO2)
     pin B ──► W11 ──► GND bus

     pin A ──► W12 ──► left header, row 8   (GPIO15)
     pin B ──► W13 ──► GND bus

   GND bus   (W3, W5, W7, W9, W11, W13 all land here)
     W14 ──► right header, row 1  (GND)
```

### Wire list

| Wire | From (component pin)            | To (board pin)                          |
|------|---------------------------------|-----------------------------------------|
| W1   | EC11, 3-pin side, left pin      | GPIO4 — left header, row 4              |
| W2   | EC11, 3-pin side, right pin     | GPIO5 — left header, row 5              |
| W3   | EC11, 3-pin side, middle pin    | GND bus                                 |
| W4   | Play/Pause Choc, either pin     | GPIO6 — left header, row 6              |
| W5   | Play/Pause Choc, other pin      | GND bus                                 |
| W6   | Mute Choc, either pin           | GPIO7 — left header, row 7              |
| W7   | Mute Choc, other pin            | GND bus                                 |
| W8   | SW3, either pin                 | GPIO1 — right header, row 4             |
| W9   | SW3, other pin                  | GND bus                                 |
| W10  | SW4, either pin                 | GPIO2 — right header, row 5             |
| W11  | SW4, other pin                  | GND bus                                 |
| W12  | SW5, either pin                 | GPIO15 — left header, row 8             |
| W13  | SW5, other pin                  | GND bus                                 |
| W14  | GND bus                         | GND — right header, row 1 (R21/R22/L22 identical) |

**Pin ID tips**

- **EC11**: the side with **three** pins is the encoder — middle pin is the
  common (COM → GND bus), the two outer pins are channels A/B (→ GPIO4/5,
  order doesn't matter). The side with **two** pins is the push switch:
  leave it unconnected. If in doubt, confirm COM with a multimeter: the
  middle pin reads 0 Ω briefly to each outer pin in turn as you rotate, and
  the two outer pins never read 0 Ω to each other.
- **Kailh Choc**: both pins are identical (it's just a momentary switch), so
  either one can carry the GPIO wire.
- The three reserved switches are polled and debounced in firmware today;
  assigning them a job later is a one-line change to the `g_btns[]` table in
  `src/main.cpp` (set their `Action`).

## Build & flash

1. PlatformIO with the `espressif32` platform (Arduino-ESP32 ≥ 2.0.14).
2. Connect the **UART-USB** port (the one with the CP210x chip), press RST,
   hold BOOT if it doesn't auto-enter download mode.
3. `pio run` / click Build, then Upload.
4. Open the monitor at 115200 on the UART port for boot logs.

### First Wi-Fi setup

After the first flash, the controller creates a temporary Wi-Fi network:

1. Join `MediaCtrl-XXXX` with password `configureme`.
2. Open `http://192.168.4.1` in a browser.
3. Enter the 2.4 GHz Wi-Fi name, password, and a hostname such as
  `desk-controller`.
4. Select **Save all settings**. The setup network closes while the device
  joins your Wi-Fi.

On the local network, open `http://desk-controller.local` (or the IP shown in
the UI). If the saved network cannot be reached for 12 seconds, the
controller starts its setup network again so it remains discoverable and
configurable.

The AP password is currently `configureme`; change it in `src/main.cpp` if
the controller will be used in an untrusted environment. The configuration
page uses plain HTTP, so only configure Wi-Fi on a trusted local network.

### Button and dial mappings

Each button can send nothing, a media action (play/pause, mute, next,
previous, or stop), a keyboard key, a key plus modifiers, or a macro. Macro
steps use `KEY:delay-ms` separated by commas, for example:

```text
CTRL+SHIFT+S:0,A:80,ENTER:120
```

The dial supports volume, next/previous track, scrolling, or any keyboard
key. Its direction, steps per detent, acceleration, and minimum output
interval are configurable. This allows uses such as timeline scrubbing,
zoom, brush size, or app-specific shortcut navigation in addition to volume.

Plug the **native USB-C** port in afterwards (or before — order doesn't
matter) and Windows/macOS should enumerate a consumer-control HID with no
drivers.

> First connect: if Windows shows an unknown device, unplug/replug once —
> the HID device sometimes needs a second enumeration pass.

## PC-side macro launcher (staged, no trigger wired yet)

`pc/media-launcher.ahk` (AutoHotkey **v2**) can catch the keyboard **F13**
action and launch the executable in `TargetExe`. Map any button to keyboard
key `F13` in the web UI, edit `TargetExe`, and run the script.

F13 is reserved for this device — don't remap it in other apps.

## Controls

| Input            | Sends                                        |
|------------------|----------------------------------------------|
| Encoder CW/CCW   | Consumer Vol Up (0x00E9) / Vol Down (0x00EA) — 1 step/count, rate-limited while spinning |
| Play/Pause       | Consumer 0x00CD, held for as long as the button is held |
| Mute             | Consumer 0x00E2, held for as long as the button is held |
| Switches #3–#5   | Configurable keyboard/media/macro actions      |

## Tunables (top of `src/main.cpp`)

| Macro             | Default | Meaning                                        |
|-------------------|---------|------------------------------------------------|
| `ENC_MIN_EDGE_US` | 300     | noise gate between encoder edges (µs)          |
| `ENC_DIR`         | 1       | set `-1` to flip CW/CCW                        |
| `BTN_DEBOUNCE_MS` | 20      | two-sample switch debounce                     |
| `VOL_STEP_MS`     | 80      | legacy documentation entry; use the web UI     |

## Edge cases handled

- **USB suspend/resume & replug**: the TinyUSB stack re-enumerates; on
  resume the firmware re-sends all-releases so no key stays logically down.
- **Encoder noise / overspeed**: 4x quadrature with invalid-state rejection
  + inter-edge gate; counts accumulate in the ISR so fast spins never lose
  steps (only the *output* is rate-limited).
- **Button bounce**: two-sample 20 ms debounce → exactly one event per make/break.
