#include <Arduino.h>
#include <BleKeyboard.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include <cstring>
#include <driver/gpio.h>

// XIAO ESP32-C3 Morse keyboard with OLED and rotary controls.
// OLED: SSD1306 128x64 I2C (SDA=D4, SCL=D5), address 0x3C.

constexpr uint8_t DOT_PIN = D0;
constexpr uint8_t DASH_PIN = D1;
constexpr uint8_t BUZZER_PIN = D7;
constexpr uint8_t SELECT_BUTTON_PIN = D8;
constexpr uint8_t ENCODER_A_PIN = D2;
constexpr uint8_t ENCODER_B_PIN = D3;
constexpr uint8_t OLED_SDA_PIN = D4;
constexpr uint8_t OLED_SCL_PIN = D5;
constexpr uint8_t OLED_ADDRESS = 0x3C;

constexpr uint8_t BUZZER_LEDC_CH = 0;
constexpr uint8_t BUZZER_LEDC_RESOLUTION = 10;
constexpr uint16_t BUZZER_LEDC_MAX_DUTY = (1U << BUZZER_LEDC_RESOLUTION) - 1;
constexpr uint16_t SIDETONE_HZ = 3000;

constexpr bool SWAP_PADDLES = false;
constexpr bool REVERSE_ENCODER = false;
constexpr uint8_t MIN_WPM = 5;
constexpr uint8_t MAX_WPM = 50;
constexpr float DASH_RATIO = 3.0f;
constexpr uint8_t MIN_VOLUME = 0;
constexpr uint8_t MAX_VOLUME = 10;
constexpr uint8_t CONTACT_DEBOUNCE_MS = 20;
constexpr uint8_t RELEASE_REARM_MS = 20;
constexpr uint8_t CHARACTER_GAP_DOTS = 2;
constexpr uint8_t BLE_TX_QUEUE_SIZE = 64;
constexpr uint16_t BLE_CONNECT_SETTLE_MS = 800;
constexpr uint16_t BLE_KEY_DOWN_MS = 20;
constexpr uint16_t BLE_KEY_GAP_MS = 20;

uint8_t wpm = 22;
uint8_t volumeLevel = 1;  // 0 (mute) - 10 (maximum PWM duty)

BleKeyboard bleKeyboard("Morse_Keyboard_beta3", "myml12", 100);
Adafruit_SSD1306 display(128, 64, &Wire, -1);
bool displayReady = false;
bool displayDirty = true;

// OLED最下段に表示する、復号済み文字の横スクロール履歴。
constexpr uint8_t DISPLAY_HISTORY_CHARS = 10;
char currentMorseDisplay[8]{};
char decodedHistory[DISPLAY_HISTORY_CHARS + 1]{};
uint8_t decodedHistoryLength = 0;

void setCurrentMorseDisplay(const char* marks) {
  strncpy(currentMorseDisplay, marks, sizeof(currentMorseDisplay) - 1);
  currentMorseDisplay[sizeof(currentMorseDisplay) - 1] = '\0';
  displayDirty = true;
}

void appendDecodedDisplay(char character) {
  if (decodedHistoryLength < DISPLAY_HISTORY_CHARS) {
    decodedHistory[decodedHistoryLength++] = character;
  } else {
    memmove(decodedHistory, decodedHistory + 1, DISPLAY_HISTORY_CHARS - 1);
    decodedHistory[DISPLAY_HISTORY_CHARS - 1] = character;
  }
  decodedHistory[decodedHistoryLength] = '\0';
  displayDirty = true;
}

uint32_t dotMs() { return 1200UL / wpm; }

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
#define BUZZER_LEDC_ID BUZZER_PIN
#else
#define BUZZER_LEDC_ID BUZZER_LEDC_CH
#endif

uint16_t buzzerDuty() {
  return static_cast<uint16_t>(BUZZER_LEDC_MAX_DUTY * volumeLevel / MAX_VOLUME);
}

void initBuzzer() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  ledcAttach(BUZZER_PIN, SIDETONE_HZ, BUZZER_LEDC_RESOLUTION);
#else
  ledcSetup(BUZZER_LEDC_CH, SIDETONE_HZ, BUZZER_LEDC_RESOLUTION);
  ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CH);
#endif
  ledcWrite(BUZZER_LEDC_ID, 0);
}

void buzzerOn() {
  if (volumeLevel == 0) return;
  ledcWriteTone(BUZZER_LEDC_ID, SIDETONE_HZ);
  ledcWrite(BUZZER_LEDC_ID, buzzerDuty());
}

