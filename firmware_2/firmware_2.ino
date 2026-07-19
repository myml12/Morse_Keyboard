#include <Arduino.h>
#include <BleKeyboard.h>
#include <cstring>
#include <driver/gpio.h>

// ESP32-C3 Morse keyboard (_2)
// 離したら余分な要素を出さない Iambic A: Dit=1、要素間=1、Dah=設定比。
// ブザーは他励式（パッシブ）→ LEDC でサイドトーンを出す。
//
// GATT_INSUF_ENCRYPTION が出て文字送信が遅延／不安定なとき:
// ライブラリ BleKeyboard.cpp の
//   setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND)
// を
//   setAuthenticationMode(ESP_LE_AUTH_BOND)
// に変更し、ホスト側で一度ペアリングを削除してから再ペアする。
// （C3/S3 でよくある問題。CPU 性能不足ではない。）

constexpr uint8_t DOT_PIN = D0;      // 短点
constexpr uint8_t DASH_PIN = D1;     // 長点
constexpr uint8_t BUZZER_PIN = D8;   // 他励式ブザー
constexpr uint8_t BUZZER_LEDC_CH = 0;
constexpr uint16_t SIDETONE_HZ = 700;
constexpr bool SWAP_PADDLES = false;
constexpr uint8_t WPM = 22;
constexpr float DASH_RATIO = 3.0f;  // Icom の 1:1:X、X は 2.8〜4.5
constexpr uint8_t CONTACT_DEBOUNCE_MS = 20;
constexpr uint8_t RELEASE_REARM_MS = 20;

// BleKeyboard の名前は最大15文字程度。区別用に _2 を付ける。
BleKeyboard bleKeyboard("XIAO Morse_2", "Y.Mizuno", 100);

uint32_t dotMs() { return 1200UL / WPM; }

void writeKeyboard(uint8_t character) {
  if (!bleKeyboard.isConnected()) return;
  bleKeyboard.write(character);
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
#define BUZZER_LEDC_ID BUZZER_PIN
#else
#define BUZZER_LEDC_ID BUZZER_LEDC_CH
#endif

void initBuzzer() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  ledcAttach(BUZZER_PIN, SIDETONE_HZ, 8);
#else
  ledcSetup(BUZZER_LEDC_CH, SIDETONE_HZ, 8);
  ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CH);
#endif
  ledcWriteTone(BUZZER_LEDC_ID, 0);
}

void buzzerOn() { ledcWriteTone(BUZZER_LEDC_ID, SIDETONE_HZ); }

void buzzerOff() { ledcWriteTone(BUZZER_LEDC_ID, 0); }

struct MorseEntry {
  const char* code;
  char character;
};

const MorseEntry MORSE_TABLE[] = {
    {".-", 'A'},    {"-...", 'B'},  {"-.-.", 'C'},  {"-..", 'D'},
    {".", 'E'},     {"..-.", 'F'},  {"--.", 'G'},   {"....", 'H'},
    {"..", 'I'},    {".---", 'J'},  {"-.-", 'K'},   {".-..", 'L'},
    {"--", 'M'},    {"-.", 'N'},    {"---", 'O'},   {".--.", 'P'},
    {"--.-", 'Q'},  {".-.", 'R'},   {"...", 'S'},   {"-", 'T'},
    {"..-", 'U'},   {"...-", 'V'},  {".--", 'W'},   {"-..-", 'X'},
    {"-.--", 'Y'},  {"--..", 'Z'},  {"-----", '0'}, {".----", '1'},
    {"..---", '2'}, {"...--", '3'}, {"....-", '4'}, {".....", '5'},
    {"-....", '6'}, {"--...", '7'}, {"---..", '8'}, {"----.", '9'},
    {".-.-.-", '.'}, {"--..--", ','}, {"..--..", '?'}, {"-.-.--", '!'},
    {"-..-.", '/'}, {"-.--.", '('}, {"-.--.-", ')'}, {".-...", '&'},
    {"---...", ':'}, {"-.-.-.", ';'}, {"-...-", '='}, {".-.-.", '+'},
    {"-....-", '-'}, {"..--.-", '_'}, {".-..-.", '"'}, {"...-..-", '$'},
    {".--.-.", '@'},
};

class MorseDecoder {
 public:
  void flush(uint32_t now) {
    const uint32_t silence = now - lastElementEndMs_;
    if (characterPending_ && silence >= dotMs() * 3UL) {
      char decoded = '\0';
      for (const auto& entry : MORSE_TABLE) {
        if (strcmp(marks_, entry.code) == 0) {
          decoded = entry.character;
          break;
        }
      }
      if (decoded != '\0') writeKeyboard(static_cast<uint8_t>(decoded));
      marksLength_ = 0;
      marks_[0] = '\0';
      characterPending_ = false;
      characterSent_ = true;
    }
    if (!characterPending_ && characterSent_ && !spaceSent_ &&
        silence >= dotMs() * 7UL) {
      writeKeyboard(' ');
      spaceSent_ = true;
    }
  }

