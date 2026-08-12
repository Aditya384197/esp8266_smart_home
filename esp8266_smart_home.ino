/*
 * ESP8266 Offline/Online Smart Home (NodeMCU v3)
 * ------------------------------------------------
 * Port of the ESP32 offline_smart_home project to ESP8266.
 *
 * SCOPE (as agreed):
 *   - Exactly 3 relays + 3 physical wall switches (fixed, no optional relay 4/5)
 *   - Local AP (always on) + optional home Wi-Fi (STA) for internet features
 *   - Physical switch changes reflect on the web page within ~1s
 *   - Per-relay schedule manager (local, keeps working even if cloud is down)
 *   - Wi-Fi (SSID/Password) can be saved on its own -- Cloud fields are optional
 *     and remote control only activates once ALL THREE cloud fields are filled
 *   - Local OTA (password protected) + optional Remote OTA via HTTPS URL
 *
 * REQUIRED LIBRARIES (install via Arduino IDE Library Manager):
 *   - ESP8266WiFi, ESP8266WebServer, DNSServer, LittleFS, ESP8266HTTPClient,
 *     ESP8266httpUpdate  -> all bundled with the "esp8266" board package
 *   - "ArduinoJson" by Benoit Blanchon (version 6.x)  -> install separately
 *
 * BOARD SETTINGS (Arduino IDE):
 *   Board: "NodeMCU 1.0 (ESP-12E Module)"
 *   Flash Size: "4MB (FS:2MB OTA:~1019KB)"   <-- IMPORTANT: must reserve space
 *               for BOTH an OTA partition and a LittleFS partition, otherwise
 *               remote/local OTA will fail with "not enough space".
 *
 * GPIO MAP (chosen specifically to avoid ESP8266 boot-strapping pins
 * GPIO0 / GPIO2 / GPIO15, which must NOT be used for relay outputs or for
 * switch inputs that could be closed at power-on -- doing so can prevent
 * the board from booting or, worse, glitch a relay ON briefly at power-up):
 *
 *   Relay 1 (e.g. Living Room Light) -> D1 (GPIO5)
 *   Relay 2 (e.g. Ceiling Fan)       -> D2 (GPIO4)
 *   Relay 3 (e.g. Charging Socket)   -> D0 (GPIO16)
 *   Switch 1                         -> D5 (GPIO14)
 *   Switch 2                         -> D6 (GPIO12)
 *   Switch 3                         -> D7 (GPIO13)
 *
 *   Wire each switch between its GPIO and GND. Internal pull-ups are used,
 *   so an open switch reads HIGH (off) and a closed switch reads LOW (on).
 *
 * MAINS SAFETY: relay contacts and 230V AC wiring must be installed by a
 * qualified person. The ESP8266 GPIOs are low-voltage logic only and must
 * never be wired directly to mains.
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESP8266httpUpdate.h>
#include <Updater.h>
#include <memory>
#include <time.h>

/* ---------------- User configuration ---------------- */

#define RELAY_COUNT              3
#define SWITCH_COUNT              3

static const uint8_t RELAY_GPIO[RELAY_COUNT] = { 5, 4, 16 };   // D1, D2, D0
static const uint8_t SWITCH_GPIO[SWITCH_COUNT] = { 14, 12, 13 }; // D5, D6, D7

#define RELAY_ACTIVE_LEVEL        HIGH   // change to LOW if your relay board is active-low
#define SWITCH_ACTIVE_LEVEL       LOW    // switch wired to GND, closed = LOW

#define DEFAULT_AP_SSID           "ESP8266-SMART-HOME"
#define DEFAULT_AP_PASSWORD       "ChangeMe123"
#define AP_MAX_CONNECTIONS        4

#define MAX_NAME_LEN              31
#define MAX_SSID_LEN              32
#define MAX_PASS_LEN              63
#define MAX_CLOUD_URL_LEN         191
#define MAX_DEVICE_ID_LEN         63
#define MAX_DEVICE_TOKEN_LEN      127
#define MAX_SCHEDULES             20

#define OTA_UPDATE_PASSWORD       "OTA@ESP8266#2026"
#define CONFIG_PATH               "/config.json"
#define SWITCH_DEBOUNCE_MS         60   // switch must be stable this long to register
#define CLOUD_POLL_INTERVAL_MS     5000
#define DNS_PORT                   53

/* ---------------- Runtime state ---------------- */

