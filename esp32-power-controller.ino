/*
 * ESP32 Hot Water Cylinder Controller
 * 
 * Works alongside the Arduino Nano power diverter to:
 * - Monitor cumulative on-time of the hot water cylinder via interrupt
 * - Calculate seasonal heating requirements (more in winter, less in summer)
 * - Apply overnight top-up boost if solar diversion was insufficient
 * - Provide remote monitoring and control via Blynk
 * 
 * The power diverter only activates when there's surplus solar, so on cloudy
 * days the cylinder may not reach temperature. This controller ensures minimum
 * heating by topping up overnight when needed.
 */

#include "config.h"           // WiFi and Blynk credentials (gitignored)
#include <WiFi.h>
#include <time.h>
#include <sntp.h>
#include <BlynkSimpleEsp32.h>
#include "non_blocking_delay.h"

// =============================================================================
// Hardware Configuration
// =============================================================================

#define PIN_DIVERTER_STATUS 32  // Input: reads status from Nano (HIGH = load on)
#define PIN_BOOST_SIGNAL    33  // Output: signal to Nano to force boost (active HIGH)

// =============================================================================
// Heating Requirements Configuration
// =============================================================================

// Default hours of heating required per day (adjustable via Blynk)
float requiredHoursSummer = 4.0;
float requiredHoursWinter = 5.5;

// Night-time top-up schedule (adjustable via Blynk)
int alarmHour = 2;
int alarmMinute = 45;

// Timezone: POSIX format from https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
const char* TIMEZONE = "NZST-12NZDT-13,M9.4.0/02:00:00,M4.1.0/03:00:00";

// =============================================================================
// State Variables
// =============================================================================

// On-time tracking
volatile bool isrPending = false;
unsigned long pulseStartUs = 0;
unsigned long pulseStartMs = 0;           // Backup for microsecond overflow protection
unsigned long cumulativeOnTimeMs = 0;
unsigned long pulseCount = 0;
float cumulativeOnTimeHours = 0.0;

// Daily cycle control
enum class DailyState {
  WAITING_FOR_ALARM,    // Normal daytime operation, counting on-time
  BOOST_IN_PROGRESS,    // Currently applying boost
  CYCLE_COMPLETE        // Boost done (or skipped), waiting for new day
};
DailyState dailyState = DailyState::WAITING_FOR_ALARM;
int lastCompletedDay = -1;  // Day of year when last cycle completed (-1 = never)
int currentDayOfYear = 0;   // Tracks day during boost for completion

// Reboot protection: prevents unnecessary heating after power cut on sunny day
// Defaults to false - must complete one full daily cycle before top-ups are allowed
bool hasCompletedDailyCycle = false;

// Calculated values
float requiredHoursToday = 0.0;
float shortfallHours = 0.0;

// Manual boost from Blynk
bool manualBoostActive = false;

// Non-blocking boost control
unsigned long boostStartMs = 0;
unsigned long boostDurationMs = 0;

// Uptime tracking
time_t bootTime;

// Status strings for Blynk
char uptimeString[32];
char lastCycleString[64];

// Blynk timer
BlynkTimer timer;

// =============================================================================
// Blynk Virtual Pin Handlers
// =============================================================================

// V1: Alarm hour
BLYNK_WRITE(V1) {
  alarmHour = param.asInt();
  Serial.printf("Alarm hour set to %d\n", alarmHour);
}

// V2: Alarm minute
BLYNK_WRITE(V2) {
  alarmMinute = param.asInt();
  Serial.printf("Alarm minute set to %d\n", alarmMinute);
}

// V3: Summer heating hours
BLYNK_WRITE(V3) {
  requiredHoursSummer = param.asFloat();
  Serial.printf("Summer hours set to %.2f\n", requiredHoursSummer);
}

// V4: Winter heating hours
BLYNK_WRITE(V4) {
  requiredHoursWinter = param.asFloat();
  Serial.printf("Winter hours set to %.2f\n", requiredHoursWinter);
}

