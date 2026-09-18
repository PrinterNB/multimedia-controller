# ESP32-S3 USB-HID Media Controller

A custom USB-HID media controller for an ESP32-S3 DevKitC-1:

- **EC11 rotary encoder** (smooth-mod, push contact not used) → volume up/down
- **Kailh Choc switch #1** → Play/Pause
- **Kailh Choc switch #2** → Mute
- **Switches #3–#5** → wired and debounced, but no action yet (reserved)

Firmware is `src/main.cpp` (Arduino core). The device enumerates on the
DevKitC-1's **native USB-C port** as a HID consumer-control device. Flashing
and serial logging stay on the second **UART-USB port** (CP210x), so a
plugged-in HID device never blocks firmware updates.

## Wiring (all inputs: GPIO → switch → GND, internal pull-ups)

| Function          | GPIO   | Notes                                  |
|-------------------|--------|----------------------------------------|
| Encoder CH A      | GPIO4  | encoder pin 1 (A)                       |
| Encoder CH B      | GPIO5  | encoder pin 2 (B)                       |
| Encoder common    | GND    | encoder pin 3 (shared, to board GND)    |
| Encoder push      | —      | middle contacts left unconnected         |
| Play/Pause        | GPIO6  | Kailh Choc, other side to GND           |
| Mute              | GPIO7  | Kailh Choc, other side to GND           |
| Switch #3 (rsvd)  | GPIO1  | wired, no action yet                    |
| Switch #4 (rsvd)  | GPIO2  | wired, no action yet                    |
| Switch #5 (rsvd)  | GPIO15 | wired, no action yet                    |

Nothing else is required — all the input GPIOs above support internal
pull-ups, so no external resistors. The three reserved switches are polled
and debounced in firmware today; assigning them a job later is a one-line
change to the `g_btns[]` table in `src/main.cpp` (set their `Action`).

## Build & flash

1. PlatformIO with the `espressif32` platform (Arduino-ESP32 ≥ 2.0.14).
2. Connect the **UART-USB** port (the one with the CP210x chip), press RST,
   hold BOOT if it doesn't auto-enter download mode.
3. `pio run` / click Build, then Upload.
4. Open the monitor at 115200 on the UART port for boot logs.

Plug the **native USB-C** port in afterwards (or before — order doesn't
matter) and Windows/macOS should enumerate a consumer-control HID with no
drivers.

> First connect: if Windows shows an unknown device, unplug/replug once —
> the HID device sometimes needs a second enumeration pass.

## PC-side macro launcher (staged, no trigger wired yet)

`pc/media-launcher.ahk` (AutoHotkey **v2**) is ready to catch an **F13**
keypress and launch the executable in `TargetExe`. No switch sends F13 today
(all three reserved switches are `Action::kNone`), so the script does nothing
until one of them is mapped. To wire up a macro later:

1. Add a keyboard report: re-include `USBHIDKeyboard`, add a `USBHIDKeyboard
   keyboard;` object, call `keyboard.begin()` in `setup()`, and send
   `keyboard.press(KEY_F13)` / `keyboard.release(KEY_F13)` from the matching
   reserved switch's `Action`.
2. Edit `TargetExe` in the AHK script, run it, done.

F13 is reserved for this device — don't remap it in other apps.

## Controls

| Input            | Sends                                        |
|------------------|----------------------------------------------|
| Encoder CW/CCW   | Consumer Vol Up (0x00E9) / Vol Down (0x00EA) — 1 step/count, rate-limited while spinning |
| Play/Pause       | Consumer 0x00CD, held for as long as the button is held |
| Mute             | Consumer 0x00E2, held for as long as the button is held |
| Switches #3–#5   | (none — reserved)                            |

## Tunables (top of `src/main.cpp`)

| Macro             | Default | Meaning                                        |
|-------------------|---------|------------------------------------------------|
| `ENC_MIN_EDGE_US` | 300     | noise gate between encoder edges (µs)          |
| `ENC_DIR`         | 1       | set `-1` to flip CW/CCW                        |
| `BTN_DEBOUNCE_MS` | 20      | two-sample switch debounce                     |
| `VOL_STEP_MS`     | 80      | gap between volume steps while spinning        |

## Edge cases handled

- **USB suspend/resume & replug**: the TinyUSB stack re-enumerates; on
  resume the firmware re-sends all-releases so no key stays logically down.
- **Encoder noise / overspeed**: 4x quadrature with invalid-state rejection
  + inter-edge gate; counts accumulate in the ISR so fast spins never lose
  steps (only the *output* is rate-limited).
- **Button bounce**: two-sample 20 ms debounce → exactly one event per make/break.
