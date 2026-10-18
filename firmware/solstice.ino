#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <esp_arduino_version.h>
#include <PubSubClient.h>
#include <esp_timer.h>

// SOLSTICE V4 - Stage A (ESP32-WROOM)
const char* WIFI_SSID = "<WifiNetworkName>";
const char* WIFI_PASSWORD = "<wifi-password>";
const char* AP_SSID = "Solstice-Setup";
const char* AP_PASSWORD = "<accesspoint-password>"; // >= 8 chars; choose a unique strong password
const char* TZ_PACIFIC = "PST8PDT,M3.2.0/2,M11.1.0/2";
const double LAT = 37.7397, LON = -121.4252; // Approximate Tracy, CA coordinates
const int COOL_PIN = 21, WARM_PIN = 19;
const int PWM_HZ = 5000, PWM_BITS = 8;
const int MIN_K = 2700, MAX_K = 6500;
const uint32_t SAVE_AFTER_MS = 2500;
WebServer server(80);
Preferences prefs;

// MQTT telemetry. Set the Raspberry Pi Zero 2 W LAN address and credentials.
const char* MQTT_HOST = "raspberrypi.local";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER = "<mqtt_user>";
const char* MQTT_PASSWORD = "<mqtt_password>";
const char* MQTT_STATE = "home/solstice/state";
const char* MQTT_EVENTS = "home/solstice/events";
const char* MQTT_AVAIL = "home/solstice/availability";
const char* MQTT_SETTINGS = "home/solstice/settings";
const char* MQTT_ADJUSTMENTS = "home/solstice/adjustments";
const char* MQTT_HEALTH = "home/solstice/telemetry";
const char* FIRMWARE_VERSION = "4.2-mqtt-resilience";
const uint32_t HEALTH_INTERVAL_MS = 60000;
uint32_t lastHealthPublishedAt = 0;
bool healthPublishPending = true;
WiFiClient mqttTransport;
PubSubClient mqtt(mqttTransport);
uint32_t lastMqttAttempt = 0;
const uint32_t MQTT_RETRY_MS = 15000;
uint32_t mqttFailureCount = 0;
uint32_t mqttReconnectCount = 0;
bool mqttEverConnected = false;
// NetworkClient connection timeout bounds TCP connect attempts. PubSubClient
// socket timeout bounds MQTT CONNACK/read waits (seconds, minimum 1).
const int MQTT_TCP_CONNECT_TIMEOUT_MS = 250;
const uint16_t MQTT_SOCKET_TIMEOUT_SEC = 1;
// Small RAM queue buffers events during temporary broker outages.
struct LightEvent { bool on; char source[12]; time_t timestamp; uint32_t sequence; };
LightEvent eventQueue[16];
uint8_t queueHead = 0, queueCount = 0;
uint32_t eventSequence = 0;
bool lastObservedPower = false;

void queueLightEvent(bool on, const char* source) {
  if (queueCount == 16) {
    // Queue full: drop oldest; this is not durable storage.
    queueHead = (queueHead + 1) % 16;
    queueCount--;
    Serial.println("MQTT queue full: oldest event dropped");
  }
  uint8_t index = (queueHead + queueCount) % 16;
  eventQueue[index].on = on;
  snprintf(eventQueue[index].source, sizeof(eventQueue[index].source), "%s", source);
  eventQueue[index].timestamp = time(nullptr);
  eventQueue[index].sequence = ++eventSequence;
  queueCount++;
}

extern bool powerOn;
// Debounced settings history. Track the previous committed values rather than
// each intermediate slider request. This is RAM-only, like the power event queue.
extern int brightness, kelvin;
int committedBrightness = 70, committedKelvin = 3000;
int pendingBrightness = 70, pendingKelvin = 3000;
uint32_t settingsChangedAt = 0;
bool settingsPending = false;
char pendingSource[12] = "manual";
struct AdjustmentEvent {
  char type[20]; int previous; int value; char source[12];
  time_t timestamp; uint32_t sequence;
};
AdjustmentEvent adjustmentQueue[16];
uint8_t adjHead = 0, adjCount = 0;
uint32_t adjustmentSequence = 0;
bool settingsSnapshotPending = true;

