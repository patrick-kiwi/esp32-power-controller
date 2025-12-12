// RTC demo for ESP32, that includes TZ and DST adjustments
// Get the POSIX style TZ format string from  https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
// Complete project details at: https://RandomNerdTutorials.com/esp32-ntp-timezones-daylight-saving/
// tutorial on linux strftime https://linuxhint.com/gettimeofday_c_language/
// tutorial on reconnecting Blynk after a disconnection.  https://microcontrollerslab.com/reconnect-esp32-to-wifi-after-lost-connection/
// Blynk synch notes http://docs.blynk.cc/#blynk-firmware-virtual-pins-control-blynksyncall
// https://docs.blynk.io/en/getting-started/how-to-display-any-sensor-data-in-blynk-app
//Nice youtube video on setting up Network Time Protocol https://youtu.be/r2UAmBLBBRM


#define BLYNK_TEMPLATE_ID "TMPLI6j7TZkb"
#define BLYNK_TEMPLATE_NAME "ESP32 Power Diverter"
#define BLYNK_AUTH "PYgEJgiUOAhR-B3xtxyi5n1BevqScsiQ"
#define WIFI_SSID "Police_CID_23"
#define WIFI_PASSWORD "eketehuna1234" 


#include <WiFi.h>
#include "time.h"
#include "sntp.h"
#include "non_blocking_delay.h"
#include <BlynkSimpleEsp32.h>

#define GPIO_input 32   //Reads 3.3V signal from an LED
#define GPIO_output 33  //sends high/low signal to the Atmel 328P (pulled low with 1.5K resistor)

//Define global variables
float cumulative_ontime_h = 0.0, shortfall = 0.0, ReqHoursSummer = 4.0, ReqHoursWinter = 5.5, required_hours = 0.0;  //hours of heating required
int manualBoostActivate = 0;
int hour = 0, minute = 0, second = 0, day = 0;  //time setup
int hour_alarm_on = 2, minute_alarm_on = 45;

//global time structure required
struct timeval current_time;  

volatile bool ISR_flag = false;
unsigned long int start_time = 0L;            //high precision microsecond counter
unsigned long int cumulative_us = 0L;         //high precision microsecond counter
unsigned long int start_time_backup_ms = 0L;  //low precision millisecond counter
unsigned long int cumulative_backup_ms = 0L;  //low precision millisecond counter (protect against microsecond buffer overflow)
unsigned long int cumulative_ms = 0L;         //total time during the day the triac has been on
unsigned long int pulse_number = 0L;          //records total number of switching cycles during the day

//global logic controll triggers
bool alarm_triggered = false;           //overall alarm cycle controll
bool first_run = false;                 //makes sure heating cycle only happens once per alarm cycle

char uptime_string[32];
char last_triggered_string[64];

//uptime variables
time_t start, end;
unsigned long u_time=0;
unsigned long uday = 0;
unsigned long uhour = 0;
unsigned long umin = 0;
unsigned long usec = 0;

//Blynk setup
BlynkTimer timer;

BLYNK_WRITE(V1) {  // assigning incoming value from pin V1 to a variable
  hour_alarm_on = param.asInt();
  Serial.printf("Hour trigger changed to %d\n", hour_alarm_on);
  Serial.printf("Triggered state is %d\n", static_cast<int>(alarm_triggered));
}
BLYNK_WRITE(V2) {
  minute_alarm_on = param.asInt();
  Serial.printf("Minute trigger changed to %d\n", minute_alarm_on);
  Serial.printf("Triggered state is %d\n", static_cast<int>(alarm_triggered));
}
BLYNK_WRITE(V3) {
  ReqHoursSummer = param.asFloat();
  Serial.printf("Summer hours of on-time changed to %.2f\n", ReqHoursSummer);
}
BLYNK_WRITE(V4) {
  ReqHoursWinter = param.asFloat();

  Serial.printf("Winter hours of of on-time changed to %.2f\n", ReqHoursWinter);
}
BLYNK_WRITE(V6) {
  manualBoostActivate = param.asInt();
  if (manualBoostActivate == 1) {
    digitalWrite(GPIO_output, HIGH);

    Serial.printf("Manual boost activated\n");

  } else {
    digitalWrite(GPIO_output, LOW);
    Serial.printf("Manual boost deactivated\n");
  }
}


void initTime(String timezone) {
  struct tm timeinfo;
  Serial.println("Setting up time");
  configTime(0, 0, "de.pool.ntp.org");  // First connect to NTP server, with 0 TZ offset (CHANGE TO SUIT COUNTRY)
  if (!getLocalTime(&timeinfo)) {
    Serial.println("  Failed to obtain time");
    return;
  }
  Serial.println("  Got the time from NTP");
  // Now we can set the real timezone
  setTimezone(timezone);
}


void setTimezone(String timezone) {
  Serial.printf("  Setting Timezone to %s\n", timezone.c_str());
  setenv("TZ", timezone.c_str(), 1);  //  Now adjust the TZ.  Clock settings are adjusted to show the new local time
  tzset();
}


void IRAM_ATTR ISR_change() {
  ISR_flag = true;
}


void setup() {
  Serial.begin(115200);
  pinMode(GPIO_input, INPUT);
  pinMode(GPIO_output, OUTPUT);
  digitalWrite(GPIO_output, LOW);
  attachInterrupt(digitalPinToInterrupt(GPIO_input), ISR_change, CHANGE);
  Serial.setDebugOutput(true);
  Blynk.begin(BLYNK_AUTH, WIFI_SSID, WIFI_PASSWORD);
  timer.setInterval(7000L, talleyup);
  timer.setInterval(60000L, checkTrigger);
  timer.setInterval(31234L, checkWifi);
  Blynk.syncAll();
  initTime("NZST-12NZDT-13,M9.4.0/02:00:00,M4.1.0/03:00:00");  // Set for Berlin  New Zealand time is "NZST-12NZDT-13,M9.4.0/02:00:00,M4.1.0/03:00:00"
  printLocalTime();
  Serial.printf("Free Heap Memory: %s Bytes\n", String(ESP.getFreeHeap()).c_str());
  start = time(NULL);  //to compare for uptime calculation
}


void loop() {
if (ISR_flag) {
  if ( digitalRead(GPIO_input) == HIGH ) { 
    //Detected a rising edge
    start_time = micros();
    start_time_backup_ms = millis();
    pulse_number += 1;
  } else {   
      //Detected a falling edge (GPIO_input must be LOW)
      cumulative_us += micros() - start_time;
      cumulative_backup_ms += millis() - start_time_backup_ms;
    }
    ISR_flag = false;
  }
if (alarm_triggered) {
    heat_cylinder();
  }
  Blynk.run();
  timer.run();  // Initiates BlynkTimer
}