struct Schedule {
  bool enabled;
  uint8_t relay;   // 1..3
  uint8_t hour;
  uint8_t minute;
  uint8_t action;  // 0=off, 1=on
  uint8_t days;    // bitmask, bit0=Sun ... bit6=Sat
};

int relayState[RELAY_COUNT] = {0, 0, 0};
char relayName[RELAY_COUNT][MAX_NAME_LEN + 1] = {"Living Room Light", "Ceiling Fan", "Charging Socket"};

char apSsid[MAX_SSID_LEN + 1] = DEFAULT_AP_SSID;
char apPassword[MAX_PASS_LEN + 1] = DEFAULT_AP_PASSWORD;
char staSsid[MAX_SSID_LEN + 1] = "";
char staPassword[MAX_PASS_LEN + 1] = "";
char cloudUrl[MAX_CLOUD_URL_LEN + 1] = "";
char deviceId[MAX_DEVICE_ID_LEN + 1] = "";
char deviceToken[MAX_DEVICE_TOKEN_LEN + 1] = "";

Schedule schedules[MAX_SCHEDULES];
int scheduleCount = 0;

ESP8266WebServer server(80);
DNSServer dnsServer;
bool staConnected = false;
bool otaInProgress = false;
unsigned long lastCloudPoll = 0;
bool timeSynced = false;

/* ---------------- Persistence (LittleFS + ArduinoJson) ---------------- */

void loadDefaults() {
  strlcpy(apSsid, DEFAULT_AP_SSID, sizeof(apSsid));
  strlcpy(apPassword, DEFAULT_AP_PASSWORD, sizeof(apPassword));
  staSsid[0] = '\0'; staPassword[0] = '\0';
  cloudUrl[0] = '\0'; deviceId[0] = '\0'; deviceToken[0] = '\0';
  const char *defNames[RELAY_COUNT] = {"Living Room Light", "Ceiling Fan", "Charging Socket"};
  for (int i = 0; i < RELAY_COUNT; i++) {
    relayState[i] = 0;
    strlcpy(relayName[i], defNames[i], sizeof(relayName[i]));
  }
  scheduleCount = 0;
}

bool saveConfig() {
  DynamicJsonDocument doc(4096);
  doc["ap_ssid"] = apSsid;
  doc["ap_pass"] = apPassword;
  doc["sta_ssid"] = staSsid;
  doc["sta_pass"] = staPassword;
  doc["cloud_url"] = cloudUrl;
  doc["device_id"] = deviceId;
  doc["device_token"] = deviceToken;

  JsonArray states = doc.createNestedArray("states");
  JsonArray names = doc.createNestedArray("names");
  for (int i = 0; i < RELAY_COUNT; i++) {
    states.add(relayState[i]);
    names.add(relayName[i]);
  }

  JsonArray sch = doc.createNestedArray("schedules");
  for (int i = 0; i < scheduleCount; i++) {
    JsonObject o = sch.createNestedObject();
    o["enabled"] = schedules[i].enabled;
    o["relay"] = schedules[i].relay;
    o["hour"] = schedules[i].hour;
    o["minute"] = schedules[i].minute;
    o["action"] = schedules[i].action;
    o["days"] = schedules[i].days;
  }

  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) return false;
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

void loadConfig() {
  loadDefaults();
  if (!LittleFS.exists(CONFIG_PATH)) return;
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return;
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;

  if (doc.containsKey("ap_ssid")) strlcpy(apSsid, doc["ap_ssid"] | DEFAULT_AP_SSID, sizeof(apSsid));
  if (doc.containsKey("ap_pass")) strlcpy(apPassword, doc["ap_pass"] | DEFAULT_AP_PASSWORD, sizeof(apPassword));
  if (doc.containsKey("sta_ssid")) strlcpy(staSsid, doc["sta_ssid"] | "", sizeof(staSsid));
  if (doc.containsKey("sta_pass")) strlcpy(staPassword, doc["sta_pass"] | "", sizeof(staPassword));
  if (doc.containsKey("cloud_url")) strlcpy(cloudUrl, doc["cloud_url"] | "", sizeof(cloudUrl));
  if (doc.containsKey("device_id")) strlcpy(deviceId, doc["device_id"] | "", sizeof(deviceId));
  if (doc.containsKey("device_token")) strlcpy(deviceToken, doc["device_token"] | "", sizeof(deviceToken));

  JsonArray states = doc["states"].as<JsonArray>();
  JsonArray names = doc["names"].as<JsonArray>();
  for (int i = 0; i < RELAY_COUNT; i++) {
    if (states && i < (int)states.size()) relayState[i] = states[i] ? 1 : 0;
    if (names && i < (int)names.size()) {
      const char *n = names[i];
      if (n && n[0]) strlcpy(relayName[i], n, sizeof(relayName[i]));
    }
  }

  scheduleCount = 0;
  JsonArray sch = doc["schedules"].as<JsonArray>();
  if (sch) {
    for (JsonObject o : sch) {
      if (scheduleCount >= MAX_SCHEDULES) break;
      int relay = o["relay"] | 0;
      int hour = o["hour"] | -1;
      int minute = o["minute"] | -1;
      if (relay < 1 || relay > RELAY_COUNT || hour < 0 || hour > 23 || minute < 0 || minute > 59) continue;
      Schedule &s = schedules[scheduleCount++];
      s.enabled = o["enabled"] | true;
      s.relay = relay;
      s.hour = hour;
      s.minute = minute;
      s.action = (o["action"] | 0) ? 1 : 0;
      s.days = o["days"] | 127;
    }
  }
}

