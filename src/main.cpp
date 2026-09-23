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
#include <USBHIDKeyboard.h>         // keyboard device
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

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
#define WIFI_CONNECT_MS 12000
#define AP_PASSWORD      "configureme"
#define MAX_ACTION_LEN   220

// Consumer control usages (HID 1.12 usage page 0x0C, from the USB lib)
#define CC_PLAY_PAUSE  CONSUMER_CONTROL_PLAY_PAUSE           // 0x00CD
#define CC_MUTE        CONSUMER_CONTROL_MUTE                 // 0x00E2
#define CC_VOL_UP      CONSUMER_CONTROL_VOLUME_INCREMENT     // 0x00E9
#define CC_VOL_DOWN    CONSUMER_CONTROL_VOLUME_DECREMENT     // 0x00EA

USBHIDConsumerControl consumer;
USBHIDKeyboard keyboard;  // shares the same USB HID interface; report id 1

// ========================= buttons =========================

// What pressing a switch does. kNone = wired + debounced, but no HID output.
enum class ActionType { kNone, kMedia, kKey, kCombo, kMacro };

struct ButtonAction {
  ActionType type;
  String value;
  String modifiers;
  String macro;
};

struct DialConfig {
  String mode;
  String key;
  int step;
  bool reverse;
  bool accelerate;
  int interval;
};

static ButtonAction g_actions[5] = {
  {ActionType::kMedia, "play", "", ""},
  {ActionType::kMedia, "mute", "", ""},
  {ActionType::kNone, "", "", ""},
  {ActionType::kNone, "", "", ""},
  {ActionType::kKey, "F13", "", ""},
};
static DialConfig g_dial = {"volume", "UP", 1, false, true, 35};
static String g_hostname = "media-controller";
static String g_wifiSsid;
static String g_wifiPassword;
static bool g_apMode = false;
static bool g_wifiReady = false;
static uint32_t g_wifiStarted = 0;
static bool g_wifiWasConnected = false;
static WebServer g_server(80);
static Preferences g_preferences;

struct Btn {
  const char* name;
  uint8_t pin;
  uint8_t stable;          // debounced level (1 = pressed)
  uint8_t lastRaw;         // last raw read
  uint32_t lastChanged;
    Btn(const char* n, uint8_t p)
      : name(n), pin(p), stable(1), lastRaw(0xFF), lastChanged(0) {}
};

static Btn g_btns[] = {
  Btn("button 1", BTN_PLAY_PIN), Btn("button 2", BTN_MUTE_PIN),
  Btn("button 3", BTN_SW3_PIN), Btn("button 4", BTN_SW4_PIN),
  Btn("button 5", BTN_SW5_PIN),
};

static uint8_t keyCode(String key) {
  key.trim(); key.toUpperCase();
  if (key.length() == 1 && key[0] >= 'A' && key[0] <= 'Z') return 0x04 + key[0] - 'A';
  if (key.length() == 1 && key[0] >= '0' && key[0] <= '9') return 0x27 + key[0] - '0';
  if (key.startsWith("F")) {
    int functionNumber = key.substring(1).toInt();
    if (functionNumber >= 1 && functionNumber <= 12) return KEY_F1 + functionNumber - 1;
    if (functionNumber >= 13 && functionNumber <= 24) return KEY_F13 + functionNumber - 13;
    if (functionNumber >= 25 && functionNumber <= 35) return 0x74 + functionNumber - 25;
  }
  if (key == "ENTER") return KEY_RETURN;
  if (key == "ESC" || key == "ESCAPE") return KEY_ESC;
  if (key == "TAB") return KEY_TAB;
  if (key == "SPACE") return ' ';
  if (key == "BACKSPACE") return KEY_BACKSPACE;
  if (key == "DELETE") return KEY_DELETE;
  if (key == "UP") return KEY_UP_ARROW;
  if (key == "DOWN") return KEY_DOWN_ARROW;
  if (key == "LEFT") return KEY_LEFT_ARROW;
  if (key == "RIGHT") return KEY_RIGHT_ARROW;
  if (key == "HOME") return KEY_HOME;
  if (key == "END") return KEY_END;
  if (key == "PAGEUP") return KEY_PAGE_UP;
  if (key == "PAGEDOWN") return KEY_PAGE_DOWN;
  if (key == "INSERT") return KEY_INSERT;
  if (key == "CAPSLOCK") return KEY_CAPS_LOCK;
  if (key == "PRINTSCREEN") return 0x46;
  if (key == "SCROLLLOCK") return 0x47;
  if (key == "PAUSE") return 0x48;
  if (key == "-") return 0x2D;
  if (key == "=") return 0x2E;
  if (key == "[") return 0x2F;
  if (key == "]") return 0x30;
  if (key == "\\") return 0x31;
  if (key == ";") return 0x33;
  if (key == "'") return 0x34;
  if (key == "`") return 0x35;
  if (key == ",") return 0x36;
  if (key == ".") return 0x37;
  if (key == "/") return 0x38;
  return (uint8_t)key.toInt();
}