// V6: Manual boost toggle
BLYNK_WRITE(V6) {
  manualBoostActive = param.asInt() == 1;
  digitalWrite(PIN_BOOST_SIGNAL, manualBoostActive ? HIGH : LOW);
  Serial.printf("Manual boost %s\n", manualBoostActive ? "ACTIVATED" : "deactivated");
}

// =============================================================================
// Interrupt Service Routine
// =============================================================================

void IRAM_ATTR onDiverterStateChange() {
  isrPending = true;
}

// =============================================================================
// Setup
// =============================================================================

void setup() {
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println("ESP32 Hot Water Cylinder Controller");
  Serial.println("========================================\n");
  
  // Configure GPIO
  pinMode(PIN_DIVERTER_STATUS, INPUT);
  pinMode(PIN_BOOST_SIGNAL, OUTPUT);
  digitalWrite(PIN_BOOST_SIGNAL, LOW);
  
  // Attach interrupt for on-time tracking
  attachInterrupt(digitalPinToInterrupt(PIN_DIVERTER_STATUS), onDiverterStateChange, CHANGE);
  
  // Connect to WiFi and Blynk
  Serial.println("Connecting to WiFi and Blynk...");
  Blynk.begin(BLYNK_AUTH, WIFI_SSID, WIFI_PASSWORD);
  
  // Sync settings from Blynk server
  Blynk.syncAll();
  
  // Initialise NTP time
  initTime();
  printLocalTime();
  
  // Record boot time for uptime calculation
  bootTime = time(NULL);
  
  // Set up periodic tasks
  timer.setInterval(7000L, updateOnTimeAccumulator);   // Tally on-time
  timer.setInterval(60000L, checkDailyAlarm);          // Check alarm each minute
  timer.setInterval(30000L, maintainWifiConnection);   // WiFi health check
  
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  Serial.println("\nNOTE: Reboot protection active - first daily cycle must complete");
  Serial.println("      before overnight top-ups will be applied.\n");
  Serial.println("Ready.\n");
}

// =============================================================================
// Main Loop
// =============================================================================

void loop() {
  // Handle pending interrupt - track on/off transitions
  if (isrPending) {
    handleDiverterTransition();
    isrPending = false;
  }
  
  // Handle non-blocking boost if in progress
  if (dailyState == DailyState::BOOST_IN_PROGRESS) {
    processBoost();
  }
  
  // Run Blynk (must be called frequently)
  Blynk.run();
  timer.run();
}

// =============================================================================
// On-Time Tracking
// =============================================================================

/*
 * Called from loop() when ISR sets flag.
 * Tracks rising and falling edges to measure on-time.
 */
void handleDiverterTransition() {
  if (digitalRead(PIN_DIVERTER_STATUS) == HIGH) {
    // Rising edge: load just turned ON
    pulseStartUs = micros();
    pulseStartMs = millis();
    pulseCount++;
  } else {
    // Falling edge: load just turned OFF
    // Use microseconds for precision, but track milliseconds as backup
    // (micros() overflows after ~71.5 minutes)
    unsigned long durationUs = micros() - pulseStartUs;
    unsigned long durationMs = millis() - pulseStartMs;
    
    // If microsecond counter likely overflowed, use millisecond backup
    if (durationMs > 4200000UL) {  // > 70 minutes
      cumulativeOnTimeMs += durationMs;
    } else {
      cumulativeOnTimeMs += durationUs / 1000UL;
    }
  }
}

/*
 * Periodic task: updates cumulative on-time and sends to Blynk.
 * 
 * Note: When the cylinder thermostat reaches temperature, it locks out the
 * element but the diverter status may stay HIGH for hours (surplus power with
 * nowhere to go). We still update Blynk so the display stays current.
 * Any in-progress pulse time isn't included until the falling edge, but that's
 * acceptable — it just means "current pulse" time appears on the next update.
 */
void updateOnTimeAccumulator() {
  cumulativeOnTimeHours = cumulativeOnTimeMs / 3600000.0f;
  Blynk.virtualWrite(V7, cumulativeOnTimeHours);
  Blynk.virtualWrite(V10, pulseCount);
}