/* ---------------- Validation ---------------- */

bool validSsid(const char *s) { size_t n = strlen(s); return n >= 1 && n <= MAX_SSID_LEN; }
bool validPassword(const char *s) { size_t n = strlen(s); return n >= 8 && n <= MAX_PASS_LEN; }
bool validName(const char *s) { size_t n = strlen(s); return n >= 1 && n <= MAX_NAME_LEN; }
bool validUrl(const char *s) { return strncmp(s, "https://", 8) == 0; }
bool cloudConfigured() { return cloudUrl[0] && deviceId[0] && deviceToken[0]; }

/* ---------------- Relay / switch control ---------------- */

void applyRelay(int idx) {
  digitalWrite(RELAY_GPIO[idx], relayState[idx] ? RELAY_ACTIVE_LEVEL : !RELAY_ACTIVE_LEVEL);
}

void applyAllRelays() {
  for (int i = 0; i < RELAY_COUNT; i++) applyRelay(i);
}

void setRelay(int idx, int state) {
  if (idx < 0 || idx >= RELAY_COUNT) return;
  if (relayState[idx] == state) return;
  relayState[idx] = state;
  applyRelay(idx);
  saveConfig();
}

/* Non-blocking debounce for each switch, checked every loop() iteration. */
int switchLastRaw[SWITCH_COUNT];
int switchStable[SWITCH_COUNT];
unsigned long switchChangeAt[SWITCH_COUNT];
bool switchInit = false;

void pollSwitches() {
  unsigned long nowMs = millis();
  for (int i = 0; i < SWITCH_COUNT; i++) {
    int raw = digitalRead(SWITCH_GPIO[i]);
    if (!switchInit) {
      switchLastRaw[i] = raw;
      switchChangeAt[i] = nowMs;
      switchStable[i] = raw;
      /* Adopt the physical switch position immediately at boot -- this
       * makes the wall switch authoritative, exactly like a normal
       * mechanical light switch after a power cut. */
      setRelay(i, raw == SWITCH_ACTIVE_LEVEL ? 1 : 0);
      continue;
    }
    if (raw != switchLastRaw[i]) {
      switchLastRaw[i] = raw;
      switchChangeAt[i] = nowMs;
    } else if (raw != switchStable[i] && (nowMs - switchChangeAt[i]) >= SWITCH_DEBOUNCE_MS) {
      switchStable[i] = raw;
      setRelay(i, raw == SWITCH_ACTIVE_LEVEL ? 1 : 0);
    }
  }
  switchInit = true;
}

/* ---------------- Schedule manager ---------------- */

int lastScheduleMinuteKey = -1;

void checkSchedules() {
  if (!timeSynced) return;
  time_t nowT = time(nullptr);
  if (nowT < 1700000000) return;
  struct tm tmv;
  localtime_r(&nowT, &tmv);
  int minuteKey = tmv.tm_yday * 1440 + tmv.tm_hour * 60 + tmv.tm_min;
  if (minuteKey == lastScheduleMinuteKey) return;
  lastScheduleMinuteKey = minuteKey;

  int dayBit = 1 << tmv.tm_wday;
  for (int i = 0; i < scheduleCount; i++) {
    Schedule &s = schedules[i];
    if (!s.enabled) continue;
    if (s.hour != tmv.tm_hour || s.minute != tmv.tm_min) continue;
    if (!(s.days & dayBit)) continue;
    setRelay(s.relay - 1, s.action);
  }
}

/* ---------------- Web UI (served from PROGMEM) ---------------- */

