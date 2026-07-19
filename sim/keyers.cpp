#include "keyers.h"

#include <cstddef>

#define len(t) (sizeof(t) / sizeof(*(t)))

// Queue Set: A Set you can shift and pop.
class QSet {
  int arr[MAX_KEYER_QUEUE];
  unsigned int arrlen = 0;

 public:
  int shift() {
    if (arrlen == 0) {
      return -1;
    }
    int ret = arr[0];
    arrlen--;
    for (unsigned int i = 0; i < arrlen; i++) {
      arr[i] = arr[i + 1];
    }
    return ret;
  }

  int pop() {
    if (arrlen == 0) {
      return -1;
    }
    arrlen--;
    return arr[arrlen];
  }

  void add(int val) {
    if (arrlen == MAX_KEYER_QUEUE) {
      return;
    }
    for (unsigned int i = 0; i < arrlen; i++) {
      if (arr[i] == val) {
        return;
      }
    }
    arr[arrlen] = val;
    arrlen++;
  }
};

class StraightKeyer : public Keyer {
 public:
  Transmitter* output = nullptr;
  unsigned int ditDuration = 100;
  // Icom Dot/Dash Ratio 1:1:X（DOT/SPACE は速度のみ、DASH だけ可変）
  double dashRatio = 3.0;
  bool txRelays[2]{};
  int currentTransmittingRelay = -1;

  StraightKeyer() { Reset(); }

  void SetOutput(Transmitter* out) override { output = out; }

  void Reset() override {
    if (output) {
      output->EndTx();
    }
    ditDuration = 100;
    txRelays[0] = txRelays[1] = false;
    currentTransmittingRelay = -1;
  }

  void SetDitDuration(unsigned int duration) override { ditDuration = duration; }

  void SetDashRatio(double ratio) override {
    // Icom: 1:1:2.8 … 1:1:4.5
    if (ratio < 2.8) {
      ratio = 2.8;
    }
    if (ratio > 4.5) {
      ratio = 4.5;
    }
    dashRatio = ratio;
  }

  unsigned int DitMarkMs() const { return ditDuration; }

  unsigned int DahMarkMs() const {
    return static_cast<unsigned int>(ditDuration * dashRatio + 0.5);
  }

  void Release() override { Reset(); }

  bool TxClosed() override {
    for (int i = 0; i < (int)len(txRelays); i++) {
      if (TxClosed(i)) {
        return true;
      }
    }
    return false;
  }

  bool TxClosed(int relay) override { return txRelays[relay]; }

  void Tx(int relay, bool closed) override {
    if (closed) {
      if (currentTransmittingRelay >= 0 && currentTransmittingRelay != relay) {
        const int prev = currentTransmittingRelay;
        txRelays[prev] = false;
        currentTransmittingRelay = -1;
        output->EndTx(prev);
      }
      if (currentTransmittingRelay == relay) {
        return;
      }
      txRelays[relay] = true;
      currentTransmittingRelay = relay;
      output->BeginTx(relay);
      return;
    }

    if (!txRelays[relay]) {
      return;
    }
    txRelays[relay] = false;
    if (currentTransmittingRelay == relay) {
      currentTransmittingRelay = -1;
      output->EndTx(relay);
    }
  }

  void Key(Paddle key, bool pressed) override { Tx(key, pressed); }

  void Tick(unsigned long /*millis*/) override {}
};

class BugKeyer : public StraightKeyer {
 public:
  unsigned long nextPulse = 0;
  bool keyPressed[2]{};

  void Reset() override {
    StraightKeyer::Reset();
    nextPulse = 0;
    keyPressed[0] = false;
    keyPressed[1] = false;
  }

  void Key(Paddle key, bool pressed) override {
    keyPressed[key] = pressed;
    if (key == 0) {
      beginPulsing();
    } else {
      StraightKeyer::Key(key, pressed);
    }
  }

  void Tick(unsigned long millis) override {
    if (nextPulse && (millis >= nextPulse)) {
      pulse(millis);
    }
  }

  void beginPulsing() {
    if (!nextPulse) {
      nextPulse = 1;
    }
  }

  virtual void pulse(unsigned long millis) {
    if (TxClosed(0)) {
      Tx(0, false);
      nextPulse = millis + ditDuration;  // SPACE = 1（Icom: 速度のみ）
    } else if (keyPressed[0]) {
      Tx(0, true);
      nextPulse = millis + DitMarkMs();  // DOT = 1（Icom: 速度のみ）
    } else {
      nextPulse = 0;
      return;
    }
  }
};

