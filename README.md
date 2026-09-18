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
   EC11 rotary encoder (5-pin)
     pin 1 (A)      ── W1 ──►  left header, row 4  (GPIO4)
     pin 2 (B)      ── W2 ──►  left header, row 5  (GPIO5)
     pin 3 (COM)    ── W3 ──►  GND bus
     pins 4, 5 (push switch)  ──►  LEAVE UNCONNECTED

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
| W1   | EC11 pin 1 (A)                  | GPIO4 — left header, row 4              |
| W2   | EC11 pin 2 (B)                  | GPIO5 — left header, row 5              |
| W3   | EC11 pin 3 (COM)                | GND bus                                 |
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

- **EC11**: the three encoder pins are the contiguous set (commonly 1-2-3);
  the two push-switch pins (commonly 4-5) sit on the far end and stay
  unconnected. If your part's markings differ, ID the common with a
  multimeter: COM reads 0 Ω to A for half a turn, 0 Ω to B for the next half
  turn; A↔B never connect directly.
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