  void addElement(bool dah, uint32_t now) {
    if (marksLength_ < sizeof(marks_) - 1) {
      marks_[marksLength_++] = dah ? '-' : '.';
      marks_[marksLength_] = '\0';
    }
    characterPending_ = true;
    spaceSent_ = false;
    lastElementEndMs_ = now;
  }

 private:
  char marks_[8]{};
  uint8_t marksLength_ = 0;
  bool characterPending_ = false;
  bool characterSent_ = false;
  bool spaceSent_ = true;
  uint32_t lastElementEndMs_ = 0;
};

MorseDecoder decoder;

class Sidetone {
 public:
  void begin(bool /*dah*/, uint32_t now) {
    decoder.flush(now);
    active_ = true;
    buzzerOn();
  }

  void end(bool dah, uint32_t now) {
    if (!active_) return;
    active_ = false;
    buzzerOff();
    decoder.addElement(dah, now);
  }

 private:
  bool active_ = false;
};

Sidetone sidetone;

class IambicKeyer {
 public:
  void setPaddle(uint8_t element, bool pressed) {
    if (pressed && !dit_ && !dah_ && phase_ == Phase::Idle) first_ = element;
    if (element == 0) {
      dit_ = pressed;
    } else {
      dah_ = pressed;
    }
  }

  void tick(uint32_t now) {
    if (phase_ == Phase::Idle) {
      const int next = nextElement();
      if (next >= 0) start(next, now);
      return;
    }
    if (static_cast<int32_t>(now - deadlineMs_) < 0) return;

    if (phase_ == Phase::Mark) {
      sidetone.end(current_ == 1, now);
      current_ = -1;
      phase_ = Phase::Gap;
      deadlineMs_ = now + dotMs();
      return;
    }

    const int next = nextElement();
    if (next < 0) {
      phase_ = Phase::Idle;
      last_ = first_ = -1;
      return;
    }
    start(next, now);
  }

  bool sending() const { return phase_ == Phase::Mark; }

 private:
  enum class Phase : uint8_t { Idle, Mark, Gap };

  int nextElement() {
    if (dit_ && dah_) {
      if (last_ < 0) return first_ >= 0 ? first_ : 0;
      return 1 - last_;
    }
    if (dit_ || dah_) {
      return dit_ ? 0 : 1;
    }
    return -1;
  }

  void start(int element, uint32_t now) {
    current_ = last_ = element;
    first_ = -1;
    sidetone.begin(element == 1, now);
    phase_ = Phase::Mark;
    const uint32_t duration =
        element == 1 ? static_cast<uint32_t>(dotMs() * DASH_RATIO + 0.5f)
                     : dotMs();
    deadlineMs_ = now + duration;
  }

  bool dit_ = false;
  bool dah_ = false;
  int current_ = -1;
  int last_ = -1;
  int first_ = -1;
  Phase phase_ = Phase::Idle;
  uint32_t deadlineMs_ = 0;
};

IambicKeyer keyer;

class PaddleInput {
 public:
  bool update(bool rawPressed, uint32_t now) {
    if (!rawPressed) {
      if (candidate_) releasedSinceMs_ = now;
      candidate_ = false;
      if (pressed_) {
        pressed_ = false;
        return true;
      }
      return false;
    }
    if (!candidate_) {
      candidate_ = true;
      pressedSinceMs_ = now;
    }
    if (!pressed_ && now - pressedSinceMs_ >= CONTACT_DEBOUNCE_MS &&
        now - releasedSinceMs_ >= RELEASE_REARM_MS) {
      pressed_ = true;
      return true;
    }
    return false;
  }

  bool pressed() const { return pressed_; }

 private:
  bool candidate_ = false;
  bool pressed_ = false;
  uint32_t pressedSinceMs_ = 0;
  uint32_t releasedSinceMs_ = 0;
};

PaddleInput ditInput;
PaddleInput dahInput;

void configureInput(uint8_t pin) {
  gpio_reset_pin(static_cast<gpio_num_t>(pin));
  pinMode(pin, INPUT_PULLUP);
}

void updatePaddles(uint32_t now) {
  const bool dotPinPressed = digitalRead(DOT_PIN) == LOW;
  const bool dashPinPressed = digitalRead(DASH_PIN) == LOW;
  const bool ditRaw = SWAP_PADDLES ? dashPinPressed : dotPinPressed;
  const bool dahRaw = SWAP_PADDLES ? dotPinPressed : dashPinPressed;

  if (ditInput.update(ditRaw, now)) keyer.setPaddle(0, ditInput.pressed());
  if (dahInput.update(dahRaw, now)) keyer.setPaddle(1, dahInput.pressed());
}

void setup() {
  configureInput(DOT_PIN);
  configureInput(DASH_PIN);
  initBuzzer();
  // 既定 8ms/キーは遅めに感じることがある。モールス1文字送信なら短くてよい。
  bleKeyboard.setDelay(2);
  bleKeyboard.begin();
}

void loop() {
  const uint32_t now = millis();
  updatePaddles(now);
  keyer.tick(now);
  if (!keyer.sending()) decoder.flush(now);
  delay(1);
}