void buzzerOff() { ledcWrite(BUZZER_LEDC_ID, 0); }

class BleTextSender {
 public:
  void enqueue(uint8_t character) {
    if (!bleKeyboard.isConnected() || count_ == BLE_TX_QUEUE_SIZE) return;
    queue_[tail_] = character;
    tail_ = (tail_ + 1) % BLE_TX_QUEUE_SIZE;
    ++count_;
  }

  void tick(uint32_t now) {
    const bool connected = bleKeyboard.isConnected();
    if (!connected) {
      connected_ = false;
      clear();
      state_ = State::Idle;
      return;
    }
    if (!connected_) {
      connected_ = true;
      readyAtMs_ = now + BLE_CONNECT_SETTLE_MS;
      state_ = State::Idle;
    }
    if (static_cast<int32_t>(now - readyAtMs_) < 0 ||
        static_cast<int32_t>(now - deadlineMs_) < 0) {
      return;
    }
    if (state_ == State::KeyDown) {
      bleKeyboard.releaseAll();
      state_ = State::Idle;
      deadlineMs_ = now + BLE_KEY_GAP_MS;
      return;
    }
    if (count_ == 0) return;
    const uint8_t character = queue_[head_];
    head_ = (head_ + 1) % BLE_TX_QUEUE_SIZE;
    --count_;
    bleKeyboard.press(character);
    state_ = State::KeyDown;
    deadlineMs_ = now + BLE_KEY_DOWN_MS;
  }

 private:
  enum class State : uint8_t { Idle, KeyDown };
  void clear() { head_ = tail_ = count_ = 0; }

  uint8_t queue_[BLE_TX_QUEUE_SIZE]{};
  uint8_t head_ = 0;
  uint8_t tail_ = 0;
  uint8_t count_ = 0;
  bool connected_ = false;
  State state_ = State::Idle;
  uint32_t readyAtMs_ = 0;
  uint32_t deadlineMs_ = 0;
};

BleTextSender bleTextSender;
void writeKeyboard(uint8_t character) { bleTextSender.enqueue(character); }

struct MorseEntry {
  const char* code;
  char character;
};