void queueAdjustment(const char* type, int previous, int value, const char* source) {
  if (previous == value) return;
  if (adjCount == 16) {
    adjHead = (adjHead + 1) % 16;
    adjCount--;
    Serial.println("MQTT adjustment queue full: oldest event dropped");
  }
  uint8_t index = (adjHead + adjCount) % 16;
  AdjustmentEvent &e = adjustmentQueue[index];
  snprintf(e.type, sizeof(e.type), "%s", type);
  e.previous = previous; e.value = value;
  snprintf(e.source, sizeof(e.source), "%s", source);
  e.timestamp = time(nullptr);
  e.sequence = ++adjustmentSequence;
  adjCount++;
}

void observeSettings(const char* source) {
  if (brightness == pendingBrightness && kelvin == pendingKelvin) return;
  pendingBrightness = brightness;
  pendingKelvin = kelvin;
  snprintf(pendingSource, sizeof(pendingSource), "%s", source);
  settingsChangedAt = millis();
  settingsPending = true;
}

void settingsTick() {
  if (!settingsPending || millis() - settingsChangedAt < 700) return;
  settingsPending = false;
  queueAdjustment("brightness", committedBrightness, pendingBrightness, pendingSource);
  queueAdjustment("color_temperature", committedKelvin, pendingKelvin, pendingSource);
  committedBrightness = pendingBrightness;
  committedKelvin = pendingKelvin;
  settingsSnapshotPending = true;
}

bool publishSettingsSnapshot() {
  char payload[128];
  snprintf(payload, sizeof(payload),
    "{\"power\":\"%s\",\"brightness\":%d,\"kelvin\":%d}",
    powerOn ? "ON" : "OFF", brightness, kelvin);
  return mqtt.publish(MQTT_SETTINGS, payload, true);
}


void observeLightState(const char* source) {
  if (powerOn == lastObservedPower) return;
  lastObservedPower = powerOn;
  queueLightEvent(powerOn, source);
}

// Periodic health metrics are snapshots, not historical events.
// PubSubClient's default publish QoS is 0; a successful return is not
// confirmation that the broker or a database persisted the message.
void publishHealthTick() {
  if (!mqtt.connected()) return;
  const uint32_t now = millis();
  if (!healthPublishPending && now - lastHealthPublishedAt < HEALTH_INTERVAL_MS) return;
  char payload[256];
  const int rssi = WiFi.RSSI();
  const unsigned long uptime = (unsigned long)(esp_timer_get_time() / 1000000ULL);
  snprintf(payload, sizeof(payload),
    "{\"firmware\":\"%s\",\"uptime_seconds\":%lu,\"wifi_rssi_dbm\":%d,"
    "\"free_heap_bytes\":%lu,\"mqtt_connected\":true,\"power\":\"%s\","
    "\"brightness\":%d,\"kelvin\":%d}",
    FIRMWARE_VERSION, uptime, rssi, (unsigned long)ESP.getFreeHeap(),
    powerOn ? "ON" : "OFF", brightness, kelvin);
  if (mqtt.publish(MQTT_HEALTH, payload, false)) {
    lastHealthPublishedAt = now;
    healthPublishPending = false;
  }
}

