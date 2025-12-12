/*
 * Configuration file for ESP32 Hot Water Controller
 * 
 * IMPORTANT: Add this file to .gitignore to keep credentials private!
 * 
 * Copy this file to config.h and fill in your actual values.
 */

#ifndef CONFIG_H
#define CONFIG_H

// Blynk configuration
// Get these from the Blynk console at https://blynk.cloud
#define BLYNK_TEMPLATE_ID   "your_template_id"
#define BLYNK_TEMPLATE_NAME "ESP32 Power Diverter"
#define BLYNK_AUTH          "your_blynk_auth_token"

// WiFi credentials
#define WIFI_SSID           "your_wifi_ssid"
#define WIFI_PASSWORD       "your_wifi_password"

#endif // CONFIG_H
