/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║   SMART ENERGY MONITOR  —  ESP32  Access Point  v3          ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║  HOW IT WORKS                                                ║
 * ║  ESP32 creates its own WiFi hotspot — no router needed.      ║
 * ║  Connect phone/laptop to that hotspot, open 192.168.4.1.    ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║  FEATURES                                                    ║
 * ║  Real-time: Vrms, Irms, W, VA, VAR, PF, THD                 ║
 * ║  Energy: kWh today/month/total  +  Indian tariff ₹ cost      ║
 * ║  Auto relay cutoff: overcurrent / overvoltage / undervoltage ║
 * ║  24-hour load profile stored in RAM (rolling avg per hour)   ║
 * ║  Pattern analysis: peak hour, baseline, ₹ saving estimate    ║
 * ║  Dark-mode dashboard, auto-refresh every 2 s                 ║
 * ║  JSON API at /api                                            ║
 * ║  EEPROM persistence (totals survive power cuts)              ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║  WIRING                                                      ║
 * ║  ACS712  OUT  → GPIO 34  (ADC, input only pin)               ║
 * ║  ZMPT101B OUT → GPIO 35  (ADC, input only pin)               ║
 * ║  Relay    IN  → GPIO 26  (active-LOW relay module)           ║
 * ║  LED            GPIO 2   (built-in)                          ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║  LIBRARY NEEDED  (Sketch → Include Library → Manage)        ║
 * ║  ArduinoJson by Benoit Blanchon  (v6 or v7)                  ║
 * ╚══════════════════════════════════════════════════════════════╝
 */

#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <math.h>

// ── 1. ACCESS POINT CONFIG ─────────────────────────────────────
const char*     AP_SSID = "EnergyMonitor";   // hotspot name
const char*     AP_PASS = "12345678";        // min 8 chars
const IPAddress AP_IP(192,168,4,1);

// ── 2. PINS ────────────────────────────────────────────────────
#define PIN_I    34   // ACS712 output
#define PIN_V    35   // ZMPT101B output
#define PIN_REL  26   // Relay IN (active-LOW)
#define PIN_LED   2   // built-in LED

// ── 3. CALIBRATION ─────────────────────────────────────────────
// Measure Vrms with a multimeter → adjust VSCALE until they match
// ACS712-5A  → ISCALE 2.5 | 20A → 4.5 | 30A → 6.5
#define VSCALE   220.0f
#define ISCALE     2.5f
#define NSAMPLES   600
#define ADC_MID   2048    // 12-bit midpoint
#define DT_US      150    // µs between samples

// ── 4. PROTECTION THRESHOLDS ───────────────────────────────────
#define TRIP_A    10.0f   // overcurrent (A)
#define TRIP_OV  265.0f   // overvoltage (V)
#define TRIP_UV  185.0f   // undervoltage (V)
#define TRIP_MS    600    // fault hold-time before trip (ms)

// ── 5. TARIFF (edit to your state EB rates) ────────────────────
struct Slab { float lim; float rate; };
const Slab TARIFF[] = {
  { 50.f,2.35f},{100.f,3.50f},{200.f,5.15f},{500.f,6.30f},{9999.f,7.50f}
};
const int   NS    = 5;
const float FIXED = 50.0f;   // ₹/month fixed charge

// ── 6. GLOBALS ─────────────────────────────────────────────────
float gV=0,gI=0,gW=0,gVA=0,gVAR=0,gPF=1,gTHD=0;
float gKwDay=0,gKwMon=0,gKwTot=0,gCDay=0,gCMon=0;
float gMinW=99999,gMaxW=0;

bool   gROn=true,gTrip=false;
String gTripMsg="";
int    gTripN=0;
ulong  gFault=0;

float gHrW[24]={};
int   gHrN[24]={};
int   gPkHr=0;
float gPkW=0,gBase=0,gSave=0;
String gTip="";

ulong gBoot=0,gLastM=0,gLastMin=0,gLastSv=0,gLastHr=0,gLastDay=0;
int   gHour=0;

WebServer server(80);

// ── 7. COST CALC ───────────────────────────────────────────────
float cost(float kwh) {
  float c=FIXED,p=0;
  for(int i=0;i<NS&&kwh>p;i++){
    float u=min(kwh-p,TARIFF[i].lim-p);
    c+=u*TARIFF[i].rate; p=TARIFF[i].lim;
  }
  return c;
}