const char PAGE_HTML[] PROGMEM = R"HTMLPAGE(
<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP8266 Smart Home</title>
<style>
:root{--bg:#f3f5f7;--card:#fff;--text:#17202a;--muted:#697586;--line:#e5e7eb;--on:#168a4b;--accent:#2563eb}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif}
.wrap{width:min(640px,100%);margin:auto;padding:18px 14px 34px}
h1{font-size:24px;margin:0 0 4px}.sub{color:var(--muted);font-size:14px;margin-bottom:14px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;margin:12px 0}
.row{display:flex;align-items:center;justify-content:space-between;gap:14px}
.name{font-weight:650;font-size:16px}.state{font-size:13px;color:var(--muted);margin-top:3px}
.switch{position:relative;width:54px;height:30px;flex:none}.switch input{opacity:0;width:0;height:0}
.slider{position:absolute;inset:0;background:#c8ced5;border-radius:40px;transition:.15s;cursor:pointer}
.slider:before{content:'';position:absolute;width:24px;height:24px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.15s}
input:checked+.slider{background:var(--on)}input:checked+.slider:before{transform:translateX(24px)}
button{border:1px solid var(--line);background:#fff;border-radius:9px;padding:9px 12px;font:inherit;cursor:pointer}
button.primary{background:var(--accent);border-color:var(--accent);color:#fff}
input[type=text],input[type=password],input[type=number],select{width:100%;padding:10px;border:1px solid #d5dae0;border-radius:9px;font:inherit}
label.f{display:block;font-size:13px;color:var(--muted);margin:11px 0 5px}
.tabs{display:flex;gap:8px;margin:14px 0 4px;flex-wrap:wrap}
.tab{padding:8px 12px;border:1px solid var(--line);border-radius:9px;background:#fff;cursor:pointer;font-size:13px}
.tab.active{background:var(--accent);color:#fff;border-color:var(--accent)}
.hidden{display:none}
.msg{font-size:13px;margin-top:8px;color:var(--muted)}
.sched-item{border-top:1px solid var(--line);padding:10px 0;font-size:14px}
.sched-item:first-child{border-top:0}
.daybtns{display:flex;gap:5px;margin-top:6px;flex-wrap:wrap}
.daybtn{width:34px;height:30px;border:1px solid var(--line);border-radius:7px;background:#fff;font-size:12px;cursor:pointer}
.daybtn.on{background:var(--accent);color:#fff;border-color:var(--accent)}
.pill{font-size:11px;padding:2px 8px;border-radius:20px;background:#eef1f4;color:var(--muted)}
.pill.ok{background:#e6f6ec;color:var(--on)}
</style></head><body><main class="wrap">
<h1>Smart Home</h1><div class="sub">ESP8266 local + cloud control</div>

<section id="relays"></section>

<div class="tabs">
<div class="tab active" onclick="showTab('sched')">Schedules</div>
<div class="tab" onclick="showTab('wifi')">Wi-Fi</div>
<div class="tab" onclick="showTab('cloud')">Internet</div>
<div class="tab" onclick="showTab('ota')">Firmware Update</div>
</div>

<section id="sched" class="card">
<div class="row"><div class="name">Schedules</div><span id="cloudPill" class="pill">Local only</span></div>
<div id="schedList"></div>
<label class="f">Relay</label><select id="sRelay"></select>
<label class="f">Time</label><input type="time" id="sTime" value="07:00">
<label class="f">Action</label><select id="sAction"><option value="1">Turn ON</option><option value="0">Turn OFF</option></select>
<label class="f">Repeat on</label>
<div class="daybtns" id="dayBtns"></div>
<div style="margin-top:12px"><button class="primary" onclick="addSchedule()">Add Schedule</button></div>
<div id="schedMsg" class="msg"></div>
</section>

<section id="wifi" class="card hidden">
<div class="name">Home Wi-Fi</div>
<div class="sub">Save this alone -- Internet fields below are optional and independent.</div>
<label class="f">SSID</label><input id="staSsid" maxlength="32">
<label class="f">Password (8-63 characters)</label><input id="staPass" type="password" maxlength="63">
<div style="margin-top:12px"><button class="primary" onclick="saveWifi()">Save Wi-Fi</button></div>
<div id="wifiMsg" class="msg"></div>
</section>

<section id="cloud" class="card hidden">
<div class="name">Internet / Remote Control</div>
<div class="sub">Fill all three fields to enable remote control. Leave empty to stay local-only.</div>
<label class="f">Cloud API URL</label><input id="cloudUrl" placeholder="https://your-worker.workers.dev">
<label class="f">Device ID</label><input id="deviceId">
<label class="f">Device Token</label><input id="deviceToken" type="password">
<div style="margin-top:12px"><button class="primary" onclick="saveCloud()">Save Internet Settings</button></div>
<div id="cloudMsg" class="msg"></div>
</section>

<section id="ota" class="card hidden">
<div class="name">Local Firmware Update</div>
<label class="f">Firmware .bin</label><input id="fw" type="file" accept=".bin">
<div style="margin-top:12px"><button class="primary" onclick="uploadFw()">Upload &amp; Restart</button></div>
<div id="otaMsg" class="msg"></div>
</section>

<div class="sub" style="margin-top:16px">ESP8266 local AP</div>
</main>
<script>
const DAY_LABELS=['Su','Mo','Tu','We','Th','Fr','Sa'];
let selDays=new Set([0,1,2,3,4,5,6]);
let relayNames=[];

const esc=function(s){return String(s).replace(/[&<>'"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;',"'":'&#39;','"':'&quot;'}[c]))};

const renderDayBtns=function(){
  let h='';
  DAY_LABELS.forEach((d,i)=>{h+=`<button type="button" class="daybtn ${selDays.has(i)?'on':''}" onclick="toggleDay(${i})">${d}</button>`});
  document.getElementById('dayBtns').innerHTML=h;
};
const toggleDay=function(i){if(selDays.has(i))selDays.delete(i);else selDays.add(i);renderDayBtns();};

const loadStatus=async function(){
  try{
    let r=await fetch('/api/status',{cache:'no-store'});
    let d=await r.json();
    relayNames=d.names;
    let h='';
    d.states.forEach((v,i)=>{
      h+=`<section class="card"><div class="row"><div><div class="name">${esc(d.names[i])}</div><div class="state">${v?'ON':'OFF'}</div></div><label class="switch"><input type="checkbox" ${v?'checked':''} onchange="setRelay(${i+1},this.checked)"><span class="slider"></span></label></div></section>`;
    });
    document.getElementById('relays').innerHTML=h;
    let sel=document.getElementById('sRelay');
    if(sel.options.length!==d.names.length){
      sel.innerHTML=d.names.map((n,i)=>`<option value="${i+1}">${esc(n)}</option>`).join('');
    }
    document.getElementById('cloudPill').textContent=d.cloudConfigured?(d.cloudOnline?'Cloud online':'Cloud configured'):'Local only';
    document.getElementById('cloudPill').className='pill'+(d.cloudConfigured?' ok':'');
  }catch(e){}
};
const setRelay=async function(n,on){
  try{await fetch(`/api/relay?relay=${n}&state=${on?1:0}`,{cache:'no-store'});await loadStatus();}catch(e){alert('Command failed');}
};

const loadSchedules=async function(){
  try{
    let r=await fetch('/api/schedules',{cache:'no-store'});let d=await r.json();
    let h='';
    d.schedules.forEach(s=>{
      let days=DAY_LABELS.filter((_,i)=>s.days&(1<<i)).join(',');
      let rn=relayNames[s.relay-1]||('Relay '+s.relay);
      h+=`<div class="sched-item"><div class="row"><div>${esc(rn)} &rarr; ${s.action?'ON':'OFF'} at ${String(s.hour).padStart(2,'0')}:${String(s.minute).padStart(2,'0')}<br><span style="color:var(--muted);font-size:12px">${days||'no days'}</span></div><button onclick="delSchedule(${s.id})">Delete</button></div></div>`;
    });
    document.getElementById('schedList').innerHTML=h||'<div class="msg">No schedules yet.</div>';
  }catch(e){}
};
const addSchedule=async function(){
  let m=document.getElementById('schedMsg');
  let relay=parseInt(document.getElementById('sRelay').value);
  let t=document.getElementById('sTime').value.split(':');
  let action=parseInt(document.getElementById('sAction').value);
  let days=0;selDays.forEach(i=>days|=(1<<i));
  if(days===0){m.textContent='Pick at least one day.';return;}
  m.textContent='Saving...';
  try{
    let r=await fetch('/api/schedules',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({relay,hour:parseInt(t[0]),minute:parseInt(t[1]),action,days,enabled:true})});
    if(!r.ok)throw 0;
    m.textContent='Schedule added.';await loadSchedules();
  }catch(e){m.textContent='Could not save schedule.';}
};
const delSchedule=async function(id){
  try{await fetch('/api/schedules?id='+id,{method:'DELETE'});await loadSchedules();}catch(e){}
};

const saveWifi=async function(){
  let m=document.getElementById('wifiMsg');
  let ssid=document.getElementById('staSsid').value,pass=document.getElementById('staPass').value;
  if(!ssid||pass.length<8){m.textContent='Enter SSID and an 8+ character password.';return;}
  m.textContent='Saving and reconnecting...';
  try{
    let r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,password:pass})});
    if(!r.ok)throw 0;m.textContent='Saved. Connecting to your Wi-Fi...';
  }catch(e){m.textContent='Save failed.';}
};
const saveCloud=async function(){
  let m=document.getElementById('cloudMsg');
  let url=document.getElementById('cloudUrl').value.trim();
  let id=document.getElementById('deviceId').value.trim();
  let token=document.getElementById('deviceToken').value.trim();
  if(!url&&!id&&!token){m.textContent='Leave all three empty to stay local-only, or fill all three to enable remote control.';return;}
  if(!url.startsWith('https://')||id.length<3||token.length<16){m.textContent='Fill in a valid HTTPS URL, Device ID and Device Token.';return;}
  m.textContent='Saving...';
  try{
    let r=await fetch('/api/cloud',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({url,deviceId:id,deviceToken:token})});
    if(!r.ok)throw 0;m.textContent='Saved. Remote control will activate once connected.';
  }catch(e){m.textContent='Save failed.';}
};
const uploadFw=function(){
  let f=document.getElementById('fw').files[0],m=document.getElementById('otaMsg');
  if(!f){m.textContent='Select a .bin file first.';return;}
  let pw=prompt('Enter OTA update password:');if(pw===null)return;
  if(!confirm('Start firmware update? Device will restart after success.'))return;
  m.textContent='Uploading...';
  let xhr=new XMLHttpRequest();
  xhr.open('POST','/api/ota',true);
  xhr.setRequestHeader('X-OTA-Password',pw);
  xhr.setRequestHeader('Content-Type','application/octet-stream');
  xhr.onload=function(){m.textContent=xhr.status>=200&&xhr.status<300?'Update successful. Restarting...':'Update failed.';};
  xhr.onerror=function(){m.textContent='Upload interrupted.';};
  xhr.send(f);
};
const showTab=function(id){
  ['sched','wifi','cloud','ota'].forEach(t=>document.getElementById(t).classList.toggle('hidden',t!==id));
  document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));
  event.target.classList.add('active');
};
renderDayBtns();
loadStatus();loadSchedules();
setInterval(loadStatus,800);
</script></body></html>
)HTMLPAGE";

/* ---------------- HTTP handlers ---------------- */

void sendJson(int code, const String &json) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", json);
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", PAGE_HTML);
}