static uint8_t modifierCode(String modifier) {
  modifier.toUpperCase();
  if (modifier == "CTRL" || modifier == "CONTROL") return KEY_LEFT_CTRL;
  if (modifier == "SHIFT") return KEY_LEFT_SHIFT;
  if (modifier == "ALT") return KEY_LEFT_ALT;
  if (modifier == "GUI" || modifier == "WIN" || modifier == "CMD") return KEY_LEFT_GUI;
  return 0;
}

static void pressKeyboardSpec(String spec) {
  int separator = spec.lastIndexOf('+');
  if (separator < 0) {
    keyboard.press(keyCode(spec));
    keyboard.release(keyCode(spec));
    return;
  }
  String modifiers = spec.substring(0, separator);
  String key = spec.substring(separator + 1);
  int start = 0;
  while (start < modifiers.length()) {
    int end = modifiers.indexOf('+', start);
    if (end < 0) end = modifiers.length();
    uint8_t modifier = modifierCode(modifiers.substring(start, end));
    if (modifier) keyboard.press(modifier);
    start = end + 1;
  }
  keyboard.press(keyCode(key));
  keyboard.release(keyCode(key));
  start = 0;
  while (start < modifiers.length()) {
    int end = modifiers.indexOf('+', start);
    if (end < 0) end = modifiers.length();
    uint8_t modifier = modifierCode(modifiers.substring(start, end));
    if (modifier) keyboard.release(modifier);
    start = end + 1;
  }
}

static void pressAction(const ButtonAction& action) {
  if (action.type == ActionType::kMedia) {
    uint16_t usage = 0;
    if (action.value == "play") usage = CC_PLAY_PAUSE;
    else if (action.value == "mute") usage = CC_MUTE;
    else if (action.value == "volumeup") usage = CC_VOL_UP;
    else if (action.value == "volumedown") usage = CC_VOL_DOWN;
    else if (action.value == "next") usage = CONSUMER_CONTROL_SCAN_NEXT;
    else if (action.value == "previous") usage = CONSUMER_CONTROL_SCAN_PREVIOUS;
    else if (action.value == "stop") usage = CONSUMER_CONTROL_STOP;
    else if (action.value == "record") usage = 0xB2;
    else if (action.value == "fastforward") usage = 0xB3;
    else if (action.value == "rewind") usage = 0xB4;
    else if (action.value == "eject") usage = 0xB8;
    else if (action.value.startsWith("raw:")) usage = action.value.substring(4).toInt();
    if (usage) { consumer.press(usage); consumer.release(); }
    return;
  }
  if (action.type == ActionType::kNone) return;
  if (action.type == ActionType::kMacro) {
    int start = 0;
    while (start < action.macro.length()) {
      int end = action.macro.indexOf(',', start);
      if (end < 0) end = action.macro.length();
      String step = action.macro.substring(start, end);
      int split = step.indexOf(':');
      String key = split < 0 ? step : step.substring(0, split);
      int delayMs = split < 0 ? 40 : step.substring(split + 1).toInt();
      pressKeyboardSpec(key);
      delay(constrain(delayMs, 0, 2000));
      start = end + 1;
    }
    return;
  }
  uint8_t modifiers[4] = {0};
  int count = 0, start = 0;
  while (start < action.modifiers.length() && count < 4) {
    int end = action.modifiers.indexOf('+', start);
    if (end < 0) end = action.modifiers.length();
    modifiers[count++] = modifierCode(action.modifiers.substring(start, end));
    start = end + 1;
  }
  for (int i = 0; i < count; i++) if (modifiers[i]) keyboard.press(modifiers[i]);
  keyboard.press(keyCode(action.value));
  keyboard.release(keyCode(action.value));
  for (int i = count - 1; i >= 0; i--) if (modifiers[i]) keyboard.release(modifiers[i]);
}