class ElBugKeyer : public BugKeyer {
 public:
  unsigned long nextRepeat = static_cast<unsigned long>(-1);
  int currentTransmittingElement = -1;

  void Reset() override {
    BugKeyer::Reset();
    nextRepeat = static_cast<unsigned long>(-1);
    currentTransmittingElement = -1;
  }

  int whichKeyPressed() {
    for (int i = 0; i < (int)len(keyPressed); i++) {
      if (keyPressed[i]) {
        return i;
      }
    }
    return -1;
  }

  void Key(Paddle key, bool pressed) override {
    keyPressed[key] = pressed;
    if (pressed) {
      nextRepeat = key;
      beginPulsing();
    } else {
      nextRepeat = whichKeyPressed();
    }
  }

  unsigned int keyDuration(int key) {
    switch (key) {
      case PADDLE_DIT:
        return DitMarkMs();
      case PADDLE_DAH:
        return DahMarkMs();
    }
    return DitMarkMs();
  }

  virtual int nextTx() {
    if (whichKeyPressed() == -1) {
      return -1;
    }
    return static_cast<int>(nextRepeat);
  }

  void pulse(unsigned long millis) override {
    int nextPulseMs = 0;
    if (currentTransmittingElement >= 0) {
      // 要素間ギャップは常に 1 単位（ウェイトしない）
      nextPulseMs = static_cast<int>(ditDuration);
      Tx(currentTransmittingElement, false);
      currentTransmittingElement = -1;
    } else {
      int next = nextTx();
      if (next >= 0) {
        nextPulseMs = static_cast<int>(keyDuration(next));
        currentTransmittingElement = next;
        Tx(next, true);
      }
    }

    if (nextPulseMs) {
      nextPulse = millis + nextPulseMs;
    } else {
      nextPulse = 0;
    }
  }
};

class UltimaticKeyer : public ElBugKeyer {
 public:
  QSet queue;

  void Key(Paddle key, bool pressed) override {
    if (pressed) {
      queue.add(key);
    }
    ElBugKeyer::Key(key, pressed);
  }

  int nextTx() override {
    int key = queue.shift();
    if (key != -1) {
      return key;
    }
    return ElBugKeyer::nextTx();
  }
};

class SingleDotKeyer : public ElBugKeyer {
 public:
  QSet queue;

  void Key(Paddle key, bool pressed) override {
    if (pressed && (key == PADDLE_DIT)) {
      queue.add(key);
    }
    ElBugKeyer::Key(key, pressed);
  }

  int nextTx() override {
    int key = queue.shift();
    if (key != -1) {
      return key;
    }
    if (keyPressed[1]) {
      return 1;
    }
    if (keyPressed[0]) {
      return 0;
    }
    return -1;
  }
};

class IambicKeyer : public ElBugKeyer {
 public:
  int nextTx() override {
    int next = ElBugKeyer::nextTx();
    if (keyPressed[PADDLE_DIT] && keyPressed[PADDLE_DAH]) {
      nextRepeat = 1 - nextRepeat;
    }
    return next;
  }
};

// Firmware と同じ Iambic A。両パドルを離した後の追加要素は送らない。
class IambicAKeyer : public Keyer {
 public:
  void SetOutput(Transmitter* out) override { output_ = out; }

  void Reset() override {
    if (transmitting_ && output_) output_->EndTx(currentElement_);
    paddles_[PADDLE_DIT] = false;
    paddles_[PADDLE_DAH] = false;
    transmitting_ = false;
    currentElement_ = lastElement_ = firstElement_ = -1;
    phase_ = Phase::Idle;
    deadlineMs_ = 0;
  }

  void SetDitDuration(unsigned int duration) override { ditDuration_ = duration; }

  void SetDashRatio(double ratio) override {
    if (ratio < 2.8) ratio = 2.8;
    if (ratio > 4.5) ratio = 4.5;
    dashRatio_ = ratio;
  }

  void Release() override { Reset(); }
  bool TxClosed() override { return transmitting_; }
  bool TxClosed(int relay) override {
    return transmitting_ && currentElement_ == relay;
  }

  void Tx(int relay, bool closed) override {
    if (closed) {
      if (transmitting_ && currentElement_ == relay) return;
      if (transmitting_ && output_) output_->EndTx(currentElement_);
      currentElement_ = relay;
      transmitting_ = true;
      if (output_) output_->BeginTx(relay);
      return;
    }
    if (!transmitting_ || currentElement_ != relay) return;
    transmitting_ = false;
    if (output_) output_->EndTx(relay);
  }