// ── 8. RELAY ───────────────────────────────────────────────────
void relay(bool on){ gROn=on; digitalWrite(PIN_REL,on?LOW:HIGH); }
void resetTrip(){ gTrip=false;gTripMsg="";gFault=0;relay(true); }

// ── 9. MEASURE ─────────────────────────────────────────────────
void measure(){
  long sv2=0,si2=0,svi=0;
  for(int n=0;n<NSAMPLES;n++){
    long v=(long)analogRead(PIN_V)-ADC_MID;
    long c=(long)analogRead(PIN_I)-ADC_MID;
    sv2+=v*v; si2+=c*c; svi+=v*c;
    delayMicroseconds(DT_US);
  }
  float Vn=sqrtf((float)sv2/NSAMPLES);
  float In=sqrtf((float)si2/NSAMPLES);
  gV=Vn*VSCALE/1000.f;
  gI=In*ISCALE/1000.f;
  gVA=gV*gI;
  gW=fabsf((float)svi/NSAMPLES*VSCALE*ISCALE/1e6f);
  if(gVA>0.5f){
    gPF=constrain(gW/gVA,0.f,1.f);
    gVAR=sqrtf(max(0.f,gVA*gVA-gW*gW));
    gTHD=(gPF<0.999f)?sqrtf(1.f/(gPF*gPF)-1.f)*100.f:0.f;
  } else { gPF=1;gVAR=0;gTHD=0; }
  if(gW>1.f){ gMinW=min(gMinW,gW); gMaxW=max(gMaxW,gW); }
}

// ── 10. ACCUMULATE ─────────────────────────────────────────────
void accum(ulong dt){
  if(!gROn||gTrip)return;
  float k=(gW/1000.f)*(dt/3600000.f);
  gKwDay+=k;gKwMon+=k;gKwTot+=k;
  gCDay=cost(gKwDay); gCMon=cost(gKwMon);
}

// ── 11. PROTECTION ─────────────────────────────────────────────
void protect(){
  bool f=false; String w="";
  if(gI>TRIP_A){f=true;w="Overcurrent "+String(gI,1)+"A";}
  else if(gV>TRIP_OV){f=true;w="Overvoltage "+String(gV,0)+"V";}
  else if(gV<TRIP_UV&&gV>10.f){f=true;w="Undervoltage "+String(gV,0)+"V";}
  if(f){
    if(!gFault)gFault=millis();
    else if(millis()-gFault>TRIP_MS&&!gTrip){
      gTrip=true;gTripMsg=w;gTripN++;relay(false);
      Serial.println("[TRIP] "+w);
    }
  } else gFault=0;
}

// ── 12. PATTERNS ───────────────────────────────────────────────
void patterns(){
  int h=gHour%24;
  gHrN[h]++;
  gHrW[h]+=(gW-gHrW[h])/gHrN[h];   // Welford running mean

  gPkW=0;
  for(int i=0;i<24;i++) if(gHrW[i]>gPkW){gPkW=gHrW[i];gPkHr=i;}

  float buf[24]; int cnt=0;
  for(int i=0;i<24;i++) if(gHrW[i]>5.f)buf[cnt++]=gHrW[i];
  if(cnt<3)return;
  for(int i=0;i<cnt-1;i++)
    for(int j=0;j<cnt-i-1;j++)
      if(buf[j]>buf[j+1]){float t=buf[j];buf[j]=buf[j+1];buf[j+1]=t;}
  float base=0; int tk=min(cnt,5);
  for(int i=0;i<tk;i++)base+=buf[i];
  gBase=base/tk;

  float sk=((gPkW-gBase)/1000.f)*4.f*30.f;
  gSave=max(0.f,cost(gKwMon)-cost(max(0.f,gKwMon-sk)));

  gTip="Shift heavy loads from "+String(gPkHr)+":00-"+String((gPkHr+1)%24)
      +":00 (peak "+String(gPkW,0)+"W). ";
  if(gBase>60.f)gTip+="Standby="+String(gBase,0)+"W-unplug idle devices. ";
  if(gPF<0.85f) gTip+="PF="+String(gPF,2)+" low-add capacitor bank. ";
  if(gTHD>8.f)  gTip+="THD="+String(gTHD,1)+"%-check non-linear loads.";
}