static void releaseAction(const ButtonAction& action) {
  if (action.type == ActionType::kMedia) consumer.release();
}

static String actionTypeName(ActionType type) {
  switch (type) {
    case ActionType::kMedia: return "media";
    case ActionType::kKey: return "key";
    case ActionType::kCombo: return "combo";
    case ActionType::kMacro: return "macro";
    default: return "none";
  }
}

static ActionType actionTypeFromName(String type) {
  type.toLowerCase();
  if (type == "media") return ActionType::kMedia;
  if (type == "key") return ActionType::kKey;
  if (type == "combo") return ActionType::kCombo;
  if (type == "macro") return ActionType::kMacro;
  return ActionType::kNone;
}

static String jsonEscape(const String& value) {
  String out;
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '"' || c == '\\') out += '\\';
    switch (c) {
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

static String actionJson(const ButtonAction& action) {
  return "{\"type\":\"" + actionTypeName(action.type) +
    "\",\"value\":\"" + jsonEscape(action.value) +
    "\",\"modifiers\":\"" + jsonEscape(action.modifiers) +
    "\",\"macro\":\"" + jsonEscape(action.macro) + "\"}";
}

static String configJson() {
  String json = "{\"hostname\":\"" + jsonEscape(g_hostname) + "\",\"ssid\":\"" +
    jsonEscape(g_wifiSsid) + "\",\"ap\":" + (g_apMode ? "true" : "false") +
    ",\"ip\":\"" + (g_apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",\"buttons\":[";
  for (int i = 0; i < 5; i++) { if (i) json += ","; json += actionJson(g_actions[i]); }
  json += "],\"dial\":{\"mode\":\"" + jsonEscape(g_dial.mode) + "\",\"key\":\"" + jsonEscape(g_dial.key) +
    "\",\"step\":" + String(g_dial.step) + ",\"reverse\":" + (g_dial.reverse ? "true" : "false") +
    ",\"accelerate\":" + (g_dial.accelerate ? "true" : "false") +
    ",\"interval\":" + String(g_dial.interval) + "}}";
  return json;
}

static void loadConfig() {
  g_preferences.begin("media-ctrl", false);
  g_wifiSsid = g_preferences.getString("ssid", "");
  g_wifiPassword = g_preferences.getString("password", "");
  g_hostname = g_preferences.getString("hostname", "media-controller");
  for (int i = 0; i < 5; i++) {
    String savedType = g_preferences.getString(("b" + String(i) + "t").c_str(), "");
    if (savedType.length()) {
      g_actions[i].type = actionTypeFromName(savedType);
      g_actions[i].value = g_preferences.getString(("b" + String(i) + "v").c_str(), "");
      g_actions[i].modifiers = g_preferences.getString(("b" + String(i) + "m").c_str(), "");
      g_actions[i].macro = g_preferences.getString(("b" + String(i) + "x").c_str(), "");
    } else {
      String saved = g_preferences.getString(("btn" + String(i)).c_str(), "");
      if (!saved.length()) continue;
      int p1 = saved.indexOf('|'), p2 = saved.indexOf('|', p1 + 1), p3 = saved.indexOf('|', p2 + 1);
      if (p1 > 0 && p2 > p1 && p3 > p2) {
        g_actions[i].type = actionTypeFromName(saved.substring(0, p1));
        g_actions[i].value = saved.substring(p1 + 1, p2);
        g_actions[i].modifiers = saved.substring(p2 + 1, p3);
        g_actions[i].macro = saved.substring(p3 + 1);
      }
    }
  }
  g_dial.mode = g_preferences.getString("dialMode", g_dial.mode);
  g_dial.key = g_preferences.getString("dialKey", g_dial.key);
  g_dial.step = constrain(g_preferences.getInt("dialStep", g_dial.step), 1, 10);
  g_dial.reverse = g_preferences.getBool("dialReverse", g_dial.reverse);
  g_dial.accelerate = g_preferences.getBool("dialAccel", g_dial.accelerate);
  g_dial.interval = constrain(g_preferences.getInt("dialInterval", g_dial.interval), 0, 500);
}

static void saveConfig() {
  g_preferences.putString("ssid", g_wifiSsid);
  g_preferences.putString("password", g_wifiPassword);
  g_preferences.putString("hostname", g_hostname);
  for (int i = 0; i < 5; i++) {
    g_preferences.putString(("b" + String(i) + "t").c_str(), actionTypeName(g_actions[i].type));
    g_preferences.putString(("b" + String(i) + "v").c_str(), g_actions[i].value.substring(0, MAX_ACTION_LEN));
    g_preferences.putString(("b" + String(i) + "m").c_str(), g_actions[i].modifiers.substring(0, MAX_ACTION_LEN));
    g_preferences.putString(("b" + String(i) + "x").c_str(), g_actions[i].macro.substring(0, MAX_ACTION_LEN));
  }
  g_preferences.putString("dialMode", g_dial.mode);
  g_preferences.putString("dialKey", g_dial.key);
  g_preferences.putInt("dialStep", g_dial.step);
  g_preferences.putBool("dialReverse", g_dial.reverse);
  g_preferences.putBool("dialAccel", g_dial.accelerate);
  g_preferences.putInt("dialInterval", g_dial.interval);
}

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Media Controller</title><style>
:root{font-family:system-ui,sans-serif;--bg:#e9eee8;--ink:#17211b;--surface:#17211b;--paper:#fbfcf8;--line:#c9d2c7;--lime:#c8f169;--muted:#647064;--field:#ffffff;--on-accent:#17211b;color:var(--ink);background:var(--bg)}:root.dark{--bg:#12150f;--ink:#e6e2d6;--surface:#242b20;--paper:#1a1f18;--line:#333b2d;--lime:#c8f169;--muted:#939a8b;--field:#101410;--on-accent:#17211b}*{box-sizing:border-box}body{margin:0}.top{background:var(--surface);color:white;padding:28px max(20px,calc((100% - 980px)/2));display:flex;justify-content:space-between;gap:20px;align-items:end}.top h1{margin:0;font-size:clamp(28px,5vw,52px);letter-spacing:-2px}.top p{margin:8px 0 0;color:#b9c5b8}.status{background:var(--lime);color:var(--on-accent);padding:9px 13px;border-radius:20px;font-weight:700;white-space:nowrap}.hgroup{display:flex;gap:10px;align-items:center}.theme-toggle{border:1px solid var(--line);background:var(--paper);color:var(--ink);font-size:16px;line-height:1;padding:8px 10px;border-radius:20px;cursor:pointer}.wrap{max-width:980px;margin:28px auto;padding:0 20px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:18px}.panel{background:var(--paper);border:1px solid var(--line);padding:20px;border-radius:8px;box-shadow:0 5px 18px #33442b0d}.panel h2{margin:0 0 16px;font-size:20px}.wide{grid-column:1/-1}.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}@media(max-width:600px){.row{grid-template-columns:1fr}}label{display:block;font-size:12px;font-weight:700;text-transform:uppercase;letter-spacing:.08em;margin:12px 0 6px}input,select,textarea{width:100%;padding:10px;border:1px solid var(--line);border-radius:5px;background:var(--field);color:var(--ink);font:inherit}textarea{min-height:62px;resize:vertical}.hint{color:var(--muted);font-size:13px;line-height:1.45}.btn{border:0;border-radius:5px;padding:11px 16px;background:var(--surface);color:white;font-weight:700;cursor:pointer;margin-top:14px}.btn.alt{background:var(--lime);color:var(--on-accent)}.button-card{border-top:3px solid var(--lime);padding-top:8px}.button-card h3{margin:0}.toast{position:fixed;bottom:20px;right:20px;background:var(--surface);color:white;padding:12px 16px;border-radius:5px;display:none}.disabled{opacity:.45}.small{font-size:12px;color:var(--muted)}
</style></head><body><header class="top"><div><h1>Media Controller</h1><p>USB actions, tuned locally from any browser.</p></div><div class="hgroup"><div class="status" id="status">Loading...</div><button id="themeToggle" class="theme-toggle" type="button" aria-label="Toggle dark mode">&#9788;</button></div></header><main class="wrap"><div class="grid"><section class="panel"><h2>Network</h2><p class="hint">Connect the controller to your 2.4 GHz Wi-Fi. After saving, use the hostname shown here on your local network.</p><label>Wi-Fi name</label><input id="ssid" autocomplete="off"><label>Wi-Fi password</label><input id="password" type="password" placeholder="Leave blank to keep current"><label>Hostname</label><input id="hostname" maxlength="30"><button class="btn alt" onclick="save()">Save network</button></section><section class="panel"><h2>Connection</h2><p id="connection" class="hint">Checking device status...</p><button class="btn" onclick="location.reload()">Refresh status</button><p class="small">Setup AP: <b>MediaCtrl-XXXX</b>, password <b>configureme</b></p></section><section class="panel wide"><h2>Buttons 1-5</h2><div id="buttons" class="grid"></div></section><section class="panel wide"><h2>Dial</h2><div class="row"><div><label>Action</label><select id="dialMode"><option value="volume">Volume</option><option value="next">Next track</option><option value="previous">Previous track</option><option value="scroll">Scroll</option><option value="key">Keyboard key</option></select></div><div><label>Keyboard key</label><input id="dialKey" value="UP" placeholder="UP or F5"></div></div><div class="row"><div><label>Steps per click</label><input id="dialStep" type="number" min="1" max="10"></div><div><label>Minimum interval (ms)</label><input id="dialInterval" type="number" min="0" max="500"></div></div><label><input id="dialReverse" type="checkbox" style="width:auto"> Reverse direction</label><label><input id="dialAccelerate" type="checkbox" style="width:auto"> Accelerate on fast turns</label><p class="hint">Scroll sends arrow keys. Keyboard mode sends the chosen key, which makes the dial useful for timeline scrubbing, brush size, zoom, or any app with keyboard shortcuts.</p><button class="btn alt" onclick="save()">Save all settings</button></section></div></main><div class="toast" id="toast"></div><script>
let cfg;const media=[['play','Play / pause'],['mute','Mute'],['volumeup','Volume up'],['volumedown','Volume down'],['next','Next track'],['previous','Previous track'],['stop','Stop'],['record','Record'],['fastforward','Fast forward'],['rewind','Rewind'],['eject','Eject'],['__raw__','Custom consumer code']];const keys=['A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z','0','1','2','3','4','5','6','7','8','9','ENTER','ESC','TAB','SPACE','BACKSPACE','DELETE','UP','DOWN','LEFT','RIGHT','HOME','END','PAGEUP','PAGEDOWN','INSERT','CAPSLOCK','PRINTSCREEN','SCROLLLOCK','PAUSE','-','=','[',']','\\',';','\'','`',',','.','/',...Array.from({length:24},(_,i)=>'F'+(i+1))];
function esc(s){return (s||'').replaceAll('&','&amp;').replaceAll('"','&quot;').replaceAll('<','&lt;')}function options(a,v){if(v&&!a.some(x=>(Array.isArray(x)?x[0]:x)===v))a=[[''+v,'Custom: '+v],...a];return a.map(x=>{let value=Array.isArray(x)?x[0]:x;let label=Array.isArray(x)?x[1]:x;return `<option value="${value}" ${v===value?'selected':''}>${label}</option>`}).join('')}function card(a,i){return `<div class="button-card"><h3>Button ${i+1}</h3><label>Action</label><select id="t${i}" onchange="toggle(${i})"><option value="none" ${a.type==='none'?'selected':''}>Nothing</option><option value="media" ${a.type==='media'?'selected':''}>Media control</option><option value="key" ${a.type==='key'?'selected':''}>Keyboard key</option><option value="combo" ${a.type==='combo'?'selected':''}>Key + modifiers</option><option value="macro" ${a.type==='macro'?'selected':''}>Macro</option></select><div id="extra${i}"></div></div>`}function toggle(i){let type=document.getElementById('t'+i).value,a=cfg.buttons[i],x=document.getElementById('extra'+i);if(type==='media')x.innerHTML='<label>Media action</label><select id="v'+i+'" onchange="document.getElementById(\'r'+i+'\').hidden=this.value!==\'__raw__\'">'+options(media,a.value)+'</select><input id="r'+i+'" placeholder="e.g. raw:205 (decimal usage)" '+(a.value.indexOf('raw:')===0?'value="'+esc(a.value)+'"':'hidden')+'>';else if(type==='key')x.innerHTML='<label>Key</label><select id="v'+i+'">'+options(keys,a.value)+'</select>';else if(type==='combo')x.innerHTML='<label>Key</label><select id="v'+i+'">'+options(keys,a.value)+'</select><label>Modifiers (use +)</label><input id="m'+i+'" value="'+esc(a.modifiers)+'" placeholder="CTRL+SHIFT">';else if(type==='macro')x.innerHTML='<label>Steps: KEY:delay,KEY:delay</label><textarea id="x'+i+'" placeholder="CTRL+A:0,A:80">'+esc(a.macro)+'</textarea><p class="small">Each step is pressed and released, then waits its delay in milliseconds.</p>';else x.innerHTML=''}async function load(){let res;try{res=await fetch('/api/config');cfg=await res.json()}catch(e){document.getElementById('status').textContent='OFFLINE';document.getElementById('status').title='Reload when the device is back online';return}document.getElementById('ssid').value=cfg.ssid;document.getElementById('hostname').value=cfg.hostname;document.getElementById('status').textContent=cfg.ap?'SETUP AP':'ONLINE';document.getElementById('connection').innerHTML=cfg.ap?'Connect to <b>'+cfg.ip+'</b> while on the setup Wi-Fi.':'Reach this page at <b>http://'+cfg.hostname+'.local</b><br>IP: <b>'+cfg.ip+'</b>';document.getElementById('buttons').innerHTML=cfg.buttons.map(card).join('');cfg.buttons.forEach((a,i)=>toggle(i));let d=cfg.dial;document.getElementById('dialMode').value=d.mode;document.getElementById('dialKey').value=d.key;document.getElementById('dialStep').value=d.step;document.getElementById('dialInterval').value=d.interval;document.getElementById('dialReverse').checked=d.reverse;document.getElementById('dialAccelerate').checked=d.accelerate}async function save(){let p=new URLSearchParams({ssid:document.getElementById('ssid').value,password:document.getElementById('password').value,hostname:document.getElementById('hostname').value,dialMode:document.getElementById('dialMode').value,dialKey:document.getElementById('dialKey').value,dialStep:document.getElementById('dialStep').value,dialInterval:document.getElementById('dialInterval').value,dialReverse:document.getElementById('dialReverse').checked?'1':'0',dialAccelerate:document.getElementById('dialAccelerate').checked?'1':'0'});cfg.buttons.forEach((a,i)=>{let type=document.getElementById('t'+i).value;p.set('t'+i,type);if(type==='media'){var sv=document.getElementById('v'+i).value;p.set('v'+i,sv==='__raw__'?document.getElementById('r'+i).value.trim():sv)}else if(type==='key'||type==='combo')p.set('v'+i,document.getElementById('v'+i).value);if(type==='combo')p.set('m'+i,document.getElementById('m'+i).value);if(type==='macro')p.set('x'+i,document.getElementById('x'+i).value)});let r;try{r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p})}catch(e){r={ok:false}}toast(r.ok?'Saved. Reconnecting Wi-Fi if needed.':'Could not save');if(r.ok)setTimeout(load,1200)}function toast(t){let x=document.getElementById('toast');x.textContent=t;x.style.display='block';setTimeout(()=>x.style.display='none',2500)}try{if(localStorage.getItem('mc-theme')==='dark'||(!localStorage.getItem('mc-theme')&&matchMedia('(prefers-color-scheme: dark)').matches))document.documentElement.classList.add('dark')}catch(e){}document.getElementById('themeToggle').onclick=()=>{document.documentElement.classList.toggle('dark');try{localStorage.setItem('mc-theme',document.documentElement.classList.contains('dark')?'dark':'light')}catch(e){}}load();
</script></body></html>
)HTML";

