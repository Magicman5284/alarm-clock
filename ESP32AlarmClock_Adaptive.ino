#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <TM1637Display.h>
#include <DFRobotDFPlayerMini.h>
#include <time.h>
#include <sys/time.h>

// put your wifi here plz
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";


const bool ENABLE_FALLBACK_AP = false;
const char* AP_SSID     = "ESP32-AlarmClock";
const char* AP_PASSWORD = "alarmclock123"; 

const char* MDNS_HOSTNAME = "alarmclock";


const char* TZ_INFO = "EST5EDT,M3.2.0/2,M11.1.0/2";

// Pin assignments from the current PCB.
static const uint8_t TM1637_CLK_PIN = 12;
static const uint8_t TM1637_DIO_PIN = 14;
static const uint8_t BUTTON_PIN     = 27;


static const int DFPLAYER_RX_PIN = 16;
static const int DFPLAYER_TX_PIN = 17;

static const uint8_t DFPLAYER_VOLUME = 24;
static const uint8_t MAX_ALARMS = 9;
static const uint16_t BUTTON_DEBOUNCE_MS = 35;
static const uint32_t DISPLAY_OVERLAY_MS = 5000;


TM1637Display display(TM1637_CLK_PIN, TM1637_DIO_PIN);
DFRobotDFPlayerMini dfPlayer;
WebServer server(80);
Preferences prefs;
bool mdnsStarted = false;
uint32_t lastWifiReconnectAttempt = 0;

struct AlarmEntry {
  bool active = false;
  char type = 0;
  char value[8] = {0};
  char hhmm[5] = {0};
  int lastFiredDate = 0;
};

AlarmEntry alarms[MAX_ALARMS];
bool dfPlayerReady = false;
bool alarmRinging = false;
bool alarmWasTimer = false;
bool timerActive = false;
uint32_t timerStartedMs = 0;
uint32_t timerDurationMs = 0;

bool stopwatchRunning = false;
uint32_t stopwatchStartedMs = 0;
uint32_t stopwatchElapsedMs = 0;

enum DisplayMode : uint8_t { DISPLAY_CLOCK, DISPLAY_NEXT_ALARM, DISPLAY_STOPWATCH_RESULT };
DisplayMode displayMode = DISPLAY_CLOCK;
uint32_t displayModeStartedMs = 0;
int displayOverlayHHMM = -1; // -1 means show dashes
int64_t lastEvaluatedMinuteKey = INT64_MIN;
uint32_t lastDisplayUpdateMs = 0;

int lastButtonReading = HIGH;
int stableButtonState = HIGH;
uint32_t lastButtonChangeMs = 0;

const uint8_t BLANK_SEGMENTS[4] = {0x00, 0x00, 0x00, 0x00};
const uint8_t DASH_SEGMENTS[4]  = {0x40, 0x40, 0x40, 0x40};


