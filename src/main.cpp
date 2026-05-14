#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <time.h>
#include <Adafruit_NeoPixel.h>
#include "secrets.h"

#define LED_PIN      48
#define LED_COUNT     1
#define NUM_ZONES     5
#define NUM_PROGRAMS  2
#define QUEUE_MAX    (NUM_ZONES * NUM_PROGRAMS)

Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

const bool  RELAY_ACTIVE_LOW = true;
static const char* TZ_PACIFIC = "PST8PDT,M3.2.0,M11.1.0";

uint8_t relayPins[NUM_ZONES]      = {4, 5, 6, 7, 15};
char    relayNames[NUM_ZONES][32] = {"Zone 1","Zone 2","Zone 3","Zone 4","Zone 5"};
bool    relayState[NUM_ZONES]     = {};

struct WateringProgram { bool enabled; uint8_t hour, minute, days; };
const char* PROG_NAMES[NUM_PROGRAMS] = {"Morning", "Afternoon"};
WateringProgram programs[NUM_PROGRAMS] = {
  {true,  5, 30, 0x7F},
  {true, 15, 30, 0x7F},
};
time_t progLastFired[NUM_PROGRAMS] = {};

uint16_t zoneDuration[NUM_ZONES][NUM_PROGRAMS] = {};

struct QEntry { uint8_t zone; uint32_t secs; uint8_t trigger; }; // trigger: 0=manual, 1+=program id+1
QEntry  qBuf[QUEUE_MAX];
int     qHead = 0, qSize = 0;
int8_t  activeZone      = -1;
time_t  activeEndTime   = 0;
time_t  activeStartTime = 0;
uint8_t activeTrigger   = 0;

// ESPAsyncWebServer callbacks run on Core 0; loop() runs on Core 1.
// stateMux guards all shared queue and active-zone state between the two tasks.
static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

#define HISTORY_MAX 200
struct HistoryEntry { time_t start; uint16_t duration; uint8_t zone; uint8_t trigger; };
HistoryEntry history[HISTORY_MAX];
int histHead = 0, histCount = 0;
uint8_t historyDays = 7;

#define TEMP_HIST_MAX 1008
struct TempSample { time_t t; float c; };
TempSample tempHist[TEMP_HIST_MAX];
int tempHistHead = 0, tempHistCount = 0;
unsigned long lastTempSample = 0;

Preferences    prefs;
AsyncWebServer server(80);

inline float toF(float c) { return c * 9.0f / 5.0f + 32.0f; }

void addTempSample(time_t t, float c) {
  int idx = (tempHistHead + tempHistCount) % TEMP_HIST_MAX;
  tempHist[idx] = {t, c};
  if (tempHistCount < TEMP_HIST_MAX) tempHistCount++;
  else tempHistHead = (tempHistHead + 1) % TEMP_HIST_MAX;
}

// ── Queue ─────────────────────────────────────────────────────────────────────

void enqueue(uint8_t z, uint32_t secs, uint8_t trigger = 0) {
  portENTER_CRITICAL(&stateMux);
  if (qSize < QUEUE_MAX)
    qBuf[(qHead + qSize++) % QUEUE_MAX] = {z, secs, trigger};
  portEXIT_CRITICAL(&stateMux);
}

QEntry dequeue() {  // must be called while holding stateMux
  QEntry e = qBuf[qHead];
  qHead = (qHead + 1) % QUEUE_MAX;
  qSize--;
  return e;
}

void clearQueue() {
  portENTER_CRITICAL(&stateMux);
  qSize = 0;
  portEXIT_CRITICAL(&stateMux);
}

void removeFromQueue(int z) {
  portENTER_CRITICAL(&stateMux);
  QEntry tmp[QUEUE_MAX]; int n = 0;
  for (int i = 0; i < qSize; i++) {
    QEntry e = qBuf[(qHead + i) % QUEUE_MAX];
    if (e.zone != (uint8_t)z) tmp[n++] = e;
  }
  qHead = 0; qSize = n;
  memcpy(qBuf, tmp, n * sizeof(QEntry));
  portEXIT_CRITICAL(&stateMux);
}

// ── LED ───────────────────────────────────────────────────────────────────────

void updateLED() {
  if (activeZone >= 0) {
    led.setBrightness(70);
    led.setPixelColor(0, led.Color(0, 0, 255));
  } else {
    led.setBrightness(30);
    led.setPixelColor(0, led.Color(0, 255, 0));
  }
  led.show();
}

// ── Relay ─────────────────────────────────────────────────────────────────────

void setRelay(int idx, bool on) {
  relayState[idx] = on;
  bool pinOn = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(relayPins[idx], pinOn ? HIGH : LOW);
  updateLED();
}

void purgeHistory() {
  time_t now; time(&now);
  time_t cutoff = now - (time_t)historyDays * 86400;
  if (histCount == 0 || history[histHead].start >= cutoff) return;
  HistoryEntry tmp[HISTORY_MAX]; int n = 0;
  for (int i = 0; i < histCount; i++) {
    HistoryEntry& e = history[(histHead + i) % HISTORY_MAX];
    if (e.start >= cutoff) tmp[n++] = e;
  }
  histHead = 0; histCount = n;
  memcpy(history, tmp, n * sizeof(HistoryEntry));
}

void addHistory(HistoryEntry e) {
  purgeHistory();
  if (histCount >= HISTORY_MAX) {
    history[histHead] = e;
    histHead = (histHead + 1) % HISTORY_MAX;
  } else {
    history[(histHead + histCount) % HISTORY_MAX] = e;
    histCount++;
  }
}

// requireZone >= 0 stops only if that zone is currently active; -1 stops whatever is active.
void stopActive(int requireZone = -1) {
  portENTER_CRITICAL(&stateMux);
  if (activeZone < 0 || (requireZone >= 0 && activeZone != requireZone)) {
    portEXIT_CRITICAL(&stateMux);
    return;
  }
  time_t now; time(&now);
  uint16_t dur   = (uint16_t)min((long)(now - activeStartTime), 65535L);
  int8_t   zone  = activeZone;
  uint8_t  trig  = activeTrigger;
  time_t   start = activeStartTime;
  activeZone = -1; activeEndTime = 0; activeStartTime = 0;
  portEXIT_CRITICAL(&stateMux);
  addHistory({start, dur, (uint8_t)zone, trig});
  setRelay(zone, false);
  Serial.printf("Zone %d (%s) done after %us\n", zone, relayNames[zone], dur);
}

void allOffFn() {
  clearQueue(); stopActive();
  for (int i = 0; i < NUM_ZONES; i++) setRelay(i, false);
}

// ── Persistence ───────────────────────────────────────────────────────────────

void saveConfig();
void loadConfig() {
  prefs.begin("irr", true);
  for (int i = 0; i < NUM_ZONES; i++) {
    char k[10];
    snprintf(k, sizeof(k), "n%d", i);
    String nm = prefs.getString(k, relayNames[i]); nm.toCharArray(relayNames[i], 32);
    snprintf(k, sizeof(k), "p%d", i); relayPins[i] = prefs.getUChar(k, relayPins[i]);
    for (int pr = 0; pr < NUM_PROGRAMS; pr++) {
      snprintf(k, sizeof(k), "d%d_%d", i, pr);
      zoneDuration[i][pr] = prefs.getUShort(k, 0);
    }
  }
  for (int pr = 0; pr < NUM_PROGRAMS; pr++) {
    char k[10];
    snprintf(k, sizeof(k), "pe%d", pr); programs[pr].enabled = prefs.getBool(k,   programs[pr].enabled);
    snprintf(k, sizeof(k), "ph%d", pr); programs[pr].hour    = prefs.getUChar(k,  programs[pr].hour);
    snprintf(k, sizeof(k), "pm%d", pr); programs[pr].minute  = prefs.getUChar(k,  programs[pr].minute);
    snprintf(k, sizeof(k), "pw%d", pr); programs[pr].days    = prefs.getUChar(k,  programs[pr].days);
  }
  historyDays = prefs.getUChar("hdays", 7);
  uint8_t cfgVer = prefs.getUChar("cfgVer", 0);
  prefs.end();
  if (cfgVer < 2) {
    for (int i = 0; i < NUM_ZONES; i++)
      for (int pr = 0; pr < NUM_PROGRAMS; pr++)
        if (zoneDuration[i][pr] > 0)
          zoneDuration[i][pr] = (uint16_t)min((uint32_t)zoneDuration[i][pr] * 60, (uint32_t)65535);
    saveConfig();
  }
}