const MorseEntry MORSE_TABLE[] = {
    {".-", 'A'}, {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'},
    {".", 'E'}, {"..-.", 'F'}, {"--.", 'G'}, {"....", 'H'},
    {"..", 'I'}, {".---", 'J'}, {"-.-", 'K'}, {".-..", 'L'},
    {"--", 'M'}, {"-.", 'N'}, {"---", 'O'}, {".--.", 'P'},
    {"--.-", 'Q'}, {".-.", 'R'}, {"...", 'S'}, {"-", 'T'},
    {"..-", 'U'}, {"...-", 'V'}, {".--", 'W'}, {"-..-", 'X'},
    {"-.--", 'Y'}, {"--..", 'Z'}, {"-----", '0'}, {".----", '1'},
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
    if (characterPending_ && silence >= dotMs() * CHARACTER_GAP_DOTS) {
      char decoded = '\0';
      for (const auto& entry : MORSE_TABLE) {
        if (strcmp(marks_, entry.code) == 0) {
          decoded = entry.character;
          break;
        }
      }
      // BLE未接続でも表示上の復号は続ける。
      appendDecodedDisplay(decoded != '\0' ? decoded : '?');
      if (decoded != '\0') writeKeyboard(static_cast<uint8_t>(decoded));
      marksLength_ = 0;
      marks_[0] = '\0';
      setCurrentMorseDisplay(marks_);
      characterPending_ = false;
      characterSent_ = true;
    }
    if (!characterPending_ && characterSent_ && !spaceSent_ &&
        silence >= dotMs() * 7UL) {
      appendDecodedDisplay(' ');
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
    setCurrentMorseDisplay(marks_);
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
    // A short press during the current mark/gap is retained for one next mark.
    if (pressed && phase_ != Phase::Idle) {
      if (element == 0) ditPending_ = true;
      else dahPending_ = true;
    }
    if (element == 0) dit_ = pressed;
    else dah_ = pressed;
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
      ditPending_ = dahPending_ = false;
      return;
    }
    start(next, now);
  }

  bool sending() const { return phase_ == Phase::Mark; }

 private:
  enum class Phase : uint8_t { Idle, Mark, Gap };

  int nextElement() {
    const bool ditAvailable = dit_ || ditPending_;
    const bool dahAvailable = dah_ || dahPending_;
    if (ditAvailable && dahAvailable) {
      if (last_ < 0) return first_ >= 0 ? first_ : 0;
      return 1 - last_;
    }
    if (ditAvailable || dahAvailable) return ditAvailable ? 0 : 1;
    return -1;
  }

  void start(int element, uint32_t now) {
    current_ = last_ = element;
    first_ = -1;
    if (element == 0) ditPending_ = false;
    else dahPending_ = false;
    sidetone.begin(element == 1, now);
    phase_ = Phase::Mark;
    const uint32_t duration =
        element == 1 ? static_cast<uint32_t>(dotMs() * DASH_RATIO + 0.5f)
                     : dotMs();
    deadlineMs_ = now + duration;
  }

  bool dit_ = false;
  bool dah_ = false;
  bool ditPending_ = false;
  bool dahPending_ = false;
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

enum class EditItem : uint8_t { Wpm, Volume };
EditItem editItem = EditItem::Wpm;

void drawDisplay() {
  if (!displayReady) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(editItem == EditItem::Wpm ? ">WPM " : " WPM ");
  display.print(wpm);
  display.setCursor(65, 0);
  display.print(editItem == EditItem::Volume ? ">VOL " : " VOL ");
  display.print(static_cast<uint16_t>(volumeLevel) * 10);
  display.print('%');

  display.drawFastHLine(0, 11, 128, SSD1306_WHITE);
  display.setCursor(0, 17);
  display.print("NOW: ");
  display.print(currentMorseDisplay);
  display.drawFastHLine(0, 31, 128, SSD1306_WHITE);

  // 最下段は新しい復号文字が右へ追加され、満杯なら左から流れていく。
  display.setTextSize(2);
  display.setCursor(0, 41);
  display.print(decodedHistory);
  display.display();
  displayDirty = false;
}

void applyEncoderStep(int8_t direction) {
  if (REVERSE_ENCODER) direction = -direction;
  if (editItem == EditItem::Wpm) {
    const int value = constrain(static_cast<int>(wpm) + direction, MIN_WPM, MAX_WPM);
    wpm = static_cast<uint8_t>(value);
  } else {
    const int value = constrain(static_cast<int>(volumeLevel) + direction,
                                MIN_VOLUME, MAX_VOLUME);
    volumeLevel = static_cast<uint8_t>(value);
  }
  displayDirty = true;
}

void updateControls(uint32_t now) {
  // Full-step quadrature decoder: one adjustment per physical encoder detent.
  static const int8_t transitions[16] = {
      0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0,
  };
  static uint8_t previousEncoderState = 0;
  static int8_t encoderAccumulator = 0;
  const uint8_t encoderState =
      (digitalRead(ENCODER_A_PIN) == HIGH ? 2 : 0) |
      (digitalRead(ENCODER_B_PIN) == HIGH ? 1 : 0);
  encoderAccumulator += transitions[(previousEncoderState << 2) | encoderState];
  previousEncoderState = encoderState;
  if (encoderAccumulator >= 4) {
    encoderAccumulator = 0;
    applyEncoderStep(1);
  } else if (encoderAccumulator <= -4) {
    encoderAccumulator = 0;
    applyEncoderStep(-1);
  }

  static bool previousRawButton = HIGH;
  static bool stableButton = HIGH;
  static uint32_t buttonChangedMs = 0;
  const bool rawButton = digitalRead(SELECT_BUTTON_PIN);
  if (rawButton != previousRawButton) {
    previousRawButton = rawButton;
    buttonChangedMs = now;
  }
  if (stableButton != rawButton && now - buttonChangedMs >= 20) {
    stableButton = rawButton;
    if (stableButton == LOW) {
      editItem = editItem == EditItem::Wpm ? EditItem::Volume : EditItem::Wpm;
      displayDirty = true;
    }
  }
}

void setup() {
  configureInput(DOT_PIN);
  configureInput(DASH_PIN);
  pinMode(SELECT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(ENCODER_A_PIN, INPUT_PULLUP);
  pinMode(ENCODER_B_PIN, INPUT_PULLUP);
  initBuzzer();

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  displayReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  if (displayReady) drawDisplay();

  bleKeyboard.begin();
}

void loop() {
  const uint32_t now = millis();
  updateControls(now);
  if (displayDirty) drawDisplay();
  updatePaddles(now);
  keyer.tick(now);
  if (!keyer.sending()) decoder.flush(now);
  bleTextSender.tick(now);
  delay(1);
}