// ── 13. EEPROM ─────────────────────────────────────────────────
#define EE_MG 0xD4
struct EE{uint8_t mg;float tot;float mon;};
void loadEE(){
  EEPROM.begin(64);EE d;EEPROM.get(0,d);
  if(d.mg==EE_MG){gKwTot=d.tot;gKwMon=d.mon;}
}
void saveEE(){EE d={EE_MG,gKwTot,gKwMon};EEPROM.put(0,d);EEPROM.commit();}

// ── 14. COLOUR HELPERS ─────────────────────────────────────────
String cGood(float v,float g,float w){
  return v>=g?"#22c55e":v>=w?"#f59e0b":"#ef4444";
}
String cBad(float v,float w,float b){
  return v<=w?"#22c55e":v<=b?"#f59e0b":"#ef4444";
}

// ── 15. DASHBOARD ──────────────────────────────────────────────
void handleRoot(){
  // Hourly chart arrays
  String hl="[",hd="[";
  for(int i=0;i<24;i++){
    hl+="\""+String(i)+"h\""+(i<23?",":"");
    hd+=String(gHrW[i],1)+(i<23?",":"");
  }
  hl+="]";hd+="]";

  ulong us=(millis()-gBoot)/1000;
  String up=String(us/3600)+"h "+String((us%3600)/60)+"m "+String(us%60)+"s";
  String rc=gTrip?"#ef4444":gROn?"#22c55e":"#f59e0b";
  String rt=gTrip?"TRIPPED":gROn?"LIVE":"OFF";

  String p;p.reserve(13000);

  p=F("<!DOCTYPE html><html lang='en'><head>"
  "<meta charset='UTF-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>Energy Monitor</title>"
  "<script src='https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js'>"
  "</script><style>"
  "*{box-sizing:border-box;margin:0;padding:0}"
  "body{background:#0f172a;color:#f1f5f9;font-family:'Segoe UI',system-ui,sans-serif;padding:14px}"
  ".hdr{display:flex;justify-content:space-between;align-items:flex-start;flex-wrap:wrap;gap:8px;margin-bottom:16px}"
  "h1{font-size:1.1rem;font-weight:700;color:#38bdf8}"
  ".sub{font-size:.7rem;color:#475569;margin-top:3px}"
  ".pill{padding:4px 13px;border-radius:20px;font-size:.72rem;font-weight:700;color:#fff;white-space:nowrap}"
  ".g4{display:grid;grid-template-columns:repeat(auto-fill,minmax(136px,1fr));gap:9px;margin-bottom:12px}"
  ".card{background:#1e293b;border:1px solid #334155;border-radius:11px;padding:12px}"
  ".lbl{font-size:.62rem;color:#475569;text-transform:uppercase;letter-spacing:.06em;margin-bottom:3px}"
  ".val{font-size:1.48rem;font-weight:700;line-height:1.1}"
  ".unit{font-size:.67rem;color:#475569;margin-top:2px}"
  ".sec{font-size:.68rem;font-weight:600;text-transform:uppercase;letter-spacing:.08em;"
  "color:#334155;margin:13px 0 7px;padding-bottom:4px;border-bottom:1px solid #1e293b}"
  ".trip{background:#450a0a;border:1px solid #dc2626;border-radius:10px;"
  "padding:12px;margin-bottom:12px;font-size:.82rem;line-height:1.65}"
  ".sbox{background:#022c22;border:1px solid #166534;border-radius:11px;"
  "padding:13px;margin-bottom:12px}"
  ".stitle{font-size:.67rem;font-weight:600;text-transform:uppercase;"
  "letter-spacing:.06em;color:#4ade80;margin-bottom:9px}"
  ".sr{display:flex;justify-content:space-between;align-items:center;"
  "font-size:.8rem;padding:5px 0;border-bottom:1px solid #134e2a}"
  ".sr:last-child{border:none}"
  ".stip{font-size:.77rem;color:#86efac;line-height:1.6;margin-top:8px;"
  "padding-top:8px;border-top:1px solid #134e2a}"
  ".pr{display:flex;justify-content:space-between;font-size:.79rem;"
  "padding:5px 0;border-bottom:1px solid #1e293b}"
  ".pr:last-child{border:none}"
  ".ok{color:#22c55e}.warn{color:#f59e0b}.bad{color:#ef4444}"
  ".btns{display:flex;flex-wrap:wrap;gap:7px;margin-bottom:12px}"
  "a.btn{padding:8px 15px;border-radius:8px;font-size:.78rem;font-weight:600;"
  "text-decoration:none;display:inline-block}"
  "@keyframes blink{0%,100%{opacity:1}50%{opacity:.15}}"
  ".dot{display:inline-block;width:7px;height:7px;border-radius:50%;"
  "background:#22c55e;animation:blink 1.8s infinite;margin-right:5px;vertical-align:middle}"
  "@media(max-width:400px){.g4{grid-template-columns:1fr 1fr}}"
  "</style></head><body>");

  // Header
  p+="<div class='hdr'><div>"
     "<h1><span class='dot'></span>Smart Energy Monitor</h1>"
     "<div class='sub'>Uptime: "+up
     +"  |  Connect: <b>"+String(AP_SSID)+"</b> → 192.168.4.1</div>"
     "</div><span class='pill' style='background:"+rc+"'>"+rt+"</span></div>";

  // Trip banner
  if(gTrip){
    p+="<div class='trip'>🚨 <b>RELAY TRIPPED</b> — "+gTripMsg
      +"<br><a href='/reset' class='btn' "
       "style='background:#16a34a;color:#fff;margin-top:8px;display:inline-block'>"
       "↺ Reset &amp; Reconnect</a></div>";
  }

  // Card helper lambda (as function to keep size manageable)
  auto C=[](String l,String v,String u,String c)->String{
    return "<div class='card'><div class='lbl'>"+l+"</div>"
           "<div class='val' style='color:"+c+"'>"+v+"</div>"
           "<div class='unit'>"+u+"</div></div>";
  };

  // Live readings
  p+="<div class='sec'>Live Readings</div><div class='g4'>";
  p+=C("Voltage",    String(gV,1),  "V rms", "#38bdf8");
  p+=C("Current",    String(gI,3),  "A rms", "#fb923c");
  p+=C("Real Power", String(gW,1),  "Watts", "#22c55e");
  p+=C("Apparent",   String(gVA,1), "VA",    "#a78bfa");
  p+=C("Reactive",   String(gVAR,1),"VAR",   "#f472b6");
  p+=C("Power Factor",String(gPF,3),"",      cGood(gPF,0.9f,0.75f));
  p+=C("THD",        String(gTHD,1),"%",     cBad(gTHD,5,10));
  p+=C("Min/Max",    String(gMinW==99999?0:gMinW,0)+"/"+String(gMaxW,0),"W","#64748b");
  p+="</div>";

  // Energy & cost
  p+="<div class='sec'>Energy &amp; Cost</div><div class='g4'>";
  p+=C("Today",      String(gKwDay,3),  "kWh", "#38bdf8");
  p+=C("This Month", String(gKwMon,2),  "kWh", "#38bdf8");
  p+=C("All Time",   String(gKwTot,2),  "kWh", "#475569");
  p+=C("Cost Today", "₹"+String(gCDay,2),  "", "#22c55e");
  p+=C("Cost Month", "₹"+String(gCMon,2),   "", "#f59e0b");
  p+=C("Trip Count", String(gTripN),"total",gTripN?"#ef4444":"#22c55e");
  p+="</div>";

  // ── Saving analysis ───────────────────────────────────────
  p+="<div class='sec'>Cost Saving Analysis</div><div class='sbox'>"
     "<div class='stitle'>💰 Pattern-Based Recommendations</div>";

  if(gKwMon>0.05f){
    float pNow=cost(gKwMon), p85=cost(gKwMon*0.85f);
    p+="<div class='sr'><span>Peak hour</span>"
       "<span style='color:#fbbf24;font-weight:600'>"
       +String(gPkHr)+":00 &ndash; "+String((gPkHr+1)%24)+":00</span></div>";
    p+="<div class='sr'><span>Peak load</span><span>"+String(gPkW,0)+" W</span></div>";
    p+="<div class='sr'><span>Standby load</span><span>"+String(gBase,0)+" W</span></div>";
    p+="<div class='sr'><span>Est. monthly bill</span>"
       "<span style='color:#f87171'>₹"+String(pNow,0)+"</span></div>";
    p+="<div class='sr'><span>If 15% less usage</span>"
       "<span style='color:#4ade80'>₹"+String(p85,0)+"</span></div>";
    p+="<div class='sr'><span><b>Saving potential</b></span>"
       "<span style='color:#4ade80;font-weight:700'>₹"
       +String(gSave>0?gSave:pNow-p85,0)+" / month</span></div>";
    if(gTip.length())
      p+="<div class='stip'>💡 "+gTip+"</div>";
  } else {
    p+="<div class='stip' style='color:#475569'>"
       "Accumulating data — pattern analysis starts after a few minutes.</div>";
  }
  p+="</div>";

  // Protection
  String ic=(gI<TRIP_A*0.8f)?"ok":(gI<TRIP_A)?"warn":"bad";
  String vc=(gV>TRIP_UV&&gV<TRIP_OV)?"ok":"bad";
  String pc=(gPF>0.9f)?"ok":(gPF>0.75f)?"warn":"bad";
  auto PR=[](String l,String v,String c)->String{
    return "<div class='pr'><span>"+l+"</span><span class='"+c+"'>"+v+"</span></div>";
  };
  p+="<div class='sec'>Protection</div>"
     "<div class='card' style='margin-bottom:12px'>";
  p+=PR("Current",      String(gI,2)+"A  (trip &gt;"+String(TRIP_A,0)+"A)",   ic);
  p+=PR("Voltage",      String(gV,1)+"V  ("+String(TRIP_UV,0)+"–"+String(TRIP_OV,0)+"V OK)",vc);
  p+=PR("Power factor", String(gPF,3),                                        pc);
  p+=PR("Relay",        gROn?"CLOSED — load ON":"OPEN — load OFF",            gROn?"ok":"bad");
  p+=PR("Trips total",  String(gTripN),                                       gTripN?"warn":"ok");
  p+="</div>";

  // Controls
  p+="<div class='sec'>Controls</div><div class='btns'>"
     "<a href='/relay/on'  class='btn' style='background:#15803d;color:#fff'>▶ Relay ON</a>"
     "<a href='/relay/off' class='btn' style='background:#b91c1c;color:#fff'>■ Relay OFF</a>"
     "<a href='/reset'     class='btn' style='background:#0369a1;color:#fff'>↺ Reset Trip</a>"
     "<a href='/clearday'  class='btn' style='background:#92400e;color:#fff'>🗑 Clear Day</a>"
     "<a href='/clearall'  class='btn' style='background:#334155;color:#f1f5f9'>🗑 Clear All</a>"
     "</div>";

  // Chart
  p+="<div class='sec'>24-Hour Load Profile</div>"
     "<div class='card'><canvas id='hc' style='max-height:190px'></canvas></div>"
     "<script>new Chart(document.getElementById('hc'),{"
     "type:'bar',"
     "data:{labels:"+hl+","
     "datasets:[{label:'Avg W',data:"+hd+","
     "backgroundColor:'rgba(56,189,248,0.4)',"
     "borderColor:'#38bdf8',borderWidth:1,borderRadius:3}]},"
     "options:{responsive:true,plugins:{legend:{display:false}},"
     "scales:{x:{ticks:{color:'#475569',font:{size:9}},grid:{color:'#1e293b'}},"
     "y:{ticks:{color:'#475569'},grid:{color:'#334155'},"
     "title:{display:true,text:'Avg W',color:'#475569',font:{size:9}}}}}}})"
     ";</script>";

  p+="<p style='font-size:.65rem;color:#1e293b;text-align:center;margin-top:12px'>"
     "Auto-refresh 2 s | API: /api | v3.0</p>"
     "<script>setTimeout(()=>location.reload(),2000);</script>"
     "</body></html>";

  server.send(200,"text/html",p);
}