static void sendNoCache(const String& body, const char* type) {
  g_server.sendHeader("Cache-Control", "no-store");
  g_server.send(200, type, body);
}

static void handleConfig() {
  if (g_server.method() == HTTP_GET) { sendNoCache(configJson(), "application/json"); return; }
  g_wifiSsid = g_server.arg("ssid");
  if (g_server.hasArg("password") && g_server.arg("password").length()) g_wifiPassword = g_server.arg("password");
  g_hostname = g_server.arg("hostname");
  g_hostname.toLowerCase();
  g_hostname.replace(" ", "-");
  if (!g_hostname.length()) g_hostname = "media-controller";
  for (int i = 0; i < 5; i++) {
    String type = g_server.arg("t" + String(i));
    g_actions[i].type = actionTypeFromName(type);
    g_actions[i].value = g_server.arg("v" + String(i));
    g_actions[i].modifiers = g_server.arg("m" + String(i));
    g_actions[i].macro = g_server.arg("x" + String(i)).substring(0, MAX_ACTION_LEN);
  }
  g_dial.mode = g_server.arg("dialMode");
  g_dial.key = g_server.arg("dialKey");
  g_dial.step = constrain(g_server.arg("dialStep").toInt(), 1, 10);
  g_dial.interval = constrain(g_server.arg("dialInterval").toInt(), 0, 500);
  g_dial.reverse = g_server.arg("dialReverse") == "1";
  g_dial.accelerate = g_server.arg("dialAccelerate") == "1";
  saveConfig();
  sendNoCache("{\"saved\":true}", "application/json");
  g_apMode = false;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  g_wifiReady = false; g_wifiStarted = millis();
  WiFi.setHostname(g_hostname.c_str());
  WiFi.begin(g_wifiSsid.c_str(), g_wifiPassword.c_str());
}