void saveConfig() {
  prefs.begin("irr", false);
  for (int i = 0; i < NUM_ZONES; i++) {
    char k[10];
    snprintf(k, sizeof(k), "n%d", i);  prefs.putString(k, relayNames[i]);
    snprintf(k, sizeof(k), "p%d", i);  prefs.putUChar(k,  relayPins[i]);
    for (int pr = 0; pr < NUM_PROGRAMS; pr++) {
      snprintf(k, sizeof(k), "d%d_%d", i, pr); prefs.putUShort(k, zoneDuration[i][pr]);
    }
  }
  for (int pr = 0; pr < NUM_PROGRAMS; pr++) {
    char k[10];
    snprintf(k, sizeof(k), "pe%d", pr); prefs.putBool(k,   programs[pr].enabled);
    snprintf(k, sizeof(k), "ph%d", pr); prefs.putUChar(k,  programs[pr].hour);
    snprintf(k, sizeof(k), "pm%d", pr); prefs.putUChar(k,  programs[pr].minute);
    snprintf(k, sizeof(k), "pw%d", pr); prefs.putUChar(k,  programs[pr].days);
  }
  prefs.putUChar("hdays", historyDays);
  prefs.putUChar("cfgVer", 2);
  prefs.end();
}

// ── Scheduler ─────────────────────────────────────────────────────────────────

void checkSchedules() {
  time_t now; struct tm ti;
  time(&now); localtime_r(&now, &ti);
  for (int pr = 0; pr < NUM_PROGRAMS; pr++) {
    WateringProgram& p = programs[pr];
    if (!p.enabled) continue;
    if (!(p.days & (1 << ti.tm_wday))) continue;
    if (ti.tm_hour != p.hour || ti.tm_min != p.minute) continue;
    if (now - progLastFired[pr] < 120) continue;
    progLastFired[pr] = now;
    int n = 0;
    for (int z = 0; z < NUM_ZONES; z++)
      if (zoneDuration[z][pr] > 0) { enqueue(z, (uint32_t)zoneDuration[z][pr], pr + 1); n++; }
    Serial.printf("Program %d (%s): queued %d zones\n", pr, PROG_NAMES[pr], n);
  }
}

void runQueue() {
  time_t now; time(&now);

  portENTER_CRITICAL(&stateMux);
  bool needStop = (activeZone >= 0 && now >= activeEndTime);
  portEXIT_CRITICAL(&stateMux);
  if (needStop) stopActive();

  portENTER_CRITICAL(&stateMux);
  if (activeZone < 0 && qSize > 0) {
    QEntry e = dequeue();
    activeZone = e.zone; activeEndTime = now + e.secs;
    activeStartTime = now; activeTrigger = e.trigger;
    portEXIT_CRITICAL(&stateMux);
    setRelay(e.zone, true);
    Serial.printf("Queue: Zone %d (%s) ON for %lus\n", e.zone, relayNames[e.zone], (unsigned long)e.secs);
  } else {
    portEXIT_CRITICAL(&stateMux);
  }
}

static String jsonEsc(const char* s) {
  String r; r.reserve(strlen(s));
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') r += '\\';
    r += *s;
  }
  return r;
}

