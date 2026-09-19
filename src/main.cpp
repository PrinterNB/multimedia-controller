/*
 * ESP32-S3 DevKitC-1 — USB HID Media Controller
 * ===============================================
 *
 * USB layout (two ports on the DevKitC-1):
 *   - Native USB (GPIO19/20): TinyUSB HID consumer-control device
 *     (play/pause, mute, volume), using the framework's USBHIDConsumerControl.
 *   - UART0 / CP210x USB: firmware flashing + serial logging (Serial @115200)
 *
 * Inputs (internal pull-ups, switches to GND):
 *   - EC11 rotary encoder (smooth mod, push contact unconnected):
 *       GPIO4 = CH A, GPIO5 = CH B — interrupt-driven 4x quadrature
 *   - 5 mechanical switches. Play/Pause, Mute and F13 (sw5) have actions;
 *     the other two are wired and debounced but emit nothing (reserved).
 *
 * Key semantics:
 *   - Encoder CW/CCW  -> volume up/down (1 HID step per encoder count,
 *                        rate-limited while spinning)
 *   - Play/Pause, Mute -> consumer toggles, reported pressed for as long as
 *                        the physical button is held
 *   - SW5 -> F13 (HID keyboard page 0x07 usage 0x68) via the framework's
 *            USBHIDKeyboard; a host-side script (pc/media-launcher.ahk)
 *            turns that into the real macro action
 */

#include <Arduino.h>
#include <USB.h>                    // core ESPUSB (native TinyUSB stack)
#include <USBHIDConsumerControl.h>  // consumer control device
#include <USBHIDKeyboard.h>         // keyboard device (for F13)

// ========================= configuration =========================

#define ENC_A_PIN       4   // encoder channel A
#define ENC_B_PIN       5   // encoder channel B

// The 5 mechanical switches. All are wired, pull-up + debounced; only the
// ones with an action below actually do anything. The rest are reserved so
// adding a function later is a one-line change to the table in setup().
#define BTN_PLAY_PIN    6   // Kailh Choc: Play/Pause
#define BTN_MUTE_PIN    7   // Kailh Choc: Mute
#define BTN_SW3_PIN     1   // reserved (no action)
#define BTN_SW4_PIN     2   // reserved (no action)
#define BTN_SW5_PIN     15  // Kailh Choc: F13 (keyboard HID, see below)

#define ENC_MIN_EDGE_US      300 // min gap between accepted encoder edges (us)
#define ENC_DIR              1   // set to -1 to flip CW/CCW
// 4x quadrature counting: one mechanical detent (one "click") produces four
// valid A/B transitions. Divide by this to make 1 click == 1 volume notch.
// If volume feels too fast/slow, change this (2 = coarser, per-half-click).
#define ENC_COUNTS_PER_DETENT 4
#define BTN_DEBOUNCE_MS  20     // two-sample debounce for switches

// Consumer control usages (HID 1.12 usage page 0x0C, from the USB lib)
#define CC_PLAY_PAUSE  CONSUMER_CONTROL_PLAY_PAUSE           // 0x00CD
#define CC_MUTE        CONSUMER_CONTROL_MUTE                 // 0x00E2
#define CC_VOL_UP      CONSUMER_CONTROL_VOLUME_INCREMENT     // 0x00E9
#define CC_VOL_DOWN    CONSUMER_CONTROL_VOLUME_DECREMENT     // 0x00EA

USBHIDConsumerControl consumer;
USBHIDKeyboard keyboard;  // shares the same USB HID interface; report id 1

// ========================= buttons =========================

// What pressing a switch does. kNone = wired + debounced, but no HID output.
enum class Action { kNone, kPlayPause, kMute, kF13 };

struct Btn {
  const char* name;
  uint8_t pin;
  Action action;
  uint8_t stable;          // debounced level (1 = pressed)
  uint8_t lastRaw;         // last raw read
  uint32_t lastChanged;
  Btn(const char* n, uint8_t p, Action a)
      : name(n), pin(p), action(a), stable(0), lastRaw(0xFF), lastChanged(0) {}
};

static Btn g_btns[] = {
  Btn("play/pause", BTN_PLAY_PIN, Action::kPlayPause),
  Btn("mute",       BTN_MUTE_PIN, Action::kMute),
  Btn("sw3",        BTN_SW3_PIN,  Action::kNone),
  Btn("sw4",        BTN_SW4_PIN,  Action::kNone),
  Btn("sw5",        BTN_SW5_PIN,  Action::kF13),
};

static void btnDown(Action a) {
  switch (a) {
    case Action::kPlayPause: consumer.press(CC_PLAY_PAUSE); break;
    case Action::kMute:      consumer.press(CC_MUTE);       break;
    case Action::kF13:       keyboard.press(KEY_F13);       break;
    case Action::kNone: break;
  }
}

static void btnUp(Action a) {
  switch (a) {
    case Action::kPlayPause:
    case Action::kMute: consumer.release(); break;
    case Action::kF13:   keyboard.release(KEY_F13);         break;
    case Action::kNone: break;
  }
}

