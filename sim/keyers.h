#pragma once

// Desktop keyer API. 既定の Iambic A は ESP32 firmware と同じ動作。

#define MAX_KEYER_QUEUE 5

typedef enum {
  PADDLE_DIT = 0,
  PADDLE_DAH = 1,
  PADDLE_STRAIGHT,
} Paddle;

class Transmitter {
 public:
  virtual ~Transmitter() = default;
  virtual void BeginTx() {}
  virtual void EndTx() {}
  virtual void BeginTx(int /*relay*/) {}
  virtual void EndTx(int /*relay*/) {}
};

class Keyer {
 public:
  virtual ~Keyer() = default;
  virtual void SetOutput(Transmitter* output) = 0;
  virtual void Reset() = 0;
  virtual void SetDitDuration(unsigned int d) = 0;
  // Icom 互換: Dot/Dash Ratio の第3項（1:1:X の X）。既定 3.0
  virtual void SetDashRatio(double /*ratio*/) {}
  virtual void Release() = 0;
  virtual bool TxClosed() = 0;
  virtual bool TxClosed(int relay) = 0;
  virtual void Tx(int relay, bool closed) = 0;
  virtual void Key(Paddle key, bool pressed) = 0;
  virtual void Tick(unsigned long millis) = 0;
};

Keyer* GetKeyerByNumber(int n, Transmitter* output);
int getKeyerNumber(Keyer* k);

inline const char* KeyerTypeName(int n) {
  switch (n) {
    case 1:
      return "Straight";
    case 2:
      return "Bug";
    case 3:
      return "ElBug";
    case 4:
      return "SingleDot";
    case 5:
      return "Ultimatic";
    case 6:
      return "Plain Iambic";
    case 7:
      return "Iambic A";
    case 8:
      return "Iambic B";
    case 9:
      return "Keyahead";
    default:
      return "Unknown";
  }
}
