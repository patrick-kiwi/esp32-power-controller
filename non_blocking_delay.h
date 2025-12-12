/*
 * Non-Blocking Delay Class
 * 
 * Provides timing without blocking execution.
 * Compatible with both Arduino and ESP32.
 */

#ifndef NON_BLOCKING_DELAY_H
#define NON_BLOCKING_DELAY_H

#include <Arduino.h>

class nblock_delay {
public:
  /*
   * Constructor
   * @param interval_ms  Duration in milliseconds
   */
  nblock_delay(unsigned long interval_ms)
    : interval_(interval_ms),
      lastTrigger_(millis()) {}

  bool elapsed() {
    if (millis() - lastTrigger_ >= interval_) {
      lastTrigger_ = millis();
      return true;
    }
    return false;
  }
  
  /*
   * Alias for elapsed() - more intuitive for event-style usage.
   * Returns true when it's time to trigger.
   */
  bool trigger() {
    return elapsed();
  }
  
  /*
   * Check if still waiting (interval has NOT elapsed).
   * Does NOT reset the timer when interval elapses.
   * 
   * @return true   if still waiting
   * @return false  if interval has elapsed
   * 
   * Usage example - wait for a period then continue:
   *   nblock_delay timeout(5000);
   *   while (timeout.waiting()) {
   *     // do work while waiting
   *   }
   *   // 5 seconds have passed
   */
  bool waiting() {
    return (millis() - lastTrigger_) < interval_;
  }
  
  /*
   * Reset the timer to start counting from now.
   */
  void reset() {
    lastTrigger_ = millis();
  }
  
  /*
   * Change the interval duration.
   * Does not reset the timer.
   */
  void setInterval(unsigned long interval_ms) {
    interval_ = interval_ms;
  }
  
  /*
   * Get remaining time until next trigger.
   * @return milliseconds remaining (0 if already elapsed)
   */
  unsigned long remaining() {
    unsigned long elapsed = millis() - lastTrigger_;
    if (elapsed >= interval_) return 0;
    return interval_ - elapsed;
  }

private:
  unsigned long interval_;
  unsigned long lastTrigger_;
};

#endif // NON_BLOCKING_DELAY_H