  void Key(Paddle key, bool pressed) override {
    if (key != PADDLE_DIT && key != PADDLE_DAH) return;
    if (pressed && !paddles_[PADDLE_DIT] && !paddles_[PADDLE_DAH] &&
        phase_ == Phase::Idle) {
      firstElement_ = key;
    }
    paddles_[key] = pressed;
  }

  void Tick(unsigned long now) override {
    if (phase_ == Phase::Idle) {
      const int next = nextElement();
      if (next >= 0) beginElement(next, now);
      return;
    }
    if (static_cast<long>(now - deadlineMs_) < 0) return;
    if (phase_ == Phase::Mark) {
      Tx(currentElement_, false);
      currentElement_ = -1;
      phase_ = Phase::Gap;
      deadlineMs_ = now + ditDuration_;
      return;
    }
    const int next = nextElement();
    if (next < 0) {
      phase_ = Phase::Idle;
      lastElement_ = firstElement_ = -1;
      return;
    }
    beginElement(next, now);
  }

 private:
  enum class Phase { Idle, Mark, Gap };

  int nextElement() {
    const bool dit = paddles_[PADDLE_DIT];
    const bool dah = paddles_[PADDLE_DAH];
    if (dit && dah) {
      return lastElement_ < 0 ? (firstElement_ >= 0 ? firstElement_ : PADDLE_DIT)
                              : 1 - lastElement_;
    }
    if (dit || dah) return dit ? PADDLE_DIT : PADDLE_DAH;
    return -1;
  }

  void beginElement(int element, unsigned long now) {
    currentElement_ = lastElement_ = element;
    firstElement_ = -1;
    Tx(element, true);
    phase_ = Phase::Mark;
    const unsigned duration = element == PADDLE_DAH
                                  ? static_cast<unsigned>(ditDuration_ * dashRatio_ + 0.5)
                                  : ditDuration_;
    deadlineMs_ = now + duration;
  }

  Transmitter* output_ = nullptr;
  unsigned int ditDuration_ = 100;
  double dashRatio_ = 3.0;
  bool paddles_[2]{};
  bool transmitting_ = false;
  int currentElement_ = -1;
  int lastElement_ = -1;
  int firstElement_ = -1;
  Phase phase_ = Phase::Idle;
  unsigned long deadlineMs_ = 0;
};

// Iambic B。スクイーズを離した後に反対側の要素を 1 つ追加する。
class IambicBKeyer : public Keyer {
 public:
  void SetOutput(Transmitter* out) override { output_ = out; }

  void Reset() override {
    if (transmitting_ && output_) {
      output_->EndTx(currentElement_);
    }
    paddles_[PADDLE_DIT] = false;
    paddles_[PADDLE_DAH] = false;
    transmitting_ = false;
    currentElement_ = -1;
    lastElement_ = -1;
    firstElement_ = -1;
    squeezeSeen_ = false;
    phase_ = Phase::Idle;
    deadlineMs_ = 0;
  }

  void SetDitDuration(unsigned int duration) override { ditDuration_ = duration; }

  void SetDashRatio(double ratio) override {
    if (ratio < 2.8) ratio = 2.8;
    if (ratio > 4.5) ratio = 4.5;
    dashRatio_ = ratio;
  }

  void Release() override { Reset(); }
  bool TxClosed() override { return transmitting_; }
  bool TxClosed(int relay) override {
    return transmitting_ && currentElement_ == relay;
  }

  void Tx(int relay, bool closed) override {
    if (closed) {
      if (transmitting_ && currentElement_ == relay) return;
      if (transmitting_ && output_) output_->EndTx(currentElement_);
      currentElement_ = relay;
      transmitting_ = true;
      if (output_) output_->BeginTx(relay);
      return;
    }
    if (!transmitting_ || currentElement_ != relay) return;
    transmitting_ = false;
    if (output_) output_->EndTx(relay);
  }

  void Key(Paddle key, bool pressed) override {
    if (key != PADDLE_DIT && key != PADDLE_DAH) return;
    if (pressed && !paddles_[PADDLE_DIT] && !paddles_[PADDLE_DAH] &&
        phase_ == Phase::Idle) {
      firstElement_ = key;
    }
    paddles_[key] = pressed;
    if (paddles_[PADDLE_DIT] && paddles_[PADDLE_DAH]) squeezeSeen_ = true;
  }