void handleStatus() {
  DynamicJsonDocument doc(1024);
  JsonArray states = doc.createNestedArray("states");
  JsonArray names = doc.createNestedArray("names");
  for (int i = 0; i < RELAY_COUNT; i++) { states.add(relayState[i]); names.add(relayName[i]); }
  doc["cloudConfigured"] = cloudConfigured();
  doc["cloudOnline"] = staConnected && cloudConfigured();
  doc["staConnected"] = staConnected;
  String out; serializeJson(doc, out);
  sendJson(200, out);
}

void handleRelay() {
  if (otaInProgress) { sendJson(409, "{\"error\":\"OTA in progress\"}"); return; }
  if (!server.hasArg("relay") || !server.hasArg("state")) { sendJson(400, "{\"error\":\"missing params\"}"); return; }
  int relay = server.arg("relay").toInt();
  int state = server.arg("state").toInt();
  if (relay < 1 || relay > RELAY_COUNT || (state != 0 && state != 1)) { sendJson(400, "{\"error\":\"invalid params\"}"); return; }
  setRelay(relay - 1, state);
  sendJson(200, "{\"ok\":true}");
}

void handleWifiPost() {
  if (server.hasArg("plain") == false) { sendJson(400, "{\"error\":\"missing body\"}"); return; }
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, server.arg("plain"))) { sendJson(400, "{\"error\":\"bad json\"}"); return; }
  const char *ssid = doc["ssid"] | "";
  const char *pass = doc["password"] | "";
  if (!validSsid(ssid) || !validPassword(pass)) { sendJson(400, "{\"error\":\"invalid ssid/password\"}"); return; }
  strlcpy(staSsid, ssid, sizeof(staSsid));
  strlcpy(staPassword, pass, sizeof(staPassword));
  saveConfig();
  sendJson(200, "{\"ok\":true}");
  WiFi.begin(staSsid, staPassword); // reconnect without a full reboot
}