// ── 16. JSON API ───────────────────────────────────────────────
void handleAPI(){
  StaticJsonDocument<600> doc;
  doc["vrms"]=round(gV*10)/10.; doc["irms"]=round(gI*1000)/1000.;
  doc["watts"]=round(gW*10)/10.; doc["va"]=round(gVA*10)/10.;
  doc["var"]=round(gVAR*10)/10.; doc["pf"]=round(gPF*1000)/1000.;
  doc["thd"]=round(gTHD*10)/10.;
  doc["kwh_today"]=gKwDay; doc["kwh_month"]=gKwMon; doc["kwh_total"]=gKwTot;
  doc["cost_today"]=round(gCDay*100)/100.; doc["cost_month"]=round(gCMon*100)/100.;
  doc["relay_on"]=gROn; doc["tripped"]=gTrip;
  doc["trip_reason"]=gTripMsg; doc["trip_count"]=gTripN;
  doc["peak_hour"]=gPkHr; doc["peak_w"]=(int)gPkW;
  doc["baseline_w"]=(int)gBase; doc["saving_rs"]=(int)gSave;
  doc["tip"]=gTip; doc["uptime_s"]=(millis()-gBoot)/1000;
  JsonArray ha=doc.createNestedArray("hourly_w");
  for(int i=0;i<24;i++)ha.add((int)gHrW[i]);
  String out; serializeJson(doc,out);
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.send(200,"application/json",out);
}