void mqttTick() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!mqtt.connected()) {
    if (lastMqttAttempt != 0 && millis() - lastMqttAttempt < MQTT_RETRY_MS) return;
    lastMqttAttempt = millis();
    if (!mqtt.connect("solstice-led-controller", MQTT_USER, MQTT_PASSWORD,
                      MQTT_AVAIL, 1, true, "OFFLINE")) {
      mqttFailureCount++;
      mqttTransport.stop();
      Serial.printf("MQTT unavailable; state=%d failures=%lu; retry in %lu ms\n",
                    mqtt.state(), (unsigned long)mqttFailureCount,
                    (unsigned long)MQTT_RETRY_MS);
      return;
    }
    if (mqttEverConnected) mqttReconnectCount++;
    mqttEverConnected = true;
    mqtt.publish(MQTT_AVAIL, "ONLINE", true);
    mqtt.publish(MQTT_STATE, powerOn ? "ON" : "OFF", true);
    settingsSnapshotPending = true;
    healthPublishPending = true;
    Serial.println("MQTT connected");
  }
  mqtt.loop();
  // State is retained; events are not retained and are sent in FIFO order.
  if (queueCount) {
    LightEvent &e = eventQueue[queueHead];
    char payload[192];
    snprintf(payload, sizeof(payload),
      "{\"state\":\"%s\",\"source\":\"%s\",\"timestamp\":%lld,\"sequence\":%lu}",
      e.on ? "ON" : "OFF", e.source, (long long)e.timestamp,
      (unsigned long)e.sequence);
    if (mqtt.publish(MQTT_EVENTS, payload, false)) {
      mqtt.publish(MQTT_STATE, powerOn ? "ON" : "OFF", true);
      settingsSnapshotPending = true;
      queueHead = (queueHead + 1) % 16;
      queueCount--;
    }
  }
  if (adjCount) {
    AdjustmentEvent &e = adjustmentQueue[adjHead];
    char payload[220];
    snprintf(payload, sizeof(payload),
      "{\"type\":\"%s\",\"previous\":%d,\"value\":%d,\"source\":\"%s\",\"timestamp\":%lld,\"sequence\":%lu}",
      e.type, e.previous, e.value, e.source,
      (long long)e.timestamp, (unsigned long)e.sequence);
    if (mqtt.publish(MQTT_ADJUSTMENTS, payload, false)) {
      adjHead = (adjHead + 1) % 16;
      adjCount--;
    }
  }
  if (settingsSnapshotPending && publishSettingsSnapshot())
    settingsSnapshotPending = false;
  publishHealthTick();
}


int brightness = 70, kelvin = 3000;
bool powerOn = false, scheduleEnabled = false;
int sunsetBrightness = 70, sunsetKelvin = 3000;
float currentCool = 0, currentWarm = 0;
int targetCool = 0, targetWarm = 0;
uint32_t lastFade = 0, lastChange = 0, lastWifiRetry = 0;
bool dirty = false, apMode = false, timeReady = false;
int lastStartDate = 0, lastEndDate = 0, manualOverrideDate = 0;
int cachedDate = 0, sunsetMinutes = -1;

int dateKey(const tm& t) { return (t.tm_year + 1900)*10000 + (t.tm_mon + 1)*100 + t.tm_mday; }
void pwmWrite(int c, int w) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(COOL_PIN, c); ledcWrite(WARM_PIN, w);
#else
  ledcWrite(0, c); ledcWrite(1, w);
#endif
}
void pwmInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(COOL_PIN, PWM_HZ, PWM_BITS); ledcAttach(WARM_PIN, PWM_HZ, PWM_BITS);
#else
  ledcSetup(0, PWM_HZ, PWM_BITS); ledcSetup(1, PWM_HZ, PWM_BITS);
  ledcAttachPin(COOL_PIN, 0); ledcAttachPin(WARM_PIN, 1);
#endif
  pwmWrite(0, 0);
}
void recalc() {
  float level = powerOn ? brightness / 100.0f : 0.0f;
  float cool = float(kelvin - MIN_K) / (MAX_K - MIN_K);
  targetCool = roundf(255.0f * level * cool);
  targetWarm = roundf(255.0f * level * (1.0f - cool));
}
void fadeTick() {
  if (millis() - lastFade < 10) return;
  lastFade = millis();
  currentCool += (targetCool - currentCool)*0.08f;
  currentWarm += (targetWarm - currentWarm)*0.08f;
  if (fabsf(currentCool-targetCool)<0.5f) currentCool=targetCool;
  if (fabsf(currentWarm-targetWarm)<0.5f) currentWarm=targetWarm;
  pwmWrite(roundf(currentCool), roundf(currentWarm));
}
void changed() { recalc(); dirty=true; lastChange=millis(); }
void saveTick() {
  if (!dirty || millis()-lastChange<SAVE_AFTER_MS) return;
  prefs.putInt("bright",brightness); prefs.putInt("kelvin",kelvin);
  prefs.putBool("power",powerOn); prefs.putBool("schedule",scheduleEnabled);
  prefs.putInt("sbright",sunsetBrightness); prefs.putInt("skelvin",sunsetKelvin);
  dirty=false;
}