void handleCloudPost() {
  if (server.hasArg("plain") == false) { sendJson(400, "{\"error\":\"missing body\"}"); return; }
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, server.arg("plain"))) { sendJson(400, "{\"error\":\"bad json\"}"); return; }
  const char *url = doc["url"] | "";
  const char *id = doc["deviceId"] | "";
  const char *token = doc["deviceToken"] | "";
  /* Only two states are allowed: all three empty (local-only) or all three
   * valid (remote control enabled). This is what keeps Wi-Fi save fully
   * independent from the cloud fields. */
  bool allEmpty = (url[0] == '\0' && id[0] == '\0' && token[0] == '\0');
  bool allValid = validUrl(url) && strlen(id) >= 3 && strlen(id) <= MAX_DEVICE_ID_LEN &&
                  strlen(token) >= 16 && strlen(token) <= MAX_DEVICE_TOKEN_LEN;
  if (!allEmpty && !allValid) { sendJson(400, "{\"error\":\"fill in a valid https url, device id and token, or leave all blank\"}"); return; }
  strlcpy(cloudUrl, url, sizeof(cloudUrl));
  strlcpy(deviceId, id, sizeof(deviceId));
  strlcpy(deviceToken, token, sizeof(deviceToken));
  saveConfig();
  sendJson(200, "{\"ok\":true}");
}