// ── 17. ROUTE HANDLERS ─────────────────────────────────────────
void redir(){server.sendHeader("Location","/");server.send(302,"","");}
void onRelayOn() {resetTrip();redir();}
void onRelayOff(){relay(false);redir();}
void onReset()   {resetTrip();redir();}
void onClearDay(){gKwDay=0;gCDay=0;gMinW=99999;gMaxW=0;redir();}
void onClearAll(){
  gKwDay=gKwMon=gKwTot=gCDay=gCMon=0;gMinW=99999;gMaxW=0;gTripN=0;
  memset(gHrW,0,sizeof(gHrW));memset(gHrN,0,sizeof(gHrN));
  gPkW=gBase=gSave=0;gTip="";saveEE();redir();
}

// ── 18. SETUP ──────────────────────────────────────────────────
void setup(){
  Serial.begin(115200);
  Serial.println("\n=== Smart Energy Monitor v3.0 ===");

  pinMode(PIN_REL,OUTPUT); pinMode(PIN_LED,OUTPUT);
  digitalWrite(PIN_REL,LOW);   // active-LOW relay: LOW = energised = load ON
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  loadEE();

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP,AP_IP,IPAddress(255,255,255,0));
  WiFi.softAP(AP_SSID,AP_PASS);
  Serial.println("[AP]  SSID     : "+String(AP_SSID));
  Serial.println("[AP]  Password : "+String(AP_PASS));
  Serial.println("[AP]  Open     : http://192.168.4.1");

  server.on("/",         HTTP_GET,handleRoot);
  server.on("/api",      HTTP_GET,handleAPI);
  server.on("/relay/on", HTTP_GET,onRelayOn);
  server.on("/relay/off",HTTP_GET,onRelayOff);
  server.on("/reset",    HTTP_GET,onReset);
  server.on("/clearday", HTTP_GET,onClearDay);
  server.on("/clearall", HTTP_GET,onClearAll);
  server.begin();
  Serial.println("[HTTP] Ready — connect to '"+String(AP_SSID)+"' then open http://192.168.4.1");

  gBoot=gLastM=gLastMin=gLastSv=gLastHr=gLastDay=millis();
}