// =============================================================================
// Daily Alarm and Boost Logic
// =============================================================================

/*
 * Periodic task: checks time and manages daily heating cycle.
 */
void checkDailyAlarm() {
  // Update and send uptime to Blynk
  sendUptimeToBlynk();
  
  // Get current time
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to get local time");
    return;
  }
  
  int currentHour = timeinfo.tm_hour;
  int currentMinute = timeinfo.tm_min;
  int dayOfYear = timeinfo.tm_yday + 1;  // tm_yday is 0-364
  
  Serial.printf("Day %d, %02d:%02d | On-time: %.2fh | State: %s\n",
                dayOfYear, currentHour, currentMinute, cumulativeOnTimeHours,
                getDailyStateString());
  
  // State machine for daily cycle
  switch (dailyState) {
    
    case DailyState::WAITING_FOR_ALARM:
      // Check if alarm time reached AND we haven't already run today
      if (currentHour == alarmHour && 
          currentMinute >= alarmMinute && 
          dayOfYear != lastCompletedDay) {
        Serial.println("\n*** ALARM TRIGGERED ***");
        printLocalTime();
        calculateHeatingRequirement(dayOfYear);
        initiateBoostIfNeeded(dayOfYear);
      }
      break;
      
    case DailyState::BOOST_IN_PROGRESS:
      // Handled in loop() via processBoost()
      break;
      
    case DailyState::CYCLE_COMPLETE:
      // Automatically reset when a new day begins
      if (dayOfYear != lastCompletedDay) {
        dailyState = DailyState::WAITING_FOR_ALARM;
        Serial.println("New day detected - daily cycle reset");
      }
      break;
  }
}

/*
 * Calculate required heating hours based on day of year.
 * Uses cosine curve: maximum at winter solstice (June 21 in NZ), minimum at summer.
 */
void calculateHeatingRequirement(int dayOfYear) {
  // Ensure winter >= summer
  float winterH = max(requiredHoursWinter, requiredHoursSummer);
  float summerH = min(requiredHoursSummer, requiredHoursWinter);
  
  // Offset so day 0 = winter solstice (June 21 ≈ day 172)
  // Using 171 as offset (0-indexed)
  int offsetDay = ((dayOfYear - 171) % 365 + 365) % 365;  // Handle negative modulo
  
  // Cosine curve: peaks at 0 (winter solstice), troughs at 182 (summer solstice)
  float amplitude = 0.5f * (winterH - summerH);
  requiredHoursToday = amplitude * cos(0.01721f * offsetDay) + amplitude + summerH;
  
  // Calculate shortfall
  shortfallHours = requiredHoursToday - cumulativeOnTimeHours;
  
  Serial.printf("Season calculation:\n");
  Serial.printf("  Day of year: %d (offset: %d)\n", dayOfYear, offsetDay);
  Serial.printf("  Range: %.1fh (summer) to %.1fh (winter)\n", summerH, winterH);
  Serial.printf("  Required today: %.2fh\n", requiredHoursToday);
  Serial.printf("  Actual on-time: %.2fh\n", cumulativeOnTimeHours);
  Serial.printf("  Shortfall: %.2fh\n", shortfallHours);
  
  Blynk.virtualWrite(V11, shortfallHours);
}

/*
 * Decides whether to apply boost based on shortfall and reboot protection.
 */
void initiateBoostIfNeeded(int dayOfYear) {
  if (shortfallHours <= 0) {
    // No boost needed - cylinder had enough solar heating
    Serial.printf("No boost needed. Required %.2fh, received %.2fh\n",
                  requiredHoursToday, cumulativeOnTimeHours);
    completeDailyCycle(dayOfYear);
    return;
  }
  
  if (!hasCompletedDailyCycle) {
    // Reboot protection: skip boost if we haven't tracked a full day
    Serial.println("\n*** BOOST SKIPPED - REBOOT PROTECTION ***");
    Serial.println("Device rebooted during day - cannot verify actual on-time.");
    Serial.println("Assuming cylinder may already be hot. Will track normally from now.\n");
    completeDailyCycle(dayOfYear);
    return;
  }
  
  // Start non-blocking boost
  boostDurationMs = (unsigned long)(shortfallHours * 3600000.0f);
  boostStartMs = millis();
  currentDayOfYear = dayOfYear;  // Save for completion
  dailyState = DailyState::BOOST_IN_PROGRESS;
  
  digitalWrite(PIN_BOOST_SIGNAL, HIGH);
  Serial.printf("\n*** STARTING BOOST: %.2f hours (%.0f ms) ***\n",
                shortfallHours, (float)boostDurationMs);
}