  void Tick(unsigned long now) override {
    if (phase_ == Phase::Idle) {
      const int next = nextElement();
      if (next >= 0) beginElement(next, now);
      return;
    }
    if (static_cast<long>(now - deadlineMs_) < 0) return;
    if (phase_ == Phase::Mark) {
      Tx(currentElement_, false);
      currentElement_ = -1;
      phase_ = Phase::Gap;
      deadlineMs_ = now + ditDuration_;
      return;
    }
    const int next = nextElement();
    if (next < 0) {
      phase_ = Phase::Idle;
      lastElement_ = -1;
      firstElement_ = -1;
      return;
    }
    beginElement(next, now);
  }

 private:
  enum class Phase { Idle, Mark, Gap };

  unsigned int elementDuration(int element) const {
    return element == PADDLE_DAH
               ? static_cast<unsigned int>(ditDuration_ * dashRatio_ + 0.5)
               : ditDuration_;
  }

  int nextElement() {
    const bool dit = paddles_[PADDLE_DIT];
    const bool dah = paddles_[PADDLE_DAH];
    if (dit && dah) {
      squeezeSeen_ = true;
      if (lastElement_ < 0) {
        const int first = firstElement_;
        firstElement_ = -1;
        return first >= 0 ? first : PADDLE_DIT;
      }
      return 1 - lastElement_;
    }
    if (dit || dah) {
      squeezeSeen_ = false;
      firstElement_ = -1;
      return dit ? PADDLE_DIT : PADDLE_DAH;
    }
    if (squeezeSeen_ && lastElement_ >= 0) {
      squeezeSeen_ = false;
      return 1 - lastElement_;
    }
    return -1;
  }

  void beginElement(int element, unsigned long now) {
    currentElement_ = element;
    lastElement_ = element;
    firstElement_ = -1;
    Tx(element, true);
    phase_ = Phase::Mark;
    deadlineMs_ = now + elementDuration(element);
  }

  Transmitter* output_ = nullptr;
  unsigned int ditDuration_ = 100;
  double dashRatio_ = 3.0;
  bool paddles_[2]{};
  bool transmitting_ = false;
  int currentElement_ = -1;
  int lastElement_ = -1;
  int firstElement_ = -1;
  bool squeezeSeen_ = false;
  Phase phase_ = Phase::Idle;
  unsigned long deadlineMs_ = 0;
};

class KeyaheadKeyer : public ElBugKeyer {
 public:
  int queue[MAX_KEYER_QUEUE]{};
  unsigned int qlen = 0;

  void Reset() override {
    ElBugKeyer::Reset();
    qlen = 0;
  }

  void Key(Paddle key, bool pressed) override {
    if (pressed) {
      if (qlen < MAX_KEYER_QUEUE) {
        queue[qlen++] = key;
      }
    }
    ElBugKeyer::Key(key, pressed);
  }

  int nextTx() override {
    if (qlen > 0) {
      int next = queue[0];
      qlen--;
      for (unsigned int i = 0; i < qlen; i++) {
        queue[i] = queue[i + 1];
      }
      return next;
    }
    return ElBugKeyer::nextTx();
  }
};

static StraightKeyer straightKeyer;
static BugKeyer bugKeyer;
static ElBugKeyer elBugKeyer;
static SingleDotKeyer singleDotKeyer;
static UltimaticKeyer ultimaticKeyer;
static IambicKeyer iambicKeyer;
static IambicAKeyer iambicAKeyer;
static IambicBKeyer iambicBKeyer;
static KeyaheadKeyer keyaheadKeyer;

static Keyer* keyers[] = {
    nullptr,
    &straightKeyer,
    &bugKeyer,
    &elBugKeyer,
    &singleDotKeyer,
    &ultimaticKeyer,
    &iambicKeyer,
    &iambicAKeyer,
    &iambicBKeyer,
    &keyaheadKeyer,
};

Keyer* GetKeyerByNumber(int n, Transmitter* output) {
  if (n < 1 || n >= (int)len(keyers)) {
    return nullptr;
  }
  Keyer* k = keyers[n];
  k->SetOutput(output);
  return k;
}

int getKeyerNumber(Keyer* k) {
  if (k == nullptr) {
    return 1;
  }
  for (int i = 1; i < (int)len(keyers); i++) {
    if (keyers[i] == k) {
      return i;
    }
  }
  return 1;
}