// NOAA-style sunrise/sunset approximation: returns minutes after local midnight.
// UTC calculation followed by localtime_r handles DST and date rollover.
int calculateSunset(const tm& localDay) {
  int N = localDay.tm_yday + 1;
  double lngHour = LON / 15.0;
  double t = N + ((18.0 - lngHour) / 24.0);
  double M = 0.9856*t - 3.289;
  auto rad=[](double d){return d*M_PI/180.0;};
  auto deg=[](double r){return r*180.0/M_PI;};
  auto norm=[](double x){ x=fmod(x,360.0); return x<0?x+360.0:x; };
  double L=norm(M+1.916*sin(rad(M))+0.020*sin(2*rad(M))+282.634);
  double RA=norm(deg(atan(0.91764*tan(rad(L)))));
  RA += floor(L/90.0)*90.0-floor(RA/90.0)*90.0;
  RA /= 15.0;
  double sinDec=0.39782*sin(rad(L));
  double cosDec=cos(asin(sinDec));
  double cosH=(cos(rad(90.833))-sinDec*sin(rad(LAT)))/(cosDec*cos(rad(LAT)));
  if (cosH < -1 || cosH > 1) return -1;
  double H=deg(acos(cosH))/15.0;
  double T=H+RA-0.06571*t-6.622;
  double UT=fmod(T-lngHour+24.0,24.0);
  // Construct a UTC timestamp for the selected local calendar day.
  tm utc = localDay;
  utc.tm_hour=0; utc.tm_min=0; utc.tm_sec=0;
  // timegm is supported by ESP32/newlib.
  time_t base = makeUtcTime(utc);
  time_t event=base+(time_t)round(UT*3600);
  tm result;
  localtime_r(&event,&result);
  // If local calendar date differs, adjust by one day (rare near date boundaries).
  return result.tm_hour*60+result.tm_min;
}

time_t makeUtcTime(struct tm utc) {
  // Convert UTC calendar date to Unix time
  // independently of the configured local timezone.
  const char* oldTZ = getenv("TZ");
  String savedTZ = oldTZ ? String(oldTZ) : "";

  setenv("TZ", "UTC0", 1);
  tzset();

  utc.tm_isdst = 0;
  time_t result = mktime(&utc);

  if (oldTZ) {
    setenv("TZ", savedTZ.c_str(), 1);
  } else {
    unsetenv("TZ");
  }
  tzset();

  return result;
}

void scheduleTick() {
  time_t now=time(nullptr);
  if (now < 1700000000) {timeReady=false; return;}
  timeReady=true;
  tm local; localtime_r(&now,&local);
  int today=dateKey(local);
  if (cachedDate != today) {
    cachedDate=today;
    sunsetMinutes=calculateSunset(local);
  }
  if (!scheduleEnabled || sunsetMinutes<0) return;
  int minute=local.tm_hour*60+local.tm_min;
  // Event-based automation: act once after sunset and once after 23:00.
  // At boot during the window, start the light unless this day was manually overridden.
  if (minute >= sunsetMinutes && minute < 23*60) {
    if (lastStartDate != today) {
      lastStartDate=today;
      if (manualOverrideDate != today) {
        powerOn=true; brightness=sunsetBrightness; kelvin=sunsetKelvin;
        changed();observeLightState("sunset");observeSettings("sunset");
      }
    }
  } else if (minute >= 23*60 && lastEndDate != today) {
    lastEndDate=today;
    powerOn=false; changed();observeLightState("schedule");
  }
}