// ── 19. LOOP ───────────────────────────────────────────────────
void loop(){
  server.handleClient();
  ulong now=millis();

  // Measure every 500 ms
  if(now-gLastM>=500){
    ulong dt=now-gLastM; gLastM=now;
    measure(); accum(dt);
    if(!gTrip) protect();
    // LED behaviour
    if(gTrip)            digitalWrite(PIN_LED,(now/150)%2);   // fast blink
    else if(gROn)        digitalWrite(PIN_LED,(now/1000)%2);  // slow blink
    else                 digitalWrite(PIN_LED,LOW);            // off
  }

  // Pattern update every 60 s
  if(now-gLastMin>=60000UL){
    gLastMin=now; patterns();
    Serial.printf("[60s] V=%.1f I=%.3f W=%.1f PF=%.3f kWh=%.4f Rs=%.2f save=%.0f\n",
                  gV,gI,gW,gPF,gKwDay,gCMon,gSave);
  }

  // Hour tick
  if(now-gLastHr>=3600000UL){gLastHr=now;gHour=(gHour+1)%24;}

  // EEPROM save every 5 min
  if(now-gLastSv>=300000UL){gLastSv=now;saveEE();}

  // Daily reset at 24 h
  if(now-gLastDay>=86400000UL){gLastDay=now;gKwDay=0;gCDay=0;gMinW=99999;gMaxW=0;}
}
