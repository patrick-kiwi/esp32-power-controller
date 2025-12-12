#ifndef NON_BLOCKING_DELAY
#define NON_BLOCKING_DELAY

class nblock_delay {
public:
  nblock_delay(const unsigned long interval) {
    this->interval = interval;
    this->last_seen = millis();
  }
  bool trigger() {
    if (millis() - this->last_seen > this->interval) {
      this->last_seen = millis();
      return true;
    } else {
      return false;
    }
  }
  ~nblock_delay() {}  //destructor
private:
  unsigned long interval;
  unsigned long last_seen;
};

#endif