const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Alarm Clock</title>
<style>
:root{color-scheme:dark;--bg:#101820;--panel:#1c2935;--line:#344554;--accent:#35c3ad;--text:#edf4f7;--muted:#aabac4}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:16px system-ui,-apple-system,Segoe UI,sans-serif}
main{max-width:760px;margin:auto;padding:20px}h1{font-size:1.7rem;margin:.2rem 0}.sub{color:var(--muted);margin:0 0 18px}
section{background:var(--panel);padding:16px;border:1px solid var(--line);border-radius:14px;margin:14px 0}
h2{font-size:1.15rem;margin:0 0 12px}label{display:block;color:var(--muted);font-size:.92rem;margin:8px 0 5px}
input,button{font:inherit;border-radius:8px;border:1px solid var(--line);padding:10px;background:#0d151c;color:var(--text);width:100%}
button{background:var(--accent);border:0;color:#06211e;font-weight:700;margin-top:10px;cursor:pointer}button.secondary{background:#314453;color:var(--text)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(145px,1fr));gap:10px}.days{display:flex;flex-wrap:wrap;gap:8px}.days label{background:#0d151c;border:1px solid var(--line);border-radius:9px;padding:8px;color:var(--text);margin:0}.days input{width:auto;margin-right:4px}
pre{white-space:pre-wrap;overflow-wrap:anywhere;line-height:1.5;background:#0d151c;border:1px solid var(--line);border-radius:8px;padding:12px;min-height:46px}
#status{color:var(--muted);white-space:pre-wrap}small{color:var(--muted)}
</style></head><body><main>
<h1>ESP32 Alarm Clock</h1><p class="sub">Local Wi-Fi control panel · 24-hour time · Eastern time zone</p>
<section><h2>Clock status</h2><div id="status">Contacting clock…</div><button class="secondary" onclick="runCmd('status')">Refresh status</button></section>
<section><h2>One-time alarm</h2><small>Payload format: ddmmyyHHMM, for example 3010260715 = 30 Oct 2026 at 07:15.</small>
<div class="grid"><div><label for="singleDate">Date</label><input id="singleDate" type="date" required></div><div><label for="singleTime">Time (24-hour)</label><input id="singleTime" type="time" value="07:00" required></div></div>
<button onclick="addSingle()">Add one-time alarm</button></section>
<section><h2>Repeating alarm</h2><label>Days (1 = Monday … 7 = Sunday)</label>
<div class="days">
<label><input type="checkbox" class="day" value="1">Mon</label><label><input type="checkbox" class="day" value="2">Tue</label><label><input type="checkbox" class="day" value="3">Wed</label><label><input type="checkbox" class="day" value="4">Thu</label><label><input type="checkbox" class="day" value="5">Fri</label><label><input type="checkbox" class="day" value="6">Sat</label><label><input type="checkbox" class="day" value="7">Sun</label>
</div><label for="repeatTime">Time (24-hour)</label><input id="repeatTime" type="time" value="07:00" required>
<button onclick="addRepeat()">Add repeating alarm</button></section>
<section><h2>Timer</h2><small>Enter hours, minutes and seconds. The display shows HH:MM above one hour remaining and MM:SS below one hour. The alarm sound plays when time runs out.</small>
<div class="grid"><div><label for="th">Hours (00–99)</label><input id="th" type="number" min="0" max="99" value="0"></div><div><label for="tm">Minutes (00–59)</label><input id="tm" type="number" min="0" max="59" value="5"></div><div><label for="ts">Seconds (00–59)</label><input id="ts" type="number" min="0" max="59" value="0"></div></div>
<button onclick="startTimer()">Start timer</button><button class="secondary" onclick="runCmd('stop-alarm')">Stop sounding alarm</button></section>
<section><h2>Stopwatch</h2><p>Press the button to start; press it again to stop. The clock button also stops it and flashes the result.</p>
<button onclick="runCmd('stopwatch')">Start / stop stopwatch</button></section>
<section><h2>Alarm list and cancellation</h2><div class="grid"><button onclick="showList('list')">Show alarms</button><button class="secondary" onclick="showList('cancel-list')">Cancel list</button></div>
<pre id="alarms">Use Show alarms to list scheduled alarms.</pre><div class="grid"><div><label for="cancelNumber">Alarm number (1–9)</label><input id="cancelNumber" type="number" min="1" max="9" step="1" value="1"></div><div style="align-self:end"><button onclick="cancelAlarm()">Cancel selected number</button></div></div></section>
<section><h2>Response</h2><pre id="response">Ready.</pre></section>
</main><script>
async function api(cmd, params={}) { const q=new URLSearchParams({cmd,...params}); const r=await fetch('/command?'+q.toString(),{cache:'no-store'}); const t=await r.text(); document.getElementById('response').textContent=t; return t; }
async function runCmd(cmd){await api(cmd); await refreshStatus();}
async function refreshStatus(){try{const q=new URLSearchParams({cmd:'status'});const r=await fetch('/command?'+q,{cache:'no-store'});document.getElementById('status').textContent=await r.text();}catch(e){document.getElementById('status').textContent='Connection error: '+e;}}
async function showList(cmd){const t=await api(cmd);document.getElementById('alarms').textContent=t;await refreshStatus();}
async function getListQuietly(){const r=await fetch('/command?cmd=list',{cache:'no-store'});return await r.text();}
function timeDigits(s){return (s||'').replace(':','');}
async function addSingle(){const d=document.getElementById('singleDate').value;const t=document.getElementById('singleTime').value;if(!d||!t){alert('Choose a date and time.');return;}const p=d.split('-');const payload=p[2]+p[1]+p[0].slice(-2)+timeDigits(t);const r=await api('single',{payload});if(r.startsWith('All set!'))document.getElementById('alarms').textContent=await getListQuietly();await refreshStatus();}
async function addRepeat(){const days=Array.from(document.querySelectorAll('.day:checked')).map(x=>x.value).join('');const time=timeDigits(document.getElementById('repeatTime').value);if(!days){alert('Choose at least one day.');return;}if(time.length!==4){alert('Choose a valid time.');return;}const r=await api('repeat',{payload:days,payload2:time});if(r.startsWith('All set!'))document.getElementById('alarms').textContent=await getListQuietly();await refreshStatus();}
async function startTimer(){const nums=['th','tm','ts'].map(id=>Number(document.getElementById(id).value||0));if(nums.some(n=>!Number.isInteger(n)||n<0)||nums[0]>99||nums[1]>59||nums[2]>59||(nums[0]+nums[1]+nums[2]===0)){alert('Enter a timer from 00:00:01 to 99:59:59.');return;}const payload=nums.map(n=>String(n).padStart(2,'0')).join('');await api('timer',{payload});await refreshStatus();}
async function cancelAlarm(){const n=document.getElementById('cancelNumber').value;await api('cancel',{payload:n});document.getElementById('alarms').textContent=await getListQuietly();await refreshStatus();}
async function syncDeviceClock(){try{await fetch('/command?'+new URLSearchParams({cmd:'sync-time',payload:String(Math.floor(Date.now()/1000))}),{cache:'no-store'});}catch(e){}await refreshStatus();}
(function init(){const now=new Date();document.getElementById('singleDate').value=now.getFullYear()+'-'+String(now.getMonth()+1).padStart(2,'0')+'-'+String(now.getDate()).padStart(2,'0');syncDeviceClock();setInterval(refreshStatus,10000);})();
</script></body></html>
)rawliteral";


bool clockTimeValid() {
  return time(nullptr) > 1700000000;
}

bool allDigits(const String& s) {
  if (s.length() == 0) return false;
  for (size_t i = 0; i < s.length(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

bool validHHMM(const String& s) {
  if (s.length() != 4 || !allDigits(s)) return false;
  int hh = s.substring(0, 2).toInt();
  int mm = s.substring(2, 4).toInt();
  return hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59;
}

int daysInMonth(int year, int month) {
  static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (month < 1 || month > 12) return 0;
  if (month == 2) {
    bool leap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
    return leap ? 29 : 28;
  }
  return days[month - 1];
}

bool validDDMMYY(const String& s) {
  if (s.length() != 6 || !allDigits(s)) return false;
  int dd = s.substring(0, 2).toInt();
  int mm = s.substring(2, 4).toInt();
  int yy = s.substring(4, 6).toInt();
  int year = 2000 + yy;
  return mm >= 1 && mm <= 12 && dd >= 1 && dd <= daysInMonth(year, mm);
}

bool isSinglePayloadValid(const String& p) {
  if (p.length() != 10 || !allDigits(p)) return false;
  return validDDMMYY(p.substring(0, 6)) && validHHMM(p.substring(6, 10));
}

String normalizeDays(const String& input) {
  if (input.length() < 1 || input.length() > 7 || !allDigits(input)) return "";
  bool seen[8] = {false, false, false, false, false, false, false, false};
  String output;
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    if (c < '1' || c > '7') return "";
    int d = c - '0';
    if (!seen[d]) {
      seen[d] = true;
      output += c;
    }
  }
  return output;
}

int dateKeyFromTm(const tm& t) {
  return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}

int isoWeekday(const tm& t) {

  return t.tm_wday == 0 ? 7 : t.tm_wday;
}

bool makeTargetTime(const char* ddmmyy, const char* hhmm, tm& target) {
  if (!ddmmyy || !hhmm) return false;
  String d = String(ddmmyy);
  String h = String(hhmm);
  if (!validDDMMYY(d) || !validHHMM(h)) return false;
  memset(&target, 0, sizeof(target));
  int dd = d.substring(0, 2).toInt();
  int mm = d.substring(2, 4).toInt();
  int yy = d.substring(4, 6).toInt();
  target.tm_mday = dd;
  target.tm_mon = mm - 1;
  target.tm_year = (2000 + yy) - 1900;
  target.tm_hour = h.substring(0, 2).toInt();
  target.tm_min = h.substring(2, 4).toInt();
  target.tm_sec = 0;
  target.tm_isdst = -1;
  return true;
}

bool singleAlarmIsFuture(const String& payload) {
  if (!clockTimeValid()) return true; // The web page will sync the clock on opening.
  tm target = {};
  if (!makeTargetTime(payload.substring(0, 6).c_str(), payload.substring(6, 10).c_str(), target)) return false;
  time_t epoch = mktime(&target);
  return epoch > time(nullptr);
}

String formatLocalDateTime() {
  if (!clockTimeValid()) return "Time not synchronized";
  time_t now = time(nullptr);
  tm local;
  localtime_r(&now, &local);
  char b[32];
  strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &local);
  return String(b);
}


void clearEntry(AlarmEntry& a) {
  a.active = false;
  a.type = 0;
  a.value[0] = '\0';
  a.hhmm[0] = '\0';
  a.lastFiredDate = 0;
}

void persistAlarms() {
  for (int i = 0; i < MAX_ALARMS; ++i) {
    char key[5];
    snprintf(key, sizeof(key), "a%d", i);
    if (!alarms[i].active) {
      prefs.remove(key);
      continue;
    }
    char value[64];
    snprintf(value, sizeof(value), "%c|%s|%s|%d", alarms[i].type,
             alarms[i].value, alarms[i].hhmm, alarms[i].lastFiredDate);
    prefs.putString(key, value);
  }
}

void loadAlarms() {
  for (int i = 0; i < MAX_ALARMS; ++i) {
    clearEntry(alarms[i]);
    char key[5];
    snprintf(key, sizeof(key), "a%d", i);
    String rec = prefs.getString(key, "");
    if (rec.length() == 0 || rec.length() > 60) continue;
    char buf[64];
    rec.toCharArray(buf, sizeof(buf));
    char* saveptr = nullptr;
    char* type = strtok_r(buf, "|", &saveptr);
    char* value = strtok_r(nullptr, "|", &saveptr);
    char* hm = strtok_r(nullptr, "|", &saveptr);
    char* last = strtok_r(nullptr, "|", &saveptr);
    if (!type || !value || !hm || !last || strlen(type) != 1) continue;
    if (type[0] == 'S') {
      if (!validDDMMYY(String(value)) || !validHHMM(String(hm))) continue;
    } else if (type[0] == 'R') {
      if (normalizeDays(String(value)).length() == 0 || !validHHMM(String(hm))) continue;
    } else continue;
    alarms[i].active = true;
    alarms[i].type = type[0];
    snprintf(alarms[i].value, sizeof(alarms[i].value), "%s", value);
    snprintf(alarms[i].hhmm, sizeof(alarms[i].hhmm), "%s", hm);
    alarms[i].lastFiredDate = atoi(last);
  }
}

int activeAlarmCount() {
  int n = 0;
  for (const auto& a : alarms) if (a.active) ++n;
  return n;
}

int findFreeAlarmSlot() {
  for (int i = 0; i < MAX_ALARMS; ++i) if (!alarms[i].active) return i;
  return -1;
}

String dayNames(const char* days) {
  static const char* names[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
  String out;
  for (size_t i = 0; i < strlen(days); ++i) {
    int d = days[i] - '1';
    if (d < 0 || d > 6) continue;
    if (out.length()) out += ", ";
    out += names[d];
  }
  return out;
}

String describeAlarm(const AlarmEntry& a) {
  String hhmm(a.hhmm);
  hhmm = hhmm.substring(0, 2) + ":" + hhmm.substring(2, 4);
  if (a.type == 'S') {
    String d(a.value);
    return "One-time " + d.substring(0, 2) + "/" + d.substring(2, 4) + "/20" + d.substring(4, 6) + " at " + hhmm;
  }
  return "Repeats " + dayNames(a.value) + " at " + hhmm;
}

String alarmListEntriesText() {
  String result;
  int number = 0;
  for (int i = 0; i < MAX_ALARMS; ++i) {
    if (!alarms[i].active) continue;
    ++number;
    result += String(number) + ". " + describeAlarm(alarms[i]) + "\n";
  }
  if (number == 0) result = "You don't have any alarms scheduled right now.\n";
  return result;
}

int slotForListNumber(int number) {
  if (number < 1 || number > MAX_ALARMS) return -1;
  int current = 0;
  for (int i = 0; i < MAX_ALARMS; ++i) {
    if (!alarms[i].active) continue;
    ++current;
    if (current == number) return i;
  }
  return -1;
}

String addSingleAlarm(const String& payload) {
  if (!isSinglePayloadValid(payload))
    return "I couldn't set that alarm. Please send exactly 10 digits in ddmmyyHHMM format, with a real date and a valid 24-hour time.";
  if (!singleAlarmIsFuture(payload))
    return "I couldn't set that alarm because its date and time have already passed. Please choose a future local date and time.";
  int slot = findFreeAlarmSlot();
  if (slot < 0)
    return "I couldn't add another alarm because all 9 alarm slots are being used. Cancel an alarm first, then try again.";
  clearEntry(alarms[slot]);
  alarms[slot].active = true;
  alarms[slot].type = 'S';
  String d = payload.substring(0, 6);
  String hm = payload.substring(6, 10);
  snprintf(alarms[slot].value, sizeof(alarms[slot].value), "%s", d.c_str());
  snprintf(alarms[slot].hhmm, sizeof(alarms[slot].hhmm), "%s", hm.c_str());
  persistAlarms();
  return "All set! Your one-time alarm is scheduled for " + describeAlarm(alarms[slot]) + ".";
}

String addRepeatingAlarm(const String& rawDays, const String& hm) {
  String days = normalizeDays(rawDays);
  if (days.length() == 0)
    return "I couldn't set that repeating alarm. Send one or more weekday digits from 1 through 7 (1=Monday, 7=Sunday).";
  if (!validHHMM(hm))
    return "I couldn't set that repeating alarm. The time must be exactly four digits in HHMM 24-hour format, such as 0715.";
  int slot = findFreeAlarmSlot();
  if (slot < 0)
    return "I couldn't add another alarm because all 9 alarm slots are being used. Cancel an alarm first, then try again.";
  clearEntry(alarms[slot]);
  alarms[slot].active = true;
  alarms[slot].type = 'R';
  snprintf(alarms[slot].value, sizeof(alarms[slot].value), "%s", days.c_str());
  snprintf(alarms[slot].hhmm, sizeof(alarms[slot].hhmm), "%s", hm.c_str());
  persistAlarms();
  return "All set! Your alarm will repeat on " + dayNames(alarms[slot].value) + " at " +
         String(alarms[slot].hhmm).substring(0, 2) + ":" +
         String(alarms[slot].hhmm).substring(2, 4) + ".";
}

String cancelAlarmNumber(const String& payload) {
  if (payload.length() != 1 || payload[0] < '1' || payload[0] > '9')
    return "I couldn't cancel that alarm. Send one digit from 1 to 9, using the number shown in the latest alarm list.";
  int number = payload.toInt();
  int slot = slotForListNumber(number);
  if (slot < 0)
    return "I couldn't find an alarm with that number. Please request the alarm list again and choose a number from the current list.";
  String oldDescription = describeAlarm(alarms[slot]);
  clearEntry(alarms[slot]);
  persistAlarms();
  return "Done! I've cancelled alarm number " + String(number) + ": " + oldDescription + ".";
}


void showDashes() {
  display.setSegments(DASH_SEGMENTS);
}

void showBlank() {
  display.setSegments(BLANK_SEGMENTS);
}

void showHHMM(int hhmm, bool colon = true) {
  if (hhmm < 0) {
    showDashes();
    return;
  }
  display.showNumberDecEx(hhmm, colon ? 0x40 : 0x00, true);
}

void showAdaptiveDuration(uint32_t totalSeconds) {
  if (totalSeconds < 3600UL) {
    uint32_t minutes = totalSeconds / 60UL;
    uint32_t seconds = totalSeconds % 60UL;
    display.showNumberDecEx((int)(minutes * 100UL + seconds), 0x40, true);
  } else {
    uint32_t hours = (totalSeconds / 3600UL) % 100UL;
    uint32_t minutes = (totalSeconds / 60UL) % 60UL;
    display.showNumberDecEx((int)(hours * 100UL + minutes), 0x40, true);
  }
}

void showElapsedOnDisplay(uint32_t elapsedMs) {

  showAdaptiveDuration(elapsedMs / 1000UL);
}

void showTimerRemainingOnDisplay(uint32_t remainingMs) {

  uint32_t remainingSeconds = (remainingMs + 999UL) / 1000UL;


  if (remainingMs < 3600000UL && remainingSeconds >= 3600UL) {
    remainingSeconds = 3599UL;
  }

  showAdaptiveDuration(remainingSeconds);
}

bool getNextScheduledAlarm(time_t now, tm& bestLocal) {
  if (!clockTimeValid()) return false;
  tm nowLocal;
  localtime_r(&now, &nowLocal);
  time_t bestEpoch = 0;
  bool found = false;

  for (int i = 0; i < MAX_ALARMS; ++i) {
    if (!alarms[i].active) continue;
    if (alarms[i].type == 'S') {
      tm target = {};
      if (!makeTargetTime(alarms[i].value, alarms[i].hhmm, target)) continue;
      time_t candidate = mktime(&target);
      if (candidate > now && (!found || candidate < bestEpoch)) {
        bestEpoch = candidate;
        found = true;
      }
    } else if (alarms[i].type == 'R') {
      
      for (int offset = 0; offset <= 7; ++offset) {
        tm day = nowLocal;
        day.tm_mday += offset;
        day.tm_hour = 12;
        day.tm_min = 0;
        day.tm_sec = 0;
        day.tm_isdst = -1;
        time_t dayEpoch = mktime(&day);
        tm normalizedDay;
        localtime_r(&dayEpoch, &normalizedDay);
        int isoDay = isoWeekday(normalizedDay);
        char digit = char('0' + isoDay);
        if (strchr(alarms[i].value, digit) == nullptr) continue;
        tm target = normalizedDay;
        target.tm_hour = (alarms[i].hhmm[0] - '0') * 10 + (alarms[i].hhmm[1] - '0');
        target.tm_min  = (alarms[i].hhmm[2] - '0') * 10 + (alarms[i].hhmm[3] - '0');
        target.tm_sec = 0;
        target.tm_isdst = -1;
        time_t candidate = mktime(&target);
        if (candidate > now && (!found || candidate < bestEpoch)) {
          bestEpoch = candidate;
          found = true;
        }
      }
    }
  }

  if (found) localtime_r(&bestEpoch, &bestLocal);
  return found;
}

void setNextAlarmDisplay() {
  tm next;
  if (getNextScheduledAlarm(time(nullptr), next)) {
    displayOverlayHHMM = next.tm_hour * 100 + next.tm_min;
  } else {
    displayOverlayHHMM = -1;
  }
  displayMode = DISPLAY_NEXT_ALARM;
  displayModeStartedMs = millis();
}

void updateDisplay() {
  uint32_t nowMs = millis();
  if (nowMs - lastDisplayUpdateMs < 100) return;
  lastDisplayUpdateMs = nowMs;


  if (timerActive) {
    uint32_t elapsed = nowMs - timerStartedMs;
    uint32_t remaining = elapsed >= timerDurationMs ? 0 : timerDurationMs - elapsed;
    showTimerRemainingOnDisplay(remaining);
    return;
  }

  if (stopwatchRunning) {
    showElapsedOnDisplay(nowMs - stopwatchStartedMs);
    return;
  }

  if (displayMode == DISPLAY_NEXT_ALARM) {
    if (nowMs - displayModeStartedMs >= DISPLAY_OVERLAY_MS) {
      displayMode = DISPLAY_CLOCK;
    } else {
      showHHMM(displayOverlayHHMM, true);
      return;
    }
  } else if (displayMode == DISPLAY_STOPWATCH_RESULT) {
    if (nowMs - displayModeStartedMs >= DISPLAY_OVERLAY_MS) {
      displayMode = DISPLAY_CLOCK;
    } else if (((nowMs - displayModeStartedMs) / 500UL) % 2UL == 0) {
      showElapsedOnDisplay(stopwatchElapsedMs);
      return;
    } else {
      showBlank();
      return;
    }
  }

  if (!clockTimeValid()) {
    showDashes();
    return;
  }
  time_t current = time(nullptr);
  tm local;
  localtime_r(&current, &local);
  int hhmm = local.tm_hour * 100 + local.tm_min;
  showHHMM(hhmm, (local.tm_sec % 2) == 0);
}


void startAlarm(bool fromTimer) {
  if (alarmRinging) return;
  alarmRinging = true;
  alarmWasTimer = fromTimer;
  displayMode = DISPLAY_CLOCK;
  if (dfPlayerReady) {
    dfPlayer.stop();
    delay(80);
    dfPlayer.playMp3Folder(1); // SD:/MP3/0001.mp3
    dfPlayer.enableLoop();
  }
}

void stopAlarm() {
  if (dfPlayerReady) {
    dfPlayer.disableLoop();
    dfPlayer.stop();
  }
  alarmRinging = false;
  alarmWasTimer = false;
  displayMode = DISPLAY_CLOCK;
}

void startStopwatch() {
  stopwatchRunning = true;
  stopwatchStartedMs = millis();
  stopwatchElapsedMs = 0;
  displayMode = DISPLAY_CLOCK;
}

String elapsedAsText(uint32_t elapsedMs) {
  uint32_t sec = elapsedMs / 1000UL;
  uint32_t h = sec / 3600UL;
  uint32_t m = (sec / 60UL) % 60UL;
  uint32_t s = sec % 60UL;
  char b[24];
  snprintf(b, sizeof(b), "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
  return String(b);
}

String stopStopwatch(bool flashResult) {
  if (!stopwatchRunning) return "Stopwatch is not running.";
  stopwatchElapsedMs = millis() - stopwatchStartedMs;
  stopwatchRunning = false;
  if (flashResult) {
    displayMode = DISPLAY_STOPWATCH_RESULT;
    displayModeStartedMs = millis();
  } else {
    displayMode = DISPLAY_CLOCK;
  }
  return "Stopwatch elapsed: " + elapsedAsText(stopwatchElapsedMs);
}

void handleButtonPress() {
  if (alarmRinging) {
    bool wasTimer = alarmWasTimer;
    stopAlarm();
    if (wasTimer) timerActive = false;
    
    setNextAlarmDisplay();
    return;
  }
  if (stopwatchRunning) {
    stopStopwatch(true);
    return;
  }
  if (timerActive) {
    timerActive = false;
    displayMode = DISPLAY_CLOCK;
    return;
  }
  
}

void pollButton() {
  int reading = digitalRead(BUTTON_PIN);
  uint32_t now = millis();
  if (reading != lastButtonReading) {
    lastButtonChangeMs = now;
    lastButtonReading = reading;
  }
  if ((now - lastButtonChangeMs) > BUTTON_DEBOUNCE_MS && reading != stableButtonState) {
    stableButtonState = reading;
    if (stableButtonState == LOW) handleButtonPress();
  }
}

void checkScheduledAlarms() {
  if (!clockTimeValid()) return;
  time_t now = time(nullptr);
  tm local;
  localtime_r(&now, &local);
  int64_t minuteKey = (((int64_t)(local.tm_year + 1900) * 10000LL +
                        (int64_t)(local.tm_mon + 1) * 100LL + local.tm_mday) * 1440LL) +
                      local.tm_hour * 60LL + local.tm_min;
  if (minuteKey == lastEvaluatedMinuteKey) return;
  lastEvaluatedMinuteKey = minuteKey;

  char currentDate[7];
  snprintf(currentDate, sizeof(currentDate), "%02d%02d%02d", local.tm_mday,
           local.tm_mon + 1, (local.tm_year + 1900) % 100);
  char currentHM[5];
  snprintf(currentHM, sizeof(currentHM), "%02d%02d", local.tm_hour, local.tm_min);
  int dateKey = dateKeyFromTm(local);
  int isoDay = isoWeekday(local);
  bool fired = false;
  bool changed = false;

  for (int i = 0; i < MAX_ALARMS; ++i) {
    if (!alarms[i].active) continue;
    if (strcmp(alarms[i].hhmm, currentHM) != 0) continue;
    if (alarms[i].type == 'S' && strcmp(alarms[i].value, currentDate) == 0) {
      clearEntry(alarms[i]);
      changed = true;
      fired = true;
    } else if (alarms[i].type == 'R' && strchr(alarms[i].value, char('0' + isoDay)) != nullptr &&
               alarms[i].lastFiredDate != dateKey) {
      alarms[i].lastFiredDate = dateKey;
      changed = true;
      fired = true;
    }
  }
  if (changed) persistAlarms();
  if (fired && !alarmRinging) startAlarm(false);
}

void checkTimer() {
  if (!timerActive) return;
  uint32_t elapsed = millis() - timerStartedMs;
  if (elapsed >= timerDurationMs) {
    timerActive = false;
    startAlarm(true);
  }
}


String statusText() {
  String s = "Here's your alarm clock status:\n";
  s += "Local time: " + formatLocalDateTime();
  if (WiFi.status() == WL_CONNECTED) {
    s += "\nWi-Fi: connected to " + String(WIFI_SSID);
    s += "\nYour clock's address: http://" + WiFi.localIP().toString() + "/";
    s += "\nFriendly address: http://" + String(MDNS_HOSTNAME) + ".local/ (if supported)";
  } else {
    s += "\nWi-Fi: not connected to your router yet.";
  }
  if (ENABLE_FALLBACK_AP) {
    s += "\nBackup Wi-Fi network: " + String(AP_SSID) + " (http://192.168.4.1/)";
  }
  s += "\nScheduled alarms: " + String(activeAlarmCount()) + " of " + String(MAX_ALARMS);
  s += "\nAlarm sound: "; s += alarmRinging ? "currently playing" : "not playing";
  s += "\nTimer: ";
  if (timerActive) {
    uint32_t elapsed = millis() - timerStartedMs;
    uint32_t remain = elapsed >= timerDurationMs ? 0 : timerDurationMs - elapsed;
    uint32_t totalSec = (remain + 999UL) / 1000UL;
    char b[24];
    snprintf(b, sizeof(b), "%02lu:%02lu:%02lu remaining",
             (unsigned long)(totalSec / 3600UL),
             (unsigned long)((totalSec / 60UL) % 60UL),
             (unsigned long)(totalSec % 60UL));
    s += b;
  } else s += "not running";
  s += "\nStopwatch: ";
  if (stopwatchRunning) s += "running (" + elapsedAsText(millis() - stopwatchStartedMs) + " elapsed)";
  else s += "not running";
  if (alarmRinging) s += "\nYour alarm is ringing. Press the clock button or send stop-alarm to silence it.";
  if (!dfPlayerReady) s += "\nA note: the DFPlayer wasn't detected at startup, so alarm audio may not play. Check its wiring, SD card, and power.";
  return s;
}

void sendText(int code, const String& message) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "text/plain; charset=utf-8", message);
}

void handleCommand() {
  String cmd = server.arg("cmd");
  cmd.toLowerCase();
  String payload = server.hasArg("payload") ? server.arg("payload") : "";
  if (payload.length() == 0 && server.hasArg("payload1")) payload = server.arg("payload1");
  String payload2 = server.hasArg("payload2") ? server.arg("payload2") : "";

  if (cmd == "single" || cmd == "single-alarm") {
    String result = addSingleAlarm(payload);
    sendText(result.startsWith("I couldn't") ? 400 : 200, result);

  } else if (cmd == "repeat" || cmd == "repeated-alarm") {
    String result = addRepeatingAlarm(payload, payload2);
    sendText(result.startsWith("I couldn't") ? 400 : 200, result);

  } else if (cmd == "cancel-list") {
    sendText(200, "Here are your scheduled alarms. Use the number beside an alarm if you'd like to cancel it:\n\n" + alarmListEntriesText());

  } else if (cmd == "list" || cmd == "alarm-list") {
    sendText(200, "Here are all of your scheduled alarms:\n\n" + alarmListEntriesText());

  } else if (cmd == "cancel" || cmd == "cancel-alarm") {
    String result = cancelAlarmNumber(payload);
    sendText(result.startsWith("I couldn't") ? 400 : 200, result);

  } else if (cmd == "timer") {
    if (payload.length() != 6 || !allDigits(payload)) {
      sendText(400, "I couldn't start that timer. Please send exactly six digits in hhmmss format, such as 000500 for five minutes.");
      return;
    }
    int hh = payload.substring(0, 2).toInt();
    int mm = payload.substring(2, 4).toInt();
    int ss = payload.substring(4, 6).toInt();
    if (mm > 59 || ss > 59 || (hh == 0 && mm == 0 && ss == 0)) {
      sendText(400, "I couldn't start that timer. Use a duration from 000001 (one second) through 995959 (99 hours, 59 minutes, 59 seconds). Minutes and seconds must be 00–59.");
      return;
    }
    bool replacingTimer = timerActive;
    timerDurationMs = ((uint32_t)hh * 3600UL + (uint32_t)mm * 60UL + (uint32_t)ss) * 1000UL;
    timerStartedMs = millis();
    timerActive = true;
    String friendlyDuration = payload.substring(0, 2) + ":" + payload.substring(2, 4) + ":" + payload.substring(4, 6);
    sendText(200, replacingTimer ?
      "Your timer has been updated! I'll alert you in " + friendlyDuration + "." :
      "Timer started! I'll let you know when " + friendlyDuration + " has passed.");

  } else if (cmd == "stopwatch") {
    if (!stopwatchRunning) {
      startStopwatch();
      sendText(200, "Stopwatch started! I'm timing from now. Send stopwatch again or press the clock button to stop it.");
    } else {
      stopwatchElapsedMs = millis() - stopwatchStartedMs;
      stopwatchRunning = false;
      displayMode = DISPLAY_STOPWATCH_RESULT;
      displayModeStartedMs = millis();
      sendText(200, "Stopwatch stopped! Your elapsed time was " + elapsedAsText(stopwatchElapsedMs) + ". I'll flash the result on the display for about five seconds.");
    }

  } else if (cmd == "stop-alarm" || cmd == "stop") {
    if (alarmRinging) {
      bool stoppedTimerAlarm = alarmWasTimer;
      stopAlarm();
      if (stoppedTimerAlarm) timerActive = false;
      sendText(200, "All quiet! I've stopped the alarm sound.");
    } else {
      sendText(200, "No alarm is sounding right now, so there's nothing to stop.");
    }

  } else if (cmd == "sync-time") {
    if (!allDigits(payload) || payload.length() < 10 || payload.length() > 11) {
      sendText(400, "I couldn't update the clock time. Send the current Unix time as a 10- or 11-digit number of seconds.");
      return;
    }
    long long epoch = strtoll(payload.c_str(), nullptr, 10);
    if (epoch < 1700000000LL || epoch > 4102444800LL) {
      sendText(400, "I couldn't update the clock time because that timestamp is outside the supported date range.");
      return;
    }
    timeval tv;
    tv.tv_sec = (time_t)epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    sendText(200, "Clock time updated successfully! The local time is now " + formatLocalDateTime() + ".");

  } else if (cmd == "status") {
    sendText(200, statusText());

  } else {
    sendText(400, "I didn't recognize that command. Try single, repeat, list, cancel-list, cancel, timer, stopwatch, stop-alarm, status, or sync-time.");
  }
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleNotFound() {
  String message = "Not found. Open the ESP32 alarm clock control page at /";
  if (WiFi.status() == WL_CONNECTED) {
    message += "\nRouter URL: http://" + WiFi.localIP().toString() + "/";
    message += "\nHostname URL: http://" + String(MDNS_HOSTNAME) + ".local/ (if supported)";
  }
  if (ENABLE_FALLBACK_AP) message += "\nFallback URL: http://192.168.4.1/";
  sendText(404, message);
}

bool routerCredentialsConfigured() {
  return strlen(WIFI_SSID) > 0 && strcmp(WIFI_SSID, "YOUR_WIFI_NAME") != 0;
}

void startMDNSIfConnected() {
  if (mdnsStarted || WiFi.status() != WL_CONNECTED) return;

  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
    Serial.print("mDNS control URL: http://");
    Serial.print(MDNS_HOSTNAME);
    Serial.println(".local/");
  } else {
    Serial.println("mDNS startup failed; use the router IP shown below.");
  }
}

void maintainWiFiConnection() {
  if (!routerCredentialsConfigured()) return;

  if (WiFi.status() == WL_CONNECTED) {
    startMDNSIfConnected();
    return;
  }


  if (millis() - lastWifiReconnectAttempt >= 15000UL) {
    lastWifiReconnectAttempt = millis();
    Serial.println("Wi-Fi disconnected; attempting to reconnect...");
    WiFi.reconnect();
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("ESP32 Wi-Fi Alarm Clock starting...");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  display.setBrightness(7, true);
  showDashes();

  prefs.begin("alarmclock", false);
  loadAlarms();
  Serial.printf("Loaded %d saved alarm(s).\n", activeAlarmCount());

  Serial2.begin(9600, SERIAL_8N1, DFPLAYER_RX_PIN, DFPLAYER_TX_PIN);
  dfPlayer.setTimeOut(500);
  if (dfPlayer.begin(Serial2, true, true)) {
    dfPlayerReady = true;
    dfPlayer.volume(DFPLAYER_VOLUME);
    dfPlayer.EQ(DFPLAYER_EQ_NORMAL);
    dfPlayer.outputDevice(DFPLAYER_DEVICE_SD);
    dfPlayer.disableLoop();
    Serial.println("DFPlayer initialized.");
  } else {
    dfPlayerReady = false;
    Serial.println("WARNING: DFPlayer did not respond. Clock/API will still run; alarm audio will not.");
  }

  WiFi.setHostname(MDNS_HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.mode(ENABLE_FALLBACK_AP ? WIFI_AP_STA : WIFI_STA);

  if (ENABLE_FALLBACK_AP) {
    bool apOk = WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.printf("Fallback AP %s: %s\n", AP_SSID, apOk ? "started" : "FAILED");
    Serial.print("Fallback AP URL: http://");
    Serial.println(WiFi.softAPIP());
  }

  if (routerCredentialsConfigured()) {
    Serial.printf("Connecting to your router Wi-Fi (%s)...\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastWifiReconnectAttempt = millis();

    uint32_t startWait = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startWait < 20000UL) {
      delay(250);
      Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Connected to local Wi-Fi.");
      Serial.print("ESP32 Wi-Fi MAC address (for router DHCP reservation): ");
      Serial.println(WiFi.macAddress());
      Serial.print("Use this control URL in Apple Shortcuts: http://");
      Serial.print(WiFi.localIP());
      Serial.println("/");
      startMDNSIfConnected();
    } else {
      Serial.println("Could not connect yet. The ESP32 will retry automatically.");
      if (ENABLE_FALLBACK_AP) Serial.println("Fallback control URL: http://192.168.4.1/");
    }
  } else {
    Serial.println("Wi-Fi credentials are still placeholders.");
    Serial.println("Edit WIFI_SSID and WIFI_PASSWORD, then upload again.");
    if (ENABLE_FALLBACK_AP) Serial.println("Fallback control URL: http://192.168.4.1/");
  }

  configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov", "time.google.com");
  server.on("/", HTTP_GET, handleRoot);
  server.on("/command", HTTP_GET, handleCommand);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started on port 80.");
}


void loop() {
  server.handleClient();
  maintainWiFiConnection();
  pollButton();
  checkTimer();
  checkScheduledAlarms();
  updateDisplay();
  delay(2);
}