/*
 * Non-blocking boost processing - called from loop().
 * Allows Blynk to keep running during boost.
 */
void processBoost() {
  static unsigned long lastProgressReport = 0;
  
  unsigned long elapsed = millis() - boostStartMs;
  
  // Progress report every minute
  if (millis() - lastProgressReport > 60000) {
    float remainingMins = (boostDurationMs - elapsed) / 60000.0f;
    Serial.printf("Boost in progress... %.1f minutes remaining\n", remainingMins);
    lastProgressReport = millis();
  }
  
  // Check if boost complete
  if (elapsed >= boostDurationMs) {
    digitalWrite(PIN_BOOST_SIGNAL, LOW);
    Serial.println("\n*** BOOST COMPLETE ***\n");
    completeDailyCycle(currentDayOfYear);
  }
}

/*
 * Called when daily cycle finishes (with or without boost).
 */
void completeDailyCycle(int dayOfYear) {
  // Record which day we completed - prevents re-triggering today
  lastCompletedDay = dayOfYear;
  
  // Reset counters for new day
  cumulativeOnTimeMs = 0;
  cumulativeOnTimeHours = 0.0;
  pulseCount = 0;
  shortfallHours = 0.0;
  
  // Mark that we've completed a full cycle (enables future boosts after reboot)
  hasCompletedDailyCycle = true;
  
  // Record completion time
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    strftime(lastCycleString, sizeof(lastCycleString),
             "%A, %B %d, %I:%M %p", &timeinfo);
  }
  Serial.printf("Daily cycle completed at: %s\n\n", lastCycleString);
  
  // Update Blynk
  maintainWifiConnection();  // Ensure connected before writes
  Blynk.virtualWrite(V7, cumulativeOnTimeHours);
  Blynk.virtualWrite(V10, pulseCount);
  Blynk.virtualWrite(V8, lastCycleString);
  
  dailyState = DailyState::CYCLE_COMPLETE;
}

/*
 * Returns string description of current daily state.
 */
const char* getDailyStateString() {
  switch (dailyState) {
    case DailyState::WAITING_FOR_ALARM:  return "Waiting";
    case DailyState::BOOST_IN_PROGRESS:  return "Boosting";
    case DailyState::CYCLE_COMPLETE:     return "Complete";
    default:                             return "Unknown";
  }
}

// =============================================================================
// Time Functions
// =============================================================================

void initTime() {
  Serial.println("Initialising NTP time...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10000)) {
    Serial.println("  Failed to obtain time from NTP");
    return;
  }
  Serial.println("  NTP time obtained");
  
  // Set timezone
  setenv("TZ", TIMEZONE, 1);
  tzset();
  Serial.printf("  Timezone set: %s\n", TIMEZONE);
}

void printLocalTime() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S %Z");
  }
}

// =============================================================================
// Utility Functions
// =============================================================================

void sendUptimeToBlynk() {
  time_t now = time(NULL);
  unsigned long uptimeSec = difftime(now, bootTime);
  
  unsigned long days = uptimeSec / 86400;
  unsigned long hours = (uptimeSec % 86400) / 3600;
  unsigned long mins = (uptimeSec % 3600) / 60;
  
  snprintf(uptimeString, sizeof(uptimeString), "%lud %luh %lum", days, hours, mins);
  Blynk.virtualWrite(V5, uptimeString);
}

void maintainWifiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected - reconnecting...");
    WiFi.disconnect();
    WiFi.reconnect();
  }
}