void handleSchedulesGet() {
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.createNestedArray("schedules");
  for (int i = 0; i < scheduleCount; i++) {
    JsonObject o = arr.createNestedObject();
    o["id"] = i;
    o["enabled"] = schedules[i].enabled;
    o["relay"] = schedules[i].relay;
    o["hour"] = schedules[i].hour;
    o["minute"] = schedules[i].minute;
    o["action"] = schedules[i].action;
    o["days"] = schedules[i].days;
  }
  String out; serializeJson(doc, out);
  sendJson(200, out);
}

void handleSchedulesPost() {
  if (scheduleCount >= MAX_SCHEDULES) { sendJson(400, "{\"error\":\"schedule limit reached (20)\"}"); return; }
  if (server.hasArg("plain") == false) { sendJson(400, "{\"error\":\"missing body\"}"); return; }
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, server.arg("plain"))) { sendJson(400, "{\"error\":\"bad json\"}"); return; }
  int relay = doc["relay"] | 0;
  int hour = doc["hour"] | -1;
  int minute = doc["minute"] | -1;
  if (relay < 1 || relay > RELAY_COUNT || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    sendJson(400, "{\"error\":\"invalid schedule\"}"); return;
  }
  Schedule &s = schedules[scheduleCount++];
  s.enabled = doc["enabled"] | true;
  s.relay = relay;
  s.hour = hour;
  s.minute = minute;
  s.action = (doc["action"] | 0) ? 1 : 0;
  s.days = doc["days"] | 127;
  saveConfig();
  sendJson(200, "{\"ok\":true}");
}

void handleSchedulesDelete() {
  if (!server.hasArg("id")) { sendJson(400, "{\"error\":\"missing id\"}"); return; }
  int id = server.arg("id").toInt();
  if (id < 0 || id >= scheduleCount) { sendJson(400, "{\"error\":\"invalid id\"}"); return; }
  for (int i = id; i < scheduleCount - 1; i++) schedules[i] = schedules[i + 1];
  scheduleCount--;
  saveConfig();
  sendJson(200, "{\"ok\":true}");
}

void handleOta() {
  HTTPUpload &upload = server.upload();
  static bool authOk = false;
  if (upload.status == UPLOAD_FILE_START) {
    authOk = false;
    if (otaInProgress) return;
    String pw = server.header("X-OTA-Password");
    if (pw != OTA_UPDATE_PASSWORD) return;
    authOk = true;
    otaInProgress = true;
    Update.begin(0xFFFFFFFF, U_FLASH);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (authOk) Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (authOk) Update.end(true);
    otaInProgress = false;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (authOk) Update.end(false);
    otaInProgress = false;
  }
}

void handleOtaDone() {
  if (!otaInProgress && Update.hasError() == false && Update.isRunning() == false) {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", "OTA successful. Restarting...");
    delay(800);
    ESP.restart();
  } else {
    server.send(500, "text/plain", "OTA failed.");
  }
}

