 Smart Energy Monitor — ESP32 Access Point Edition

No router. No cloud. No internet needed.
ESP32 creates its own WiFi hotspot → connect your phone → open the browser.

---

## How to use

| Step | Action |
|------|--------|
| 1 | Upload `energy_monitor_ap.ino` to your ESP32 |
| 2 | On your phone, go to **WiFi settings** |
| 3 | Connect to **`EnergyMonitor`** (password: `12345678`) |
| 4 | Open **`http://192.168.4.1`** in any browser |
| 5 | See live data, saving analysis, controls |

---

## Hardware needed  (~₹560 total)

| Part | Pin | Cost |
|------|-----|------|
| ESP32 DevKit v1 | — | ₹250 |
| ACS712-5A current sensor | GPIO 34 | ₹80 |
| ZMPT101B voltage sensor | GPIO 35 | ₹120 |
| 5V relay module (active-LOW) | GPIO 26 | ₹60 |
| Jumper wires | — | ₹50 |

---

## Wiring

```
230V AC Mains
     │
     ├──── ZMPT101B ──── GPIO 35   (voltage sensing)
     │
     ├──── ACS712   ──── GPIO 34   (current sensing)
     │
     └──── Relay COM ─── Live wire in
           Relay NO  ──── Live wire out to load
           Relay IN  ──── GPIO 26
```

> ⚠️ Have a qualified electrician do the mains-side wiring.

---

## Calibration (one time)

1. Connect a known load (100W bulb)
2. Open dashboard → note Voltage reading
3. Measure actual voltage with a multimeter
4. In the `.ino`, change `VSCALE` until both match
5. Repeat for current if needed (`ISCALE`)

---

## Dashboard features

| Section | What it shows |
|---------|--------------|
| Live Readings | Vrms, Irms, W, VA, VAR, PF, THD, Min/Max |
| Energy & Cost | kWh today/month/total · ₹ cost (Indian tariff slabs) |
| Cost Saving Analysis | Peak hour · Standby load · ₹ saving potential · Tips |
| Protection | Current/voltage vs thresholds · Relay state · Trip count |
| Controls | Relay ON/OFF · Reset Trip · Clear Day · Clear All |
| 24-Hour Chart | Rolling average watts per hour slot |

---

## Auto cutoff thresholds (edit in `.ino`)

| Fault | Default | Behaviour |
|-------|---------|-----------|
| Overcurrent | > 10 A | Relay trips after 600 ms |
| Overvoltage | > 265 V | Relay trips after 600 ms |
| Undervoltage | < 185 V | Relay trips after 600 ms |

Reset from dashboard or `GET http://192.168.4.1/reset`

---

## How the saving analysis works

1. Every 60 seconds the code updates a **rolling average** for the current hour slot
2. After a few hours it can identify the **peak hour** (highest average load)
3. The **baseline** is the mean of the lowest 5 non-trivial hours (standby estimate)
4. Saving estimate:
   ```
   Saveable kWh/month = (peak_W − baseline_W) / 1000 × 4h × 30 days
   Saving ₹ = cost(current_kWh) − cost(current_kWh − saveable_kWh)
   ```
5. A tip string is built dynamically:
   - Shift loads away from peak hour
   - Unplug idle devices if standby > 60 W
   - Add capacitor bank if PF < 0.85
   - Check non-linear loads if THD > 8%

---

## JSON API

```
GET http://192.168.4.1/api
```
Returns all live values, hourly array, trip state, saving analysis — useful for
connecting to a Python script or any external dashboard.

---

## Library needed

Arduino IDE → Sketch → Include Library → Manage Libraries → search:
**ArduinoJson** by Benoit Blanchon — install v6 or v7

---

## Tariff customisation

Edit the `TARIFF` array in the `.ino` to match your state EB rates:
```cpp
const Slab TARIFF[] = {
  {  50.f,  2.35f },   // 0–50 units  @ ₹2.35/unit
  { 100.f,  3.50f },   // 51–100 units
  { 200.f,  5.15f },
  { 500.f,  6.30f },
  {9999.f,  7.50f },
};
const float FIXED = 50.0f;   // ₹/month fixed charge
```

---

## Resume bullet point

> "Built standalone IoT energy monitoring system on ESP32 (AP mode, no internet);
> measures Vrms, Irms, kWh, PF, THD with auto relay cutoff on fault conditions;
> 24-hour load pattern analysis and Indian tariff-based cost saving recommendations
> served as a real-time web dashboard. [GitHub ↗]"
