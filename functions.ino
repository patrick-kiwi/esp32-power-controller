void startWifi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("Connecting Wifi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.print("Wifi RSSI=");
  Serial.println(WiFi.RSSI());
}

void talleyup() {
  if ((digitalRead(GPIO_input) == LOW)) {
    /*
    in between cycles sum up the on-time, and
    protect against buffer overflow on the microsecond
    counter which happens after 71.58 minutes 
    */
    if (cumulative_backup_ms > 4200000L) {    //ie. > 70 min
      cumulative_ms += cumulative_backup_ms;  //switch from us -> ms
    } else {
      cumulative_ms += static_cast<unsigned long int>(static_cast<float>(cumulative_us) / 1000.0);  //us -> ms
    }
    cumulative_us = 0L;
    cumulative_backup_ms = 0L;
    cumulative_ontime_h = static_cast<float>(cumulative_ms) / 3600000.0;  //ms -> hours
    Blynk.virtualWrite(V7, cumulative_ontime_h);
    Blynk.virtualWrite(V10, pulse_number);
  }
}


void checkTrigger() {
  //send the server the uptime
  end = time(NULL);
  u_time = static_cast<unsigned long>(difftime(end, start));
  uday = ulong(double(u_time) / double(86400));
  u_time = u_time % 86400L;
  uhour = ulong(double(u_time) / double(3600));
  u_time = u_time % 3600L;
  umin = ulong(double(u_time) / double(60L));
  sprintf(uptime_string, "%d days, %d hours, %d min", uday, uhour, umin);
  Blynk.virtualWrite(V5, uptime_string);

  //check for alarm every min
  //make sure we have the latest values for hour and minute triggers
  Serial.printf("cumulative_ontime is %.3f\n", cumulative_ontime_h);
  gettimeofday(&current_time, NULL);
  char buff_hours[4], buff_min[4], buff_sec[4], buff_day[4];
  strftime(buff_hours, sizeof buff_hours, "%H", localtime(&(current_time.tv_sec)));
  strftime(buff_min, sizeof buff_min, "%M", localtime(&(current_time.tv_sec)));
  //strftime(buff_sec, sizeof buff_sec, "%S", localtime(&(current_time.tv_sec)));
  strftime(buff_day, sizeof buff_day, "%j", localtime(&(current_time.tv_sec)));
  hour = atoi(buff_hours);
  minute = atoi(buff_min);
  day = atoi(buff_day);
  printf("Day: %d, Hour: %d, Minute: %d *** Alarm Hour %d Alarm Minute %d\n", day, hour, minute, hour_alarm_on, minute_alarm_on);

  //Reactivate the alarm during the hour before it's set to go off
  int retrigger_time = (hour_alarm_on == 0) ? 23 : hour_alarm_on - 1;
  if (hour == retrigger_time) {
    alarm_triggered = false;  //ie alarm hasn't yet been triggered and is ready to go
    first_run = true;         //ie this is the first run allow code block to run
  }

  //Actions to take when HWC once per day check is triggered
  if (hour == hour_alarm_on && minute >= minute_alarm_on && alarm_triggered == false) {
    alarm_triggered = true;  //run block once per evening
    //check that the required winter hours are greater than the required summer hours
    float winterH = (ReqHoursWinter >= ReqHoursSummer) ? ReqHoursWinter : ReqHoursSummer;
    float summerH = (ReqHoursSummer <= ReqHoursWinter) ? ReqHoursSummer : ReqHoursWinter;
    Serial.print("Once per day check event triggered at: ");
    printLocalTime();
    // desired day_offset ranges from 0-364, where day 0 is Winter solstice (21st June or 171th day of the year)
    // Accuweather day is defined as 1-365, where day 1 is the 1st January
    int offset_day = (day - 171) % 364 + 1;
    float f = 0.5 * (winterH - summerH);
    required_hours = f * cos(0.017214 * static_cast<float>(offset_day)) + f + summerH;
    Serial.printf("Days is: %d and offset day is %d\n", day, offset_day);
    Serial.printf("Summer heating hours required: %.3f h\n", summerH);
    Serial.printf("Winter heating hours required: %.3f h\n", winterH);
    Serial.printf("Todays heating hours required: %.3f h\n", required_hours);  //cosine function with winter set as 0
    //calculate the shortfall in ms (note signed int 2^32/2-1 can handle 24.8 days of ms)
    shortfall = required_hours - cumulative_ontime_h;
    Blynk.virtualWrite(V11, shortfall);
    Serial.printf("Total ontime of hot water cylinder today has been: %.2f h\n", cumulative_ontime_h);
    
  }
}

void heat_cylinder() {
  if (first_run) {  //Ensure this block of code runs only once per trigger cycle

      //calculate boost required
    if (shortfall <= 0) {
      Serial.printf("Required %.2f h of heating. Received %.2f h. No action taken!\n", required_hours, cumulative_ontime_h);
    } else {
      nblock_delay boost_ms(static_cast<unsigned long int>(shortfall) * 3600000L);  //convert h -> ms
      Serial.printf("Boost will be applied for %.3f h\n", shortfall);
      nblock_delay printing_dots(60000);
      while (!boost_ms.trigger()) {
        digitalWrite(GPIO_output, HIGH);
        if (printing_dots.trigger()) { Serial.printf("."); }
      }
      digitalWrite(GPIO_output, LOW);
      shortfall = 0.0;
      Serial.printf("\nDone! Hot water cylinder successfully recharged\n");
    }

    cumulative_ms = 0;
    cumulative_ontime_h = 0.0;
    pulse_number = 0;
    //Mark the time point when the alarm was successfully completed
    struct timeval tv;
    time_t t;
    struct tm *info;
    gettimeofday(&tv, NULL);
    t = tv.tv_sec;
    info = localtime(&t);
    strftime(last_triggered_string, sizeof last_triggered_string, "%A, %B %d, %I:%M %p.\n", info);
    printf("%s\n", last_triggered_string);
    
    //finally adjust Blynk values
    checkWifi();   /* Added After long term testing began */
    Blynk.virtualWrite(V7, cumulative_ontime_h);
    Blynk.virtualWrite(V10, pulse_number);
    Blynk.virtualWrite(V8, last_triggered_string);

    //end of heating cycle make sure this code only runs once
    first_run=false;   
}
}


void checkWifi() {
  if ((WiFi.status() != WL_CONNECTED)) {
    WiFi.disconnect();
    WiFi.reconnect();
    Serial.printf("****  Wifi was reconnected  ****\n");
  }
}


void printLocalTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time 1");
    return;
  }
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S zone %Z %z ");
}