static void startAccessPoint() {
  g_apMode = true; g_wifiReady = false;
  WiFi.mode(WIFI_AP_STA);
  String apName = "MediaCtrl-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
  WiFi.softAP(apName.c_str(), AP_PASSWORD);
  Serial.printf("[media-ctrl] setup AP %s at %s\n", apName.c_str(), WiFi.softAPIP().toString().c_str());
}

static void maintainWifi() {
  if (!g_apMode && WiFi.status() == WL_CONNECTED) {
    if (!g_wifiReady) {
      g_wifiReady = true; WiFi.setHostname(g_hostname.c_str());
      MDNS.begin(g_hostname.c_str()); MDNS.addService("http", "tcp", 80);
      Serial.printf("[media-ctrl] Wi-Fi ready: http://%s.local (%s)\n", g_hostname.c_str(), WiFi.localIP().toString().c_str());
    }
    g_wifiWasConnected = true;
  } else if (!g_apMode) {
    if (g_wifiWasConnected) g_wifiStarted = millis(); // restart the grace timer when the link drops
    g_wifiWasConnected = false;
    if (millis() - g_wifiStarted > WIFI_CONNECT_MS) startAccessPoint();
  }
}

static void startNetwork() {
  if (!g_wifiSsid.length()) startAccessPoint();
  else { WiFi.mode(WIFI_STA); WiFi.setHostname(g_hostname.c_str()); WiFi.begin(g_wifiSsid.c_str(), g_wifiPassword.c_str()); g_wifiStarted = millis(); }
  g_server.on("/", HTTP_GET, [](){ g_server.send_P(200, "text/html", INDEX_HTML); });
  g_server.on("/api/config", HTTP_GET, handleConfig);
  g_server.on("/api/config", HTTP_POST, handleConfig);
  g_server.onNotFound([](){ g_server.send(404, "text/plain", "Not found"); });
  g_server.begin();
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
      int index = (int)(&b - g_btns);
      if (!raw) pressAction(g_actions[index]);
      else releaseAction(g_actions[index]);
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

// ========================= dial output =========================
//
// One mechanical detent (one "click") == one volume notch. The ISR lumps raw
// 4x quadrature counts into g_encDelta; each tick we read the counts captured
// since the last tick, turn whole detents into notches, and emit ONLY for
// movement that happened this tick. We never keep a saved backlog, so the
// volume stops changing the instant the wheel stops turning.

static long g_encFrac = 0;    // sub-detent remainder of raw encoder counts
static uint32_t g_lastDialOutput = 0;

static void handleEncoder() {
  int32_t d = pollEncoder();  // raw counts this tick; 0 means the wheel is still
  if (d == 0) return;         // -> do nothing (no drain, no auto-repeat)

  g_encFrac += d;
  long detents = g_encFrac / ENC_COUNTS_PER_DETENT;  // whole clicks
  if (detents == 0) return;                          // <1 click: carry the remainder
  g_encFrac -= detents * ENC_COUNTS_PER_DETENT;

  if (g_dial.reverse) detents = -detents;
  long steps = abs(detents) * g_dial.step;
  if (g_dial.accelerate && abs(detents) >= 3) steps *= 2;
  bool positive = detents > 0;
  for (long n = 0; n < steps; n++) {
    uint32_t now = millis();
    if (g_dial.interval && now - g_lastDialOutput < (uint32_t)g_dial.interval) delay(g_dial.interval - (now - g_lastDialOutput));
    g_lastDialOutput = millis();
    if (g_dial.mode == "volume") {
      consumer.press(positive ? CC_VOL_UP : CC_VOL_DOWN);
      consumer.release();
    } else if (g_dial.mode == "next" || g_dial.mode == "previous") {
      uint16_t usage = (g_dial.mode == "next") == positive ? CONSUMER_CONTROL_SCAN_NEXT : CONSUMER_CONTROL_SCAN_PREVIOUS;
      consumer.press(usage); consumer.release();
    } else {
      String key = g_dial.mode == "scroll" ? (positive ? "UP" : "DOWN") : g_dial.key;
      keyboard.press(keyCode(key)); keyboard.release(keyCode(key));
    }
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
  loadConfig();
  startNetwork();
}

void loop() {
  handleEncoder();
  pollButtons();
  g_server.handleClient();
  maintainWifi();
}