// ── Web page ──────────────────────────────────────────────────────────────────

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Irrigation Control</title>
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'><path d='M16 3C16 3 5 15 5 21.5C5 27.851 9.925 32 16 32s11-4.149 11-10.5C27 15 16 3 16 3z' fill='%230ea5e9'/><path d='M12 25c-1.5-1-2.5-3-2-5' stroke='white' stroke-width='1.5' fill='none' stroke-linecap='round'/></svg>">
<style>
*{box-sizing:border-box;margin:0;padding:0}
html{font-size:17px}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#0f172a;color:#e2e8f0;min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:1.5rem 1rem 3rem}
h1{font-size:1.4rem;font-weight:600;color:#7dd3fc;letter-spacing:.05em;text-transform:uppercase;margin-bottom:.25rem}
#clock{font-size:1.1rem;font-weight:600;color:#fff;margin-bottom:.3rem;letter-spacing:.04em}
.sec{font-size:.95rem;font-weight:700;letter-spacing:.1em;text-transform:uppercase;color:#94a3b8;margin:.2rem 0 .4rem;width:100%;max-width:500px}
.prog-grid{display:flex;flex-direction:column;gap:.75rem;width:100%;max-width:500px}
.pcard{background:#1e293b;border-radius:.875rem;padding:.9rem;border:1px solid #475569;transition:border-color .2s}
.pcard.on{border-color:#0284c7}
.phead{display:flex;align-items:center;justify-content:space-between;margin-bottom:.65rem}
.pname{font-size:.9rem;font-weight:700;color:#7dd3fc;letter-spacing:.08em;text-transform:uppercase}
.ptog{width:36px;height:20px;background:#334155;border-radius:20px;border:none;cursor:pointer;position:relative;transition:background .2s;flex-shrink:0}
.ptog::after{content:'';position:absolute;top:2px;left:2px;width:16px;height:16px;background:#fff;border-radius:50%;transition:transform .2s}
.ptog.on{background:#0284c7}.ptog.on::after{transform:translateX(16px)}
.ptime{width:100%;background:#0f172a;border:1px solid #475569;border-radius:.3rem;color:#e2e8f0;padding:.3rem .45rem;font-size:.85rem;margin-bottom:.5rem}
.ptime:focus{outline:none;border-color:#7dd3fc}
.days{display:flex;gap:.2rem;justify-content:center;margin-bottom:.6rem}
.day{width:26px;height:26px;border-radius:.25rem;border:1px solid #475569;background:#0f172a;color:#94a3b8;font-size:.72rem;font-weight:700;cursor:pointer;transition:background .15s,color .15s}
.day.on{background:#0369a1;color:#e0f2fe;border-color:#0369a1}
.sbtn{width:100%;background:#0369a1;color:#e0f2fe;border:none;border-radius:.3rem;padding:.35rem;font-size:.85rem;font-weight:600;cursor:pointer;letter-spacing:.04em;transition:background .15s}
.sbtn:hover{background:#0284c7}
.zone-grid{display:flex;flex-direction:column;gap:.75rem;width:100%;max-width:500px}
.zcard{background:#1e293b;border-radius:.875rem;padding:.9rem 1rem .5rem;border:1px solid #475569;transition:border-color .2s}
.zcard.active{border-color:#22c55e}
.zcard.queued{border-color:#f59e0b}
.ztop{display:flex;align-items:center;gap:.5rem;margin-bottom:.7rem}
.zname{font-size:1rem;font-weight:600;color:#e2e8f0;flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.zbadge{font-size:.58rem;font-weight:700;letter-spacing:.07em;padding:.15rem .4rem;border-radius:.25rem;text-transform:uppercase;white-space:nowrap}
.zbadge.active{background:#166534;color:#86efac}
.zbadge.queued{background:#78350f;color:#fcd34d}
.ztog{width:44px;height:24px;background:#334155;border-radius:24px;border:none;cursor:pointer;position:relative;transition:background .2s;flex-shrink:0}
.ztog::after{content:'';position:absolute;top:2px;left:2px;width:20px;height:20px;background:#fff;border-radius:50%;transition:transform .2s}
.ztog.on{background:#22c55e}.ztog.on::after{transform:translateX(20px)}
.ebtn{background:none;border:none;color:#64748b;cursor:pointer;font-size:.95rem;padding:.15rem;border-radius:.25rem;transition:color .15s;line-height:1;flex-shrink:0}
.ebtn:hover{color:#94a3b8}
.zdurs{display:grid;grid-template-columns:1fr 1fr;gap:.5rem;margin-bottom:.4rem}
.dfield{display:flex;flex-direction:column;gap:.2rem}
.zdurs .dfield:first-child{border-right:1px solid #334155;padding-right:.5rem}
.dlabel{font-size:.68rem;font-weight:600;letter-spacing:.07em;color:#e2e8f0;text-transform:uppercase}
.dur-row{display:flex;align-items:center;gap:.3rem}
.dur-row input{width:calc(4ch + 30px);min-width:0;background:#0f172a;border:1px solid #475569;border-radius:.3rem;color:#f8fafc;padding:.28rem .4rem;font-size:.82rem;text-align:center}
.dur-row input:focus{outline:none;border-color:#7dd3fc}
.dur-row span{font-size:.75rem;font-weight:600;color:#e2e8f0;white-space:nowrap}
.ep{display:none;flex-direction:column;gap:.45rem;border-top:1px solid #475569;padding-top:.65rem;margin-top:.4rem}
.ep.open{display:flex}
.ep label{font-size:.68rem;font-weight:600;color:#e2e8f0;letter-spacing:.07em;text-transform:uppercase;display:block;margin-bottom:.15rem}
.ep input[type=text],.ep input[type=number]{background:#0f172a;border:1px solid #475569;border-radius:.3rem;color:#f8fafc;padding:.3rem .45rem;font-size:.78rem;width:100%}
.ep input:focus{outline:none;border-color:#7dd3fc}
.footer{margin-top:1.25rem}
.alloff{padding:.65rem 1.75rem;background:transparent;color:#f87171;border:1px solid #7f1d1d;border-radius:.5rem;font-size:.82rem;font-weight:600;letter-spacing:.05em;cursor:pointer;transition:background .15s,border-color .15s,color .15s}
.alloff:hover{background:#3f1010;border-color:#b91c1c;color:#fca5a5}
.topbar{display:flex;align-items:center;width:100%;max-width:500px;position:relative;margin-bottom:.25rem}
.topbar h1{text-align:left}
#uptime{color:#94a3b8;font-size:.9rem;font-weight:600;letter-spacing:.03em;text-align:center;white-space:nowrap}
#chip-temp{color:#94a3b8;font-size:.9rem;font-weight:600;letter-spacing:.03em;text-align:right;white-space:nowrap}
body.light{background:#f1f5f9;color:#1e293b}
body.light h1{color:#0369a1}
body.light #clock{color:#1e293b}
body.light .sec{color:#475569}
body.light .pcard,body.light .zcard,body.light .modal{background:#fff;border-color:#94a3b8}
body.light .pcard.on{border-color:#0284c7}
body.light .zcard.active{border-color:#22c55e}
body.light .zcard.queued{border-color:#f59e0b}
body.light .pname{color:#0369a1}
body.light .zname{color:#1e293b}
body.light .ebtn{color:#94a3b8}
body.light .dlabel,body.light .ep label,body.light .dur-row span,body.light .ztog-label{color:#1e293b;font-weight:600}
body.light .zdurs .dfield:first-child{border-color:#cbd5e1}
body.light .ptog{background:#94a3b8}
body.light .ztog{background:#94a3b8}
body.light .ptime,body.light .dur-row input,body.light .ep input,body.light .modal input{background:#f8fafc;border-color:#94a3b8;color:#1e293b}
body.light .rnbtn{background:#f8fafc;color:#0369a1;border-color:#93c5fd}
body.light .rnbtn:hover:not(:disabled){background:#eff6ff}
body.light .zexpand{border-color:#94a3b8}
body.light .ep{border-color:#94a3b8}
body.light .mbtn-cancel{background:#e2e8f0;color:#475569}
body.light .modal-ov{background:rgba(0,0,0,.4)}
body.light .modal-title{color:#0369a1}
body.light #uptime{color:#475569}
body.light #chip-temp{color:#475569}
body.light .day{background:#f8fafc;border-color:#94a3b8;color:#475569}
body.light .day.on{background:#0369a1;color:#e0f2fe;border-color:#0369a1}
.info-row{display:flex;align-items:center;justify-content:space-between;margin-top:1.25rem;width:100%;max-width:500px;gap:.5rem;flex-wrap:nowrap}
.info-side{font-size:.9rem;font-weight:600;color:#94a3b8;text-decoration:none;flex:0 0 auto;display:flex;align-items:center;letter-spacing:.03em;white-space:nowrap;transition:color .15s}
.info-side:hover{color:#e2e8f0}
.info-center{font-size:.9rem;font-weight:600;color:#64748b;text-align:center;flex:1;letter-spacing:.03em;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.theme-tgl{display:flex;gap:.15rem;margin-left:auto}
.th-icon{background:none;border:none;font-size:1rem;cursor:pointer;padding:.15rem .2rem;border-radius:.25rem;opacity:.22;transition:opacity .2s;line-height:1}
.th-icon:hover{opacity:.5}
.log-row{display:flex;justify-content:center;margin:.75rem 0 .25rem;width:100%;max-width:500px}
.log-trigger{background:none;border:1px solid #334155;border-radius:.5rem;color:#64748b;font-size:1rem;font-weight:600;padding:.4rem 1.2rem;cursor:pointer;transition:background .15s,color .15s,border-color .15s;letter-spacing:.03em}
.log-trigger:hover{background:#1e293b;color:#94a3b8;border-color:#475569}
body.light .info-side{color:#475569}
body.light .info-side:hover{color:#0f172a}
body.light .info-center{color:#94a3b8}
body.light .log-trigger{border-color:#cbd5e1;color:#94a3b8}
body.light .log-trigger:hover{background:#f1f5f9;color:#475569;border-color:#94a3b8}
.log-ov{position:fixed;inset:0;background:rgba(0,0,0,.7);display:flex;align-items:flex-end;justify-content:center;z-index:200}
.log-modal{background:#0f172a;border-radius:1rem 1rem 0 0;width:100%;max-width:500px;max-height:90vh;display:flex;flex-direction:column}
.log-head{display:flex;align-items:center;justify-content:space-between;padding:.85rem 1rem;border-bottom:1px solid #475569;flex-shrink:0}
.log-head span{font-size:.88rem;font-weight:600;color:#e2e8f0}
.log-close{background:none;border:none;color:#94a3b8;cursor:pointer;font-size:1.1rem;line-height:1;padding:.2rem}
.log-close:hover{color:#94a3b8}
.log-body{overflow-y:auto;padding:.75rem 1rem 2rem;flex:1}
.log-empty{color:#64748b;font-size:.8rem;text-align:center;padding:2rem 0}
.log-day{margin-bottom:.35rem}
.log-day-btn{width:100%;text-align:left;background:none;border:none;color:#7dd3fc;font-size:.8rem;font-weight:700;cursor:pointer;padding:.45rem 0;letter-spacing:.04em}
.log-day-content{padding-left:.25rem}
.log-prog{margin-bottom:.5rem}
.log-prog-btn{width:100%;text-align:left;background:none;border:none;color:#94a3b8;font-size:.65rem;font-weight:700;letter-spacing:.1em;text-transform:uppercase;cursor:pointer;padding:.3rem 0}
.log-prog-content{padding-left:.35rem}
.log-entry{display:flex;align-items:center;gap:.4rem;padding:.22rem 0;border-bottom:1px solid #334155}
.log-zone{flex:1;font-size:.78rem;color:#e2e8f0}
.log-dur{font-size:.72rem;font-weight:600;color:#22c55e;min-width:38px;text-align:right}
.log-time{font-size:.68rem;color:#64748b;min-width:52px;text-align:right}
body.light .log-modal{background:#f8fafc}
body.light .log-head{border-color:#94a3b8}
body.light .log-head span{color:#1e293b}
body.light .log-day-btn{color:#0369a1}
body.light .log-prog-btn{color:#64748b}
body.light .log-entry{border-color:#cbd5e1}
body.light .log-zone{color:#1e293b}
body.light .alloff{color:#dc2626;border-color:#fca5a5;background:transparent}
body.light .alloff:hover{background:#fff0f0;border-color:#f87171;color:#b91c1c}
.rnbtn{background:#0f172a;color:#7dd3fc;border:1px solid #3b82f6;border-radius:.3rem;padding:.25rem .55rem;font-size:.85rem;font-weight:600;cursor:pointer;white-space:nowrap;transition:background .15s;flex-shrink:0}
.rnbtn:hover:not(:disabled){background:#1e3a5f}
.rnbtn:disabled{color:#334155;border-color:#1e293b;cursor:default}
.xbtn{background:none;border:none;color:#64748b;cursor:pointer;font-size:2.25rem;padding:.05rem .25rem;line-height:1;flex-shrink:0;transition:color .15s}
.xbtn:hover{color:#94a3b8}
.zexpand{display:none;flex-direction:column;gap:.55rem;border-top:1px solid #475569;padding-top:.65rem;margin-top:.5rem}
.zexpand.open{display:flex}
.ztog-row{display:flex;align-items:center;gap:.6rem}
.ztog-label{font-size:.79rem;font-weight:600;color:#e2e8f0;flex:1}
.modal-ov{position:fixed;inset:0;background:rgba(0,0,0,.65);display:flex;align-items:center;justify-content:center;z-index:100}
.modal{background:#1e293b;border-radius:.875rem;padding:1.25rem;border:1px solid #475569;width:260px;max-width:90vw}
.modal-title{font-size:.88rem;font-weight:600;color:#7dd3fc;margin-bottom:.9rem}
.modal label{font-size:.62rem;color:#94a3b8;letter-spacing:.08em;text-transform:uppercase;display:block;margin-bottom:.35rem}
.modal input{width:100%;background:#0f172a;border:1px solid #475569;border-radius:.3rem;color:#e2e8f0;padding:.4rem .5rem;font-size:1.1rem;text-align:center;margin-bottom:.9rem}
.modal input:focus{outline:none;border-color:#7dd3fc}
.modal-btns{display:flex;gap:.5rem}
.mbtn{flex:1;padding:.42rem;border-radius:.3rem;font-size:.78rem;font-weight:600;cursor:pointer;border:none}
.mbtn-cancel{background:#334155;color:#94a3b8}.mbtn-cancel:hover{background:#475569}
.dur-presets{display:flex;gap:.5rem;margin-bottom:.75rem}
.dur-preset{flex:1;padding:.55rem .25rem;background:#0f172a;border:1px solid #475569;border-radius:.4rem;color:#7dd3fc;font-size:.82rem;font-weight:600;cursor:pointer;transition:background .15s,border-color .15s}
.dur-preset:hover{background:#1e3a5f;border-color:#0284c7}
body.light .dur-preset{background:#f8fafc;border-color:#cbd5e1;color:#0369a1}
body.light .dur-preset:hover{background:#eff6ff;border-color:#0284c7}
@media(max-width:400px){.zdurs{grid-template-columns:1fr 1fr}}
.tg-ov{position:fixed;inset:0;background:rgba(0,0,0,.7);display:flex;align-items:flex-end;justify-content:center;z-index:200}
.tg-modal{background:#0f172a;border-radius:1rem 1rem 0 0;width:100%;max-width:500px;padding:1rem;box-sizing:border-box}
.tg-head{display:flex;align-items:center;justify-content:space-between;margin-bottom:.75rem}
.tg-title{font-size:.82rem;font-weight:700;color:#7dd3fc;letter-spacing:.06em;text-transform:uppercase}
.tg-close{background:none;border:none;color:#94a3b8;font-size:1.3rem;cursor:pointer;line-height:1}
.tg-note{font-size:.6rem;color:#94a3b8;text-align:center;margin-top:.4rem;letter-spacing:.03em}
canvas#tg-canvas{width:100%;border-radius:.5rem;display:block}
body.light .tg-modal{background:#f8fafc}
body.light .tg-title{color:#0369a1}
body.light .tg-note{color:#94a3b8}
.tg-toggle{display:flex;gap:.3rem;margin-bottom:.5rem}
.tg-btn{flex:1;padding:.25rem 0;font-size:.7rem;font-weight:600;border-radius:.375rem;border:1px solid #475569;background:none;color:#cbd5e1;cursor:pointer;letter-spacing:.04em}
.tg-btn.active{background:#0ea5e9;border-color:#0ea5e9;color:#fff}
body.light .tg-btn{border-color:#94a3b8;color:#475569}
body.light .tg-btn.active{background:#0284c7;border-color:#0284c7;color:#fff}
body.color{background:#171717;color:#e5e5e5}
body.color h1{color:#38bdf8}
body.color #clock{color:#fff}
body.color .sec{color:#34d399}
body.color .pcard,body.color .zcard,body.color .modal{background:#262626;border-color:#606060}
body.color .pcard.on{border-color:#0284c7}
body.color .zcard.active{border-color:#22c55e}
body.color .zcard.queued{border-color:#f59e0b}
body.color .pname{color:#fb923c}
body.color .zname{color:#f5f5f5}
body.color .ebtn{color:#6b6b6b}
body.color .dlabel,body.color .ep label,body.color .dur-row span,body.color .ztog-label{color:#f5f5f5;font-weight:600}
body.color .zdurs .dfield:first-child{border-color:#404040}
body.color .ebtn:hover{color:#a3a3a3}
body.color .ptime,body.color .dur-row input,body.color .ep input,body.color .modal input{background:#171717;border-color:#606060;color:#e5e5e5}
body.color .ep,body.color .zexpand{border-color:#606060}
body.color .rnbtn{background:#171717;color:#38bdf8;border-color:#3b82f6}
body.color .rnbtn:hover:not(:disabled){background:#1e3a5f}
body.color .rnbtn:disabled{color:#525252;border-color:#333}
body.color .info-side{color:#737373}
body.color .info-side:hover{color:#a3a3a3}
body.color .info-center{color:#737373}
body.color .log-trigger{border-color:#404040;color:#737373}
body.color .log-trigger:hover{background:#262626;color:#a3a3a3;border-color:#606060}
body.color .modal-title{color:#38bdf8}
body.color .mbtn-cancel{background:#484848;color:#d4d4d4}
body.color .mbtn-cancel:hover{background:#5a5a5a}
body.color .dur-preset{background:#262626;border-color:#606060;color:#38bdf8}
body.color .dur-preset:hover{background:#1e3a5f;border-color:#0284c7}
body.color .alloff{color:#f87171;border-color:#7f1d1d}
body.color .alloff:hover{background:#3f1010;border-color:#b91c1c;color:#fca5a5}
body.color #uptime,body.color #chip-temp{color:#737373}
body.color .log-modal{background:#171717}
body.color .log-head{border-color:#606060}
body.color .log-head span{color:#e5e5e5}
body.color .log-close{color:#737373}
body.color .log-close:hover{color:#a3a3a3}
body.color .log-day-btn{color:#38bdf8}
body.color .log-prog-btn{color:#a3a3a3}
body.color .log-entry{border-color:#333}
body.color .log-zone{color:#e5e5e5}
body.color .tg-modal{background:#171717}
body.color .tg-title{color:#38bdf8}
body.color .tg-note{color:#a3a3a3}
body.color .day{background:#1c1c1c;border-color:#606060;color:#737373}
body.color .day.on{background:#0369a1;color:#e0f2fe;border-color:#0369a1}
body.color .tg-btn{border-color:#606060;color:#a3a3a3}
body.color .tg-btn.active{background:#0ea5e9;border-color:#0ea5e9;color:#fff}
</style>
</head>
<body>
<div class="topbar">
  <h1>Irrigation Control</h1>
  <div class="theme-tgl">
    <button class="th-icon" onclick="cycleTheme()" title="Cycle theme">&#127912;</button>
  </div>
</div>
<div id="clock">--:--</div>
<div class="sec">Zones</div>
<div class="zone-grid" id="zone-grid"></div>
<div class="footer"><button class="alloff" onclick="allOff()">All Off</button></div>
<div class="sec">Programs</div>
<div class="prog-grid" id="prog-grid"></div>
<div class="log-row">
  <button class="log-trigger" onclick="openLog()">&#128203; Run Log</button>
</div>
<div class="info-row">
  <a class="info-side" href="https://github.com/jurassic73/irrigation_control" target="_blank"><svg height="14" viewBox="0 0 16 16" width="14" style="vertical-align:middle;margin-right:.3em" fill="currentColor" aria-hidden="true"><path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z"/></svg>GitHub</a>
  <span class="info-center" id="uptime">Uptime: --</span>
  <span class="info-side" id="chip-temp" onclick="openTempGraph()" style="cursor:pointer;justify-content:flex-end">&#127777; ESP32: --&#176;F</span>
</div>
<div class="log-ov" id="log-ov" style="display:none">
  <div class="log-modal">
    <div class="log-head"><span>&#128203; Run History</span><button class="log-close" onclick="closeLog()">&#10005;</button></div>
    <div class="log-body" id="log-body"><div class="log-empty">Loading…</div></div>
  </div>
</div>
<div class="tg-ov" id="tg-ov" style="display:none">
  <div class="tg-modal">
    <div class="tg-head">
      <span class="tg-title">&#127777; ESP32 Temp History</span>
      <button class="tg-close" onclick="closeTempGraph()">&#10005;</button>
    </div>
    <div class="tg-toggle">
      <button class="tg-btn active" id="tg-day" onclick="setTgView('day')">1 Day</button>
      <button class="tg-btn" id="tg-week" onclick="setTgView('week')">1 Week</button>
    </div>
    <canvas id="tg-canvas" height="200"></canvas>
    <div class="tg-note" id="tg-note">Chip die temp &bull; 10-min samples &bull; last 24h &bull; resets on reboot</div>
  </div>
</div>
<div class="modal-ov" id="rn-modal" style="display:none">
  <div class="modal">
    <div class="modal-title" id="rn-title">Run Zone</div>
    <div class="dur-presets">
      <button class="dur-preset" onclick="rnPreset(60)">1 min</button>
      <button class="dur-preset" onclick="rnPreset(300)">5 min</button>
      <button class="dur-preset" onclick="rnShowCustom()">Custom</button>
    </div>
    <div id="rn-custom" style="display:none">
      <label>Duration</label>
      <div class="dur-row" style="margin-bottom:.9rem;justify-content:center;gap:.45rem">
        <input type="number" id="rn-mins" min="0" max="480" value="5" style="max-width:3.5rem;font-size:1.1rem;padding:.4rem .5rem;text-align:center">
        <span style="font-size:.75rem;color:#64748b">min</span>
        <input type="number" id="rn-secs" min="0" max="59" value="0" style="max-width:3.5rem;font-size:1.1rem;padding:.4rem .5rem;text-align:center">
        <span style="font-size:.75rem;color:#64748b">sec</span>
      </div>
      <div class="modal-btns" style="margin-bottom:.5rem">
        <button class="mbtn sbtn" onclick="rnConfirm()">Run</button>
      </div>
    </div>
    <button class="mbtn mbtn-cancel" onclick="rnCancel()" style="width:100%">Cancel</button>
  </div>
</div>
<script>
let programs=[],zones=[],activeZone=-1,queued=[],tzSec=0,editing=new Set(),expanded=new Set(),daysSel={};
const DAYS=['S','M','T','W','T','F','S'];

function fmtUptime(s){
  const d=Math.floor(s/86400),h=Math.floor((s%86400)/3600),m=Math.floor((s%3600)/60);
  if(d>0) return d+'d '+h+'h';
  if(h>0) return h+'h '+m+'m';
  return m+'m';
}
async function fetchConfig(){
  const d=await(await fetch('/config')).json();
  tzSec=d.tzSec; programs=d.programs; zones=d.zones;
  activeZone=d.activeZone; queued=d.queued||[];
  initClock(d.epoch);
  if(d.uptime!=null) document.getElementById('uptime').textContent='Uptime: '+fmtUptime(d.uptime);
  render();
}

let clockBase=0,clockSync=0;
function initClock(e){clockBase=e;clockSync=Date.now();}
function tickClock(){
  if(!clockBase)return;
  const utc=clockBase+Math.floor((Date.now()-clockSync)/1000);
  document.getElementById('clock').textContent='Local: '+fmtTime(utc);
}
setInterval(tickClock,1000);

function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
function escA(s){return String(s).replace(/&/g,'&amp;').replace(/"/g,'&quot;');}
function pad(n){return String(n).padStart(2,'0');}
function zst(id){return id===activeZone?'active':queued.includes(id)?'queued':'off';}

function render(){renderPrograms();renderZones();}

function renderPrograms(){
  const el=document.getElementById('prog-grid');
  el.innerHTML='';
  programs.forEach((p,i)=>{
    if(daysSel['p'+i]===undefined)daysSel['p'+i]=p.days;
    const dh=DAYS.map((l,d)=>'<button class="day'+(((daysSel['p'+i]>>d)&1)?' on':'')+'" onclick="togglePDay('+i+','+d+')">'+l+'</button>').join('');
    const c=document.createElement('div');
    c.className='pcard'+(p.enabled?' on':'');
    c.innerHTML='<div class="phead"><span class="pname">'+esc(p.name)+'</span>'+
      '<button class="ptog'+(p.enabled?' on':'')+'" onclick="toggleProg('+i+')"></button></div>'+
      '<input class="ptime" type="time" id="pt'+i+'" value="'+pad(p.h)+':'+pad(p.m)+'" onchange="saveProg('+i+')">'+
      '<div class="days">'+dh+'</div>';
    el.appendChild(c);
  });
}

function renderZones(){
  const el=document.getElementById('zone-grid');
  el.innerHTML='';
  zones.forEach((z,i)=>{
    const st=zst(i),isOn=st!=='off';
    const bl=st==='active'?'Watering':'Queued';
    const busy=activeZone>=0||queued.length>0;
    const isExp=expanded.has(i),epOpen=editing.has(i);
    const durs=programs.map((p,pr)=>
      '<div class="dfield"><div class="dlabel">'+esc(p.name)+'</div>'+
      '<div class="dur-row">'+
      '<input type="number" id="durM'+i+'_'+pr+'" value="'+Math.floor(z.durations[pr]/60)+'" min="0" max="480" onchange="saveDur('+i+','+pr+')"><span>min</span>'+
      '<input type="number" id="durS'+i+'_'+pr+'" value="'+(z.durations[pr]%60)+'" min="0" max="59" onchange="saveDur('+i+','+pr+')"><span>sec</span>'+
      '</div></div>'
    ).join('');
    const c=document.createElement('div');
    c.className='zcard '+st;
    c.innerHTML=
      '<div class="ztop">'+
        '<span class="zname">'+esc(z.name)+'</span>'+
        (st!=='off'?'<span class="zbadge '+st+'">'+bl+'</span>':'')+
        '<button class="rnbtn" '+(busy?'disabled':'')+' onclick="runNow('+i+')">&#9654; Run Now</button>'+
        '<button class="xbtn" onclick="toggleExpand('+i+')">'+(isExp?'▴':'▾')+'</button>'+
      '</div>'+
      '<div class="zexpand'+(isExp?' open':'')+'" id="zx'+i+'">'+
        '<div class="ztog-row">'+
          '<button class="ztog'+(isOn?' on':'')+'" onclick="toggleZone('+i+')"></button>'+
          '<span class="ztog-label">'+(isOn?'Turn off':'Turn on')+'</span>'+
          '<button class="ebtn" onclick="toggleEdit('+i+')" title="Configure">&#9881;</button>'+
        '</div>'+
        '<div class="zdurs">'+durs+'</div>'+
        '<div class="ep'+(epOpen?' open':'')+'" id="ep'+i+'">'+
          '<div><label>Name</label><input type="text" id="zn'+i+'" value="'+escA(z.name)+'" maxlength="24"></div>'+
          '<div><label>GPIO Pin</label><input type="number" id="zp'+i+'" value="'+z.pin+'" min="0" max="48"></div>'+
          '<button class="sbtn" onclick="saveZone('+i+')">Save</button>'+
        '</div>'+
      '</div>';
    el.appendChild(c);
  });
}

async function togglePDay(pi,di){
  daysSel['p'+pi]^=(1<<di);
  renderPrograms();
  await fetch('/setprogram?id='+pi+'&en='+(programs[pi].enabled?1:0)+'&h='+programs[pi].h+'&m='+programs[pi].m+'&days='+daysSel['p'+pi]);
}

async function toggleProg(i){
  programs[i].enabled=!programs[i].enabled;
  await fetch('/setprogram?id='+i+'&en='+(programs[i].enabled?1:0)+'&h='+programs[i].h+'&m='+programs[i].m+'&days='+(daysSel['p'+i]??programs[i].days));
  render();
}

async function saveProg(i){
  const t=document.getElementById('pt'+i).value;
  const[h,m]=t?t.split(':').map(Number):[programs[i].h,programs[i].m];
  const days=daysSel['p'+i]??programs[i].days;
  await fetch('/setprogram?id='+i+'&en='+(programs[i].enabled?1:0)+'&h='+h+'&m='+m+'&days='+days);
  programs[i].h=h;programs[i].m=m;programs[i].days=days;
  render();
}

async function toggleZone(i){
  const st=zst(i);
  if(st==='off'){
    await fetch('/relay?id='+i+'&state=1');
    queued=[...queued,i];
  } else {
    await fetch('/relay?id='+i+'&state=0');
    queued=queued.filter(z=>z!==i);
    if(activeZone===i)activeZone=-1;
  }
  render();
  schedFetch();
}

async function saveDur(zi,pr){
  const m=parseInt(document.getElementById('durM'+zi+'_'+pr).value)||0;
  const s=parseInt(document.getElementById('durS'+zi+'_'+pr).value)||0;
  const val=m*60+s;
  zones[zi].durations[pr]=val;
  await fetch('/setzone?id='+zi+'&d'+pr+'='+val);
}

function toggleSet(s,i){s.has(i)?s.delete(i):s.add(i);renderZones();}
function toggleExpand(i){toggleSet(expanded,i);}
function toggleEdit(i){toggleSet(editing,i);}

async function saveZone(i){
  const name=(document.getElementById('zn'+i).value.trim())||zones[i].name;
  const pin=parseInt(document.getElementById('zp'+i).value);
  if(isNaN(pin)||pin<0||pin>48)return;
  await fetch('/setzone?id='+i+'&name='+encodeURIComponent(name)+'&pin='+pin);
  zones[i].name=name;zones[i].pin=pin;
  editing.delete(i);renderZones();
}

let rnZone=-1, fetchTimer=null;
function schedFetch(){clearTimeout(fetchTimer);fetchTimer=setTimeout(fetchConfig,600);}
function runNow(i){
  rnZone=i;
  document.getElementById('rn-title').textContent='Run '+esc(zones[i].name);
  document.getElementById('rn-custom').style.display='none';
  document.getElementById('rn-modal').style.display='flex';
}
function rnShowCustom(){
  document.getElementById('rn-custom').style.display='block';
  const secs=zones[rnZone]?.durations[0]||300;
  document.getElementById('rn-mins').value=Math.floor(secs/60);
  document.getElementById('rn-secs').value=secs%60;
  const el=document.getElementById('rn-mins');
  setTimeout(()=>{el.focus();el.select();},50);
}
async function rnRun(secs){
  document.getElementById('rn-modal').style.display='none';
  await fetch('/relay?id='+rnZone+'&state=1&secs='+secs);
  queued=[...queued,rnZone];
  rnZone=-1;
  render();
  schedFetch();
}
async function rnPreset(secs){rnRun(secs);}
function rnCancel(){document.getElementById('rn-modal').style.display='none';rnZone=-1;}
async function rnConfirm(){
  const m=parseInt(document.getElementById('rn-mins').value)||0;
  const s=parseInt(document.getElementById('rn-secs').value)||0;
  rnRun(Math.max(1,m*60+s));
}

async function allOff(){
  await fetch('/alloff');
  activeZone=-1;queued=[];render();
}

function fmtTime(epoch){
  const d=new Date((epoch+tzSec)*1000);
  let h=d.getUTCHours(),m=d.getUTCMinutes();
  const ap=h>=12?'PM':'AM';h=h%12||12;
  return h+':'+String(m).padStart(2,'0')+' '+ap;
}
function fmtDur(secs){
  const m=Math.floor(secs/60),s=secs%60;
  return m>0?m+'m'+(s?` ${s}s`:''):`${s}s`;
}
function fmtDayLabel(epoch){
  const d=new Date((epoch+tzSec)*1000);
  const DN=['Sunday','Monday','Tuesday','Wednesday','Thursday','Friday','Saturday'];
  const MN=['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
  return DN[d.getUTCDay()]+', '+MN[d.getUTCMonth()]+' '+d.getUTCDate();
}
function dayKey(epoch){
  const d=new Date((epoch+tzSec)*1000);
  return d.getUTCFullYear()*10000+(d.getUTCMonth()+1)*100+d.getUTCDate();
}
function toggleLogSection(btn){
  const body=btn.nextElementSibling;
  const open=body.style.display!=='none';
  body.style.display=open?'none':'block';
  btn.textContent=btn.textContent.replace(open?'▾':'▸',open?'▸':'▾');
}
async function openLog(){
  document.getElementById('log-ov').style.display='flex';
  const data=await(await fetch('/history')).json();
  const el=document.getElementById('log-body');
  if(!data.count){el.innerHTML='<div class="log-empty">No history yet.</div>';return;}
  const days={};
  data.history.forEach(e=>{
    const dk=dayKey(e.start);
    if(!days[dk])days[dk]={label:fmtDayLabel(e.start),progs:{}};
    const t=e.trigger;
    if(!days[dk].progs[t])days[dk].progs[t]=[];
    days[dk].progs[t].push(e);
  });
  const sortedDays=Object.keys(days).sort((a,b)=>b-a);
  let html='';
  sortedDays.forEach((dk,di)=>{
    const day=days[dk];
    const dayOpen=di===0;
    html+='<div class="log-day">'+
      '<button class="log-day-btn" onclick="toggleLogSection(this)">'+(dayOpen?'▾':'▸')+' '+day.label+'</button>'+
      '<div class="log-day-content" style="display:'+(dayOpen?'block':'none')+';">';
    Object.keys(day.progs).forEach(trig=>{
      html+='<div class="log-prog">'+
        '<button class="log-prog-btn" onclick="toggleLogSection(this)">▾ '+trig+'</button>'+
        '<div class="log-prog-content">';
      day.progs[trig].forEach(e=>{
        html+='<div class="log-entry">'+
          '<span class="log-zone">'+esc(e.name)+'</span>'+
          '<span class="log-dur">'+fmtDur(e.durationSecs)+'</span>'+
          '<span class="log-time">'+fmtTime(e.start)+'</span>'+
        '</div>';
      });
      html+='</div></div>';
    });
    html+='</div></div>';
  });
  el.innerHTML=html;
}
function closeLog(){document.getElementById('log-ov').style.display='none';}

function setTheme(t){
  document.body.classList.remove('light','color');
  if(t==='light')document.body.classList.add('light');
  else if(t==='color')document.body.classList.add('color');
  localStorage.setItem('theme',t||'dark');
}
function cycleTheme(){
  const cur=localStorage.getItem('theme')||'dark';
  setTheme({dark:'light',light:'color',color:'dark'}[cur]||'light');
}
(function(){const t=localStorage.getItem('theme')||'dark';setTheme(t);})();

async function fetchTemp(){
  try{
    const d=await(await fetch('/temp')).json();
    document.getElementById('chip-temp').textContent='🌡 ESP32: '+d.f.toFixed(1)+'°F';
  }catch(e){}
}

let tgTimer=null, tgView='day', tgData=[];
const tgDays=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
async function refreshTempGraph(){
  try{
    const secs=tgView==='day'?86400:604800;
    tgData=await(await fetch('/temphistory?secs='+secs)).json();
    drawTempGraph(tgData);
  }catch(e){}
}
async function setTgView(v){
  tgView=v;
  document.getElementById('tg-day').className='tg-btn'+(v==='day'?' active':'');
  document.getElementById('tg-week').className='tg-btn'+(v==='week'?' active':'');
  document.getElementById('tg-note').textContent='Chip die temp • 10-min samples • '+(v==='day'?'last 24h':'last 7 days')+' • resets on reboot';
  await refreshTempGraph();
}
async function openTempGraph(){
  document.getElementById('tg-ov').style.display='flex';
  await setTgView('day');
  tgTimer=setInterval(refreshTempGraph,60000);
}
function closeTempGraph(){
  document.getElementById('tg-ov').style.display='none';
  clearInterval(tgTimer);tgTimer=null;
}

function drawTempGraph(pts){
  const canvas=document.getElementById('tg-canvas');
  const light=document.body.classList.contains('light');
  const color=document.body.classList.contains('color');
  const W=canvas.offsetWidth||460; canvas.width=W; canvas.height=200;
  const ctx=canvas.getContext('2d');
  const bg=light?'#f1f5f9':color?'#262626':'#1e293b';
  const gridC=light?'#e2e8f0':color?'#404040':'#334155';
  const lineC='#0ea5e9';
  const textC=light?'#64748b':'#a3a3a3';
  ctx.fillStyle=bg; ctx.fillRect(0,0,W,200);
  if(!pts||!pts.length){
    ctx.fillStyle=textC; ctx.font='12px sans-serif'; ctx.textAlign='center';
    ctx.fillText('No data yet — check back in 10 min',W/2,100); return;
  }
  if(pts.length===1){
    const cy=100,cx=W/2;
    ctx.fillStyle=textC; ctx.font='12px sans-serif'; ctx.textAlign='center';
    ctx.fillText(pts[0].f.toFixed(1)+'°F',cx,cy-14);
    ctx.beginPath(); ctx.arc(cx,cy,5,0,2*Math.PI);
    ctx.fillStyle='#0ea5e9'; ctx.fill(); return;
  }
  const m={t:12,r:12,b:32,l:44};
  const gW=W-m.l-m.r, gH=200-m.t-m.b;
  const fs=pts.map(p=>p.f);
  let minF=Infinity,maxF=-Infinity; fs.forEach(v=>{if(v<minF)minF=v;if(v>maxF)maxF=v;});
  const mn=Math.floor(minF-2), mx=Math.ceil(maxF+2);
  const ts=pts.map(p=>p.t);
  const t0=ts[0], t1=ts[ts.length-1], tSpan=t1-t0||1;
  const xp=t=>(m.l+(t-t0)/tSpan*gW);
  const yp=f=>(m.t+gH-(f-mn)/(mx-mn)*gH);
  const steps=4;
  ctx.strokeStyle=gridC; ctx.lineWidth=1;
  for(let i=0;i<=steps;i++){
    const f=mn+i*(mx-mn)/steps;
    const y=yp(f);
    ctx.beginPath(); ctx.moveTo(m.l,y); ctx.lineTo(m.l+gW,y); ctx.stroke();
    ctx.fillStyle=textC; ctx.font='10px sans-serif'; ctx.textAlign='right';
    ctx.fillText(f.toFixed(0)+'°',m.l-4,y+3);
  }
  ctx.fillStyle=textC; ctx.textAlign='center'; ctx.font='10px sans-serif';
  [0,0.25,0.5,0.75,1].forEach(r=>{
    const t=t0+r*tSpan;
    const d=new Date(t*1000);
    const lbl=tgView==='week'
      ?tgDays[d.getDay()]+' '+pad(d.getHours())+'h'
      :pad(d.getHours())+':'+pad(d.getMinutes());
    ctx.fillText(lbl,m.l+r*gW,200-m.b+12);
  });
  ctx.strokeStyle=lineC; ctx.lineWidth=2; ctx.lineJoin='round'; ctx.beginPath();
  pts.forEach((p,i)=>{
    i===0?ctx.moveTo(xp(p.t),yp(p.f)):ctx.lineTo(xp(p.t),yp(p.f));
  });
  ctx.stroke();
  ctx.lineTo(xp(ts[ts.length-1]),m.t+gH); ctx.lineTo(xp(ts[0]),m.t+gH); ctx.closePath();
  const grad=ctx.createLinearGradient(0,m.t,0,m.t+gH);
  grad.addColorStop(0,'rgba(14,165,233,0.25)'); grad.addColorStop(1,'rgba(14,165,233,0)');
  ctx.fillStyle=grad; ctx.fill();
}

fetchConfig();
setInterval(fetchConfig,15000);
fetchTemp();
setInterval(fetchTemp,30000);
</script>
</body>
</html>
)rawliteral";

// ── HTTP handlers + setup ─────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);

  led.begin();
  led.setBrightness(128);
  led.setPixelColor(0, led.Color(0, 255, 0));
  led.show();


  loadConfig();

  for (int i = 0; i < NUM_ZONES; i++) {
    pinMode(relayPins[i], OUTPUT);
    setRelay(i, false);
  }

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nConnected! IP: " + WiFi.localIP().toString());

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", TZ_PACIFIC, 1); tzset();
  Serial.print("Syncing NTP");
  { struct tm ti{}; for (int t=0;t<20&&!getLocalTime(&ti);t++){delay(500);Serial.print("."); } Serial.println(); }
  { time_t t; time(&t); addTempSample(t, temperatureRead()); lastTempSample = millis(); }

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req){
    req->send(200, "text/html", INDEX_HTML);
  });

  server.on("/config", HTTP_GET, [](AsyncWebServerRequest* req){
    time_t now; struct tm ti{};
    time(&now); localtime_r(&now, &ti);
    long tzs = ti.tm_isdst > 0 ? (-7L*3600L) : (-8L*3600L);

    String j; j.reserve(600);
    j = "{\"epoch\":" + String((long)now) + ",\"tzSec\":" + String(tzs) + ",\"uptime\":" + String((unsigned long)(millis()/1000));
    j += ",\"activeZone\":" + String(activeZone) + ",\"queued\":[";
    for (int i = 0; i < qSize; i++) {
      if (i) j += ",";
      j += String(qBuf[(qHead + i) % QUEUE_MAX].zone);
    }
    j += "],\"programs\":[";
    for (int pr = 0; pr < NUM_PROGRAMS; pr++) {
      if (pr) j += ",";
      WateringProgram& p = programs[pr];
      j += "{\"name\":\"" + String(PROG_NAMES[pr]) + "\",\"enabled\":" + (p.enabled?"true":"false") +
           ",\"h\":" + String(p.hour) + ",\"m\":" + String(p.minute) + ",\"days\":" + String(p.days) + "}";
    }
    j += "],\"zones\":[";
    for (int i = 0; i < NUM_ZONES; i++) {
      if (i) j += ",";
      j += "{\"id\":" + String(i) + ",\"name\":\"" + jsonEsc(relayNames[i]) + "\",\"pin\":" + String(relayPins[i]) + ",\"durations\":[";
      for (int pr = 0; pr < NUM_PROGRAMS; pr++) {
        if (pr) j += ",";
        j += String(zoneDuration[i][pr]);
      }
      j += "]}";
    }
    j += "]}";
    req->send(200, "application/json", j);
  });

  server.on("/setprogram", HTTP_GET, [](AsyncWebServerRequest* req){
    if (!req->hasParam("id")) { req->send(400,"text/plain","missing id"); return; }
    int id = req->getParam("id")->value().toInt();
    if (id < 0 || id >= NUM_PROGRAMS) { req->send(400,"text/plain","bad id"); return; }
    if (req->hasParam("en"))   programs[id].enabled = req->getParam("en")->value()   == "1";
    if (req->hasParam("h"))    programs[id].hour    = req->getParam("h")->value().toInt();
    if (req->hasParam("m"))    programs[id].minute  = req->getParam("m")->value().toInt();
    if (req->hasParam("days")) programs[id].days    = req->getParam("days")->value().toInt();
    saveConfig();
    Serial.printf("Program %d saved\n", id);
    req->send(200,"text/plain","ok");
  });

  server.on("/setzone", HTTP_GET, [](AsyncWebServerRequest* req){
    if (!req->hasParam("id")) { req->send(400,"text/plain","missing id"); return; }
    int idx = req->getParam("id")->value().toInt();
    if (idx < 0 || idx >= NUM_ZONES) { req->send(400,"text/plain","bad id"); return; }
    if (req->hasParam("name")) req->getParam("name")->value().toCharArray(relayNames[idx], 32);
    if (req->hasParam("pin")) {
      int pin = req->getParam("pin")->value().toInt();
      if (pin >= 0 && pin <= 48 && pin != relayPins[idx]) {
        setRelay(idx, false);
        pinMode(relayPins[idx], INPUT);
        relayPins[idx] = (uint8_t)pin;
        pinMode(relayPins[idx], OUTPUT);
        setRelay(idx, false);
      }
    }
    for (int pr = 0; pr < NUM_PROGRAMS; pr++) {
      char pname[4]; snprintf(pname, sizeof(pname), "d%d", pr);
      if (req->hasParam(pname)) zoneDuration[idx][pr] = req->getParam(pname)->value().toInt();
    }
    saveConfig();
    req->send(200,"text/plain","ok");
  });

  server.on("/relay", HTTP_GET, [](AsyncWebServerRequest* req){
    if (req->hasParam("id") && req->hasParam("state")) {
      int  idx = req->getParam("id")->value().toInt();
      bool on  = req->getParam("state")->value() == "1";
      if (idx >= 0 && idx < NUM_ZONES) {
        if (on) {
          uint32_t secs;
          if (req->hasParam("secs"))
            secs = (uint32_t)constrain(req->getParam("secs")->value().toInt(), 1, 28800);
          else
            secs = zoneDuration[idx][0] > 0 ? (uint32_t)zoneDuration[idx][0] : 300;
          enqueue(idx, secs);
          Serial.printf("Manual: Zone %d queued for %lus\n", idx, (unsigned long)secs);
        } else {
          stopActive(idx);
          removeFromQueue(idx);
          Serial.printf("Manual: Zone %d stopped/removed\n", idx);
        }
      }
    }
    req->send(200,"text/plain","ok");
  });

  server.on("/alloff", HTTP_GET, [](AsyncWebServerRequest* req){
    allOffFn();
    req->send(200,"text/plain","ok");
  });

  server.on("/history", HTTP_GET, [](AsyncWebServerRequest* req){
    static const char* trigNames[] = {"Manual", "Morning", "Afternoon"};
    String j; j.reserve(histCount * 100 + 64);
    j = "{\"retainDays\":" + String(historyDays) +
               ",\"count\":" + String(histCount) + ",\"history\":[";
    for (int i = histCount - 1; i >= 0; i--) {
      if (i < histCount - 1) j += ",";
      HistoryEntry& e = history[(histHead + i) % HISTORY_MAX];
      const char* trig = e.trigger <= NUM_PROGRAMS ? trigNames[e.trigger] : "Unknown";
      j += "{\"zone\":" + String(e.zone) +
           ",\"name\":\"" + jsonEsc(relayNames[e.zone]) + "\"" +
           ",\"trigger\":\"" + String(trig) + "\"" +
           ",\"start\":" + String((long)e.start) +
           ",\"durationSecs\":" + String(e.duration) + "}";
    }
    j += "]}";
    req->send(200, "application/json", j);
  });

  server.on("/sethistory", HTTP_GET, [](AsyncWebServerRequest* req){
    if (req->hasParam("days")) {
      historyDays = (uint8_t)constrain(req->getParam("days")->value().toInt(), 1, 90);
      saveConfig();
      purgeHistory();
      Serial.printf("History retain set to %d days\n", historyDays);
    }
    req->send(200, "text/plain", "ok");
  });

  server.on("/temp", HTTP_GET, [](AsyncWebServerRequest* req){
    float c = temperatureRead();
    char buf[32]; snprintf(buf, sizeof(buf), "{\"c\":%.1f,\"f\":%.1f}", c, toF(c));
    req->send(200, "application/json", buf);
  });

  server.on("/temphistory", HTTP_GET, [](AsyncWebServerRequest* req){
    uint32_t secs = req->hasParam("secs") ? (uint32_t)req->getParam("secs")->value().toInt() : 604800UL;
    time_t cutoff = 0;
    if (tempHistCount > 0 && secs < 604800UL) {
      cutoff = tempHist[(tempHistHead + tempHistCount - 1) % TEMP_HIST_MAX].t - (time_t)secs;
    }
    String j; j.reserve(tempHistCount * 28 + 4);
    j = "[";
    char buf[32];
    bool first = true;
    for (int i = 0; i < tempHistCount; i++) {
      TempSample& s = tempHist[(tempHistHead + i) % TEMP_HIST_MAX];
      if (s.t < cutoff) continue;
      if (!first) j += ",";
      snprintf(buf, sizeof(buf), "{\"t\":%ld,\"f\":%.1f}", (long)s.t, toF(s.c));
      j += buf;
      first = false;
    }
    j += "]";
    req->send(200, "application/json", j);
  });

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  server.begin();
}

void loop() {
  static unsigned long lastSched = 0;
  unsigned long now_ms = millis();
  runQueue();
  if (now_ms - lastSched >= 15000) { lastSched = now_ms; checkSchedules(); }
  if (now_ms - lastTempSample >= 600000UL) {
    lastTempSample = now_ms;
    time_t now; time(&now);
    addTempSample(now, temperatureRead());
  }
}