const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Solstice</title>
<style>
:root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;background:#101217;color:#f6f3ed;font-family:system-ui,-apple-system,sans-serif}main{max-width:560px;margin:auto;padding:24px 18px 50px}header{text-align:center;margin:16px 0 30px}h1{font-size:37px;letter-spacing:5px;margin:0;color:#ffd39b}h2{font-size:18px;margin:0 0 18px}p{color:#b8b9c0}.sub{margin:6px}.card{background:#1e222a;border:1px solid #30343d;border-radius:18px;padding:22px;margin:16px 0}.line{display:flex;justify-content:space-between;align-items:center;gap:12px}.value{font-size:25px;font-weight:650;color:#ffd39b}.field{margin:22px 0}input[type=range]{width:100%;accent-color:#ffc176;margin-top:12px}button{background:#363b46;color:#fff;border:0;border-radius:11px;padding:12px 15px;margin:4px;cursor:pointer}button.primary{background:#8a5a29}button.active{background:#257f5a}.buttons{display:flex;flex-wrap:wrap;gap:3px}.switch{width:56px;height:31px;border-radius:20px;background:#525760;position:relative;padding:0;margin:0}.switch:before{content:'';position:absolute;left:4px;top:4px;background:#fff;width:23px;height:23px;border-radius:50%;transition:.2s}.switch.on{background:#288c61}.switch.on:before{left:29px}.status{display:grid;grid-template-columns:1fr 1fr;gap:14px}.status>div{background:#282d36;border-radius:10px;padding:13px}.status small{display:block;color:#aab0ba;margin-bottom:7px}.muted{color:#b7bac2;font-size:13px}footer{text-align:center;margin-top:35px;color:#b6b7bc;font-size:13px}
</style></head><body><main><header><h1>☀ SOLSTICE</h1><p class="sub">Smart Ambient Lighting</p></header>
<section class="card"><div class="line"><h2>Lighting</h2><button id="power" onclick="togglePower()">Loading</button></div>
<div class="field"><div class="line"><span>Brightness</span><span class="value"><span id="bv">70</span>%</span></div><input id="brightness" type="range" min="0" max="100" oninput="setBrightness(this.value)"></div>
<div class="field"><div class="line"><span>Color temperature</span><span class="value"><span id="kv">3000</span> K</span></div><input id="kelvin" type="range" min="2700" max="6500" step="50" oninput="setKelvin(this.value)"></div>
<h2>Scenes</h2><div class="buttons"><button onclick="scene(4400,45)">Relax</button><button onclick="scene(3000,70)">Evening</button><button onclick="scene(2700,15)">Night</button><button onclick="scene(6000,100)">Daylight</button></div></section>
<section class="card"><div class="line"><h2>Sunset automation</h2><button id="schedule" class="switch" aria-label="Toggle schedule" onclick="toggleSchedule()"></button></div><p class="muted">On at local sunset · Off at 11:00 PM</p>
<div class="field"><div class="line"><span>Sunset brightness</span><span id="sbv">70%</span></div><input id="sb" type="range" min="0" max="100" oninput="setSunBright(this.value)"></div>
<div class="field"><div class="line"><span>Sunset color</span><span id="skv">3000 K</span></div><input id="sk" type="range" min="2700" max="6500" step="50" oninput="setSunKelvin(this.value)"></div>
<div class="line"><span>Today's sunset</span><strong id="sunset">—</strong></div><p id="schedstatus" class="muted">Waiting for clock</p></section>
<section class="card"><h2>Controller</h2><div class="status"><div><small>Local time</small><strong id="clock">—</strong></div><div><small>Wi-Fi signal</small><strong id="rssi">—</strong></div><div><small>Uptime</small><strong id="uptime">—</strong></div><div><small>Temperature</small><strong>Sensor not installed</strong></div><div><small>IP address</small><strong id="ip">—</strong></div><div><small>Firmware</small><strong>V4.0</strong></div></div></section>
<footer>Solstice V4.0<br>Designed &amp; Developed by Ginoy</footer>
</main><script>
let state={},timers={};const el=id=>document.getElementById(id);
async function api(url){let r=await fetch(url,{cache:'no-store'});if(!r.ok)throw Error('Network request failed');return r.json()}
function show(s,full=false){state=s;el('power').textContent=s.power?'ON':'OFF';el('power').className=s.power?'active':'';el('schedule').className='switch'+(s.schedule?' on':'');el('schedule').setAttribute('aria-pressed',s.schedule);
el('sunset').textContent=s.sunset;el('schedstatus').textContent=s.scheduleStatus;el('clock').textContent=s.clock;el('rssi').textContent=s.rssi;el('uptime').textContent=s.uptime;el('ip').textContent=s.ip;
if(full){el('brightness').value=s.brightness;el('bv').textContent=s.brightness;el('kelvin').value=s.kelvin;el('kv').textContent=s.kelvin;el('sb').value=s.sunsetBrightness;el('sbv').textContent=s.sunsetBrightness+'%';el('sk').value=s.sunsetKelvin;el('skv').textContent=s.sunsetKelvin+' K';}}
async function action(q){try{show(await api('/api/set?'+q),true)}catch(e){el('schedstatus').textContent='Connection error'}}
function debounce(key,q){clearTimeout(timers[key]);timers[key]=setTimeout(()=>action(q),180)}
function setBrightness(v){el('bv').textContent=v;debounce('b','brightness='+v)}
function setKelvin(v){el('kv').textContent=v;debounce('k','kelvin='+v)}
function setSunBright(v){el('sbv').textContent=v+'%';debounce('sb','sunsetBrightness='+v)}
function setSunKelvin(v){el('skv').textContent=v+' K';debounce('sk','sunsetKelvin='+v)}
function togglePower(){action('power='+(state.power?0:1))}
function toggleSchedule(){action('schedule='+(state.schedule?0:1))}
function scene(k,b){action('scene='+k+','+b)}
async function refresh(){try{show(await api('/api/state'))}catch(e){el('schedstatus').textContent='Controller offline'}}
refresh().then(async()=>{try{show(await api('/api/state'),true)}catch(e){}});setInterval(refresh,5000);
</script></body></html>
)HTML";