void handleCaptive() {
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

/* ---------------- Cloud polling ---------------- */

void pollCloud() {
  if (!staConnected || !cloudConfigured()) return;
  if (millis() - lastCloudPoll < CLOUD_POLL_INTERVAL_MS) return;
  lastCloudPoll = millis();

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure(); // NOTE: skips certificate validation -- acceptable for a
                          // token-authenticated hobby deployment; for stronger
                          // security, pin the Cloudflare CA certificate instead.
  HTTPClient https;
  String url = String(cloudUrl) + "/api/device/poll";
  if (!https.begin(*client, url)) return;
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Authorization", String("Bearer ") + deviceToken);

  DynamicJsonDocument reqDoc(512);
  reqDoc["deviceId"] = deviceId;
  JsonArray states = reqDoc.createNestedArray("states");
  JsonArray enabled = reqDoc.createNestedArray("enabled");
  for (int i = 0; i < RELAY_COUNT; i++) { states.add(relayState[i]); enabled.add(true); }
  for (int i = RELAY_COUNT; i < 5; i++) { states.add(0); enabled.add(false); }
  String body; serializeJson(reqDoc, body);

  int code = https.POST(body);
  if (code >= 200 && code < 300) {
    String resp = https.getString();
    DynamicJsonDocument respDoc(4096);
    if (!deserializeJson(respDoc, resp)) {
      JsonArray cmds = respDoc["commands"].as<JsonArray>();
      if (cmds) {
        for (JsonObject c : cmds) {
          int relay = c["relay"] | 0;
          int state = c["state"] | 0;
          if (relay >= 1 && relay <= RELAY_COUNT) setRelay(relay - 1, state ? 1 : 0);
        }
      }
      JsonVariant ota = respDoc["ota"];
      if (!ota.isNull()) {
        const char *otaUrlVal = ota["url"] | "";
        if (otaUrlVal[0] && strncmp(otaUrlVal, "https://", 8) == 0) {
          std::unique_ptr<BearSSL::WiFiClientSecure> otaClient(new BearSSL::WiFiClientSecure);
          otaClient->setInsecure();
          ESPhttpUpdate.update(*otaClient, otaUrlVal);
        }
      }
    }
  }
  https.end();
}

/* ---------------- Wi-Fi setup ---------------- */

void setupWifi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(apSsid, apPassword, 6, 0, AP_MAX_CONNECTIONS);
  if (staSsid[0]) WiFi.begin(staSsid, staPassword);

  dnsServer.start(DNS_PORT, "*", IPAddress(192, 168, 4, 1));
}

/* ---------------- setup / loop ---------------- */

void setup() {
  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }
  loadConfig();

  for (int i = 0; i < RELAY_COUNT; i++) {
    pinMode(RELAY_GPIO[i], OUTPUT);
    digitalWrite(RELAY_GPIO[i], !RELAY_ACTIVE_LEVEL); // safe OFF before restoring state
  }
  for (int i = 0; i < SWITCH_COUNT; i++) {
    pinMode(SWITCH_GPIO[i], INPUT_PULLUP);
  }

  setupWifi();

  server.collectHeaders("X-OTA-Password");

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/relay", HTTP_GET, handleRelay);
  server.on("/api/wifi", HTTP_POST, handleWifiPost);
  server.on("/api/cloud", HTTP_POST, handleCloudPost);
  server.on("/api/schedules", HTTP_GET, handleSchedulesGet);
  server.on("/api/schedules", HTTP_POST, handleSchedulesPost);
  server.on("/api/schedules", HTTP_DELETE, handleSchedulesDelete);
  server.on("/api/ota", HTTP_POST, handleOtaDone, handleOta);
  server.on("/generate_204", HTTP_GET, handleCaptive);
  server.on("/hotspot-detect.html", HTTP_GET, handleCaptive);
  server.on("/connecttest.txt", HTTP_GET, handleCaptive);
  server.on("/ncsi.txt", HTTP_GET, handleCaptive);
  server.onNotFound(handleCaptive);
  server.begin();
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  pollSwitches();

  bool nowConnected = (WiFi.status() == WL_CONNECTED);
  if (nowConnected && !staConnected) {
    staConnected = true;
    configTime("IST-5:30", "pool.ntp.org", "time.google.com");
  }
  if (!nowConnected) staConnected = false;

  if (staConnected && !timeSynced) {
    time_t nowT = time(nullptr);
    if (nowT > 1700000000) timeSynced = true;
  }

  checkSchedules();
  pollCloud();
}