// Two-sample debounce: accept a level change only after it persists for
// BTN_DEBOUNCE_MS. Emits exactly one down / one up per make/break.
static void pollButtons() {
  uint32_t now = millis();
  for (Btn& b : g_btns) {
    uint8_t raw = digitalRead(b.pin);
    if (raw != b.lastRaw) { b.lastRaw = raw; b.lastChanged = now; continue; }
    if (raw != b.stable && now - b.lastChanged >= BTN_DEBOUNCE_MS) {
      b.stable = raw;
      if (raw) btnDown(b.action);
      else btnUp(b.action);
    }
  }
}

// ========================= USB lifecycle =========================
//
// The stack re-enumerates after replug for us; on RESUME we re-send
// releases so no key is left logically down after sleep / cable yank.

static void onUsbEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
  if (base != ARDUINO_USB_EVENTS) return;
  switch (id) {
    case ARDUINO_USB_STARTED_EVENT:
      Serial.println("[media-ctrl] USB attached");
      break;
    case ARDUINO_USB_RESUME_EVENT:
      consumer.release();
      keyboard.release(KEY_F13);
      break;
    default:
      break;
  }
}

// ========================= encoder =========================
//
// Interrupt-driven 4x quadrature. Each pin toggles on CHANGE; the ISR
// records only which direction it turned (a long delta counter) so the
// main loop is the only place USB traffic is generated.
//
// state = (A<<1)|B of the previous edge; index = (old<<2)|new over the
// 4-bit Gray-code ring. Valid 1-bit transitions give +/-1, everything else
// (2-bit jumps = bounce/noise) decodes to 0 and is ignored.

// Columns index the NEW (A,B) state 00,01,10,11; rows the OLD state.
// Valid 1-bit transitions on the Gray ring 00->10->11->01->00 give +/-1;
// 2-bit jumps (bounce) decode to 0 and are ignored.
static const int8_t encTable[16] = {
    0, -1, +1,  0,   // old=00
    +1,  0,  0, -1,   // old=01
    -1,  0,  0, +1,   // old=10
    0, +1, -1,  0     // old=11
};

static volatile long g_encDelta = 0;          // accumulated encoder counts
static volatile uint32_t g_encLastEdge = 0;   // micros() of last accepted edge
static uint8_t g_encState = 0;

static void encoderISR() {
  uint32_t now = micros();
  if (now - g_encLastEdge < ENC_MIN_EDGE_US) return; // noise gate
  g_encLastEdge = now;

  uint8_t ns = (digitalRead(ENC_A_PIN) << 1) | digitalRead(ENC_B_PIN);
  uint8_t idx = ((g_encState << 2) | ns) & 0x0F;
  g_encState = ns;
  g_encDelta += (long)encTable[idx] * ENC_DIR;
}

static int32_t pollEncoder() {
  noInterrupts();
  int32_t d = (int32_t)g_encDelta;
  g_encDelta = 0;
  portENABLE_INTERRUPTS();
  return d;
}

// ========================= volume stepping =========================
//
// One mechanical detent (one "click") == one volume notch. The ISR lumps raw
// 4x quadrature counts into g_encDelta; each tick we read the counts captured
// since the last tick, turn whole detents into notches, and emit ONLY for
// movement that happened this tick. We never keep a saved backlog, so the
// volume stops changing the instant the wheel stops turning.

static long g_encFrac = 0;    // sub-detent remainder of raw encoder counts

static void handleEncoder() {
  int32_t d = pollEncoder();  // raw counts this tick; 0 means the wheel is still
  if (d == 0) return;         // -> do nothing (no drain, no auto-repeat)

  g_encFrac += d;
  long detents = g_encFrac / ENC_COUNTS_PER_DETENT;  // whole clicks
  if (detents == 0) return;                          // <1 click: carry the remainder
  g_encFrac -= detents * ENC_COUNTS_PER_DETENT;

  uint16_t usage = (detents > 0) ? CC_VOL_UP : CC_VOL_DOWN;
  for (long n = (detents > 0 ? detents : -detents); n > 0; n--) {
    consumer.press(usage);   // one notch = one clean press + release
    consumer.release();
  }
}

// ========================= setup / loop =========================

void setup() {
  Serial.begin(115200); // UART0 -> CP210x (flashing + logging port)
  Serial.println();
  Serial.println("[media-ctrl] ESP32-S3 USB HID media controller starting");

  pinMode(ENC_A_PIN, INPUT_PULLUP);
  pinMode(ENC_B_PIN, INPUT_PULLUP);
  for (Btn& b : g_btns) pinMode(b.pin, INPUT_PULLUP);

  g_encState = (digitalRead(ENC_A_PIN) << 1) | digitalRead(ENC_B_PIN);
  g_encLastEdge = micros();
  attachInterrupt(digitalPinToInterrupt(ENC_A_PIN), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B_PIN), encoderISR, CHANGE);

  // Native USB: consumer-control + keyboard HID on GPIO19/20.
  USB.onEvent(onUsbEvent);
  consumer.begin();
  keyboard.begin();
  USB.begin();
}

void loop() {
  handleEncoder();
  pollButtons();
}