String fmtTime(int minute) {
  if (minute<0) return "Unavailable";
  int h=minute/60, m=minute%60;
  char b[18]; snprintf(b,sizeof(b),"%d:%02d %s",(h%12)?h%12:12,m,h>=12?"PM":"AM");
  return String(b);
}
String jsonEscape(const String& s) {
  String out="\"";
  for(size_t i=0;i<s.length();i++) {char c=s[i];if(c=='"'||c=='\\')out+='\\';if(c=='\n'||c=='\r')out+=' ';else out+=c;}
  return out+'"';
}
void sendState() {
  time_t now=time(nullptr); tm local={};
  bool valid=now>=1700000000;
  if(valid)localtime_r(&now,&local);
  char clockText[32]="Clock syncing";
  if(valid)strftime(clockText,sizeof(clockText),"%I:%M:%S %p",&local);
  String status;
  if(!scheduleEnabled) status="Automation disabled";
  else if(!valid) status="Waiting for time sync";
  else if(sunsetMinutes<0) status="Sunset unavailable";
  else if(manualOverrideDate==dateKey(local)) status="Manual override until tomorrow";
  else if(local.tm_hour*60+local.tm_min>=sunsetMinutes && local.tm_hour<23) status="Scheduled lighting window";
  else status="Waiting for sunset";
  String s="{";
  s+="\"power\":"+String(powerOn?"true":"false");
  s+=",\"schedule\":"+String(scheduleEnabled?"true":"false");
  s+=",\"brightness\":"+String(brightness)+",\"kelvin\":"+String(kelvin);
  s+=",\"sunsetBrightness\":"+String(sunsetBrightness)+",\"sunsetKelvin\":"+String(sunsetKelvin);
  s+=",\"sunset\":"+jsonEscape(valid?fmtTime(sunsetMinutes):"Clock syncing");
  s+=",\"scheduleStatus\":"+jsonEscape(status);
  s+=",\"clock\":"+jsonEscape(String(clockText));
  s+=",\"rssi\":"+jsonEscape(WiFi.status()==WL_CONNECTED?String(WiFi.RSSI())+" dBm":"AP mode");
  s+=",\"ip\":"+jsonEscape(apMode?WiFi.softAPIP().toString():WiFi.localIP().toString());
  unsigned long seconds=millis()/1000;
  s+=",\"uptime\":"+jsonEscape(String(seconds/86400)+"d "+String((seconds/3600)%24)+"h "+String((seconds/60)%60)+"m");
  s+="}";
  server.send(200,"application/json",s);
}
void setupRoutes() {
  server.on("/",HTTP_GET,[](){server.send_P(200,"text/html",PAGE);});
  server.on("/api/state",HTTP_GET,sendState);
  server.on("/api/set",HTTP_GET,[](){
    bool manual=false;
    if(server.hasArg("power")){powerOn=server.arg("power")=="1";manual=true;}
    if(server.hasArg("brightness")){brightness=constrain(server.arg("brightness").toInt(),0,100);manual=true;}
    if(server.hasArg("kelvin")){kelvin=constrain(server.arg("kelvin").toInt(),MIN_K,MAX_K);manual=true;}
    if(server.hasArg("scene")){
      String x=server.arg("scene");int comma=x.indexOf(',');
      if(comma>0){kelvin=constrain(x.substring(0,comma).toInt(),MIN_K,MAX_K);brightness=constrain(x.substring(comma+1).toInt(),0,100);powerOn=true;manual=true;}
    }
    if(server.hasArg("schedule")){
      scheduleEnabled=server.arg("schedule")=="1";
      if(scheduleEnabled){lastStartDate=0;lastEndDate=0;manualOverrideDate=0;}
    }
    if(server.hasArg("sunsetBrightness")) sunsetBrightness=constrain(server.arg("sunsetBrightness").toInt(),0,100);
    if(server.hasArg("sunsetKelvin")) sunsetKelvin=constrain(server.arg("sunsetKelvin").toInt(),MIN_K,MAX_K);
    if(manual && scheduleEnabled && timeReady){time_t now=time(nullptr);tm local;localtime_r(&now,&local);manualOverrideDate=dateKey(local);}
    changed();observeLightState("manual");observeSettings("manual");sendState();
  });
  server.begin();
}
void connectWifi() {
  WiFi.mode(WIFI_STA);WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  uint32_t started=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-started<12000) delay(200);
  if(WiFi.status()==WL_CONNECTED) {
    Serial.print("Wi-Fi IP: ");Serial.println(WiFi.localIP());
    if(MDNS.begin("solstice")){MDNS.addService("http","tcp",80);Serial.println("http://solstice.local");}
    configTzTime(TZ_PACIFIC,"pool.ntp.org","time.google.com");
  } else {
    apMode=true;WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID,AP_PASSWORD);
    Serial.print("Setup AP IP: ");Serial.println(WiFi.softAPIP());
  }
}
void setup() {
  Serial.begin(115200);pwmInit();
  prefs.begin("solstice",false);
  brightness=constrain(prefs.getInt("bright",70),0,100);
  kelvin=constrain(prefs.getInt("kelvin",3000),MIN_K,MAX_K);
  powerOn=prefs.getBool("power",false);
  scheduleEnabled=prefs.getBool("schedule",false);
  sunsetBrightness=constrain(prefs.getInt("sbright",70),0,100);
  sunsetKelvin=constrain(prefs.getInt("skelvin",3000),MIN_K,MAX_K);
  lastObservedPower = powerOn;
  committedBrightness = pendingBrightness = brightness;
  committedKelvin = pendingKelvin = kelvin;
  recalc();connectWifi();setupRoutes();
  mqttTransport.setConnectionTimeout(MQTT_TCP_CONNECT_TIMEOUT_MS);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setSocketTimeout(MQTT_SOCKET_TIMEOUT_SEC);
  mqtt.setBufferSize(256);
  Serial.println("Solstice V4 ready");
}
void loop() {
  server.handleClient();fadeTick();saveTick();settingsTick();mqttTick();
  static uint32_t lastSchedule=0;
  if(millis()-lastSchedule>=1000){lastSchedule=millis();scheduleTick();}
}

