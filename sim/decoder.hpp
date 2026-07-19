#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

// サイドトーンの ON/OFF と 1:1 で符号を積む。
// 種別はキーヤーの relay を使い、聴こえない極短マークだけ破棄する。
class MorseDecoder {
 public:
  using Clock = std::chrono::steady_clock;

  double letter_gap_units = 3.0;
  double word_gap_units = 7.0;

  void SetDitMs(unsigned dit_ms) { dit_ms_ = dit_ms < 1 ? 1 : dit_ms; }

  void SetDashRatio(double ratio) {
    if (ratio < 2.8) {
      ratio = 2.8;
    }
    if (ratio > 4.5) {
      ratio = 4.5;
    }
    dash_ratio_ = ratio;
  }

  void SetLetterGapUnits(double units) {
    if (units < 1.2) {
      units = 1.2;
    }
    if (units > 4.0) {
      units = 4.0;
    }
    letter_gap_units = units;
  }

  char OnSilenceBeforeMark() {
    if (tx_on_ || !pending_) {
      return 0;
    }
    const double silent =
        std::chrono::duration<double, std::milli>(Clock::now() - last_finished_)
            .count();
    if (silent >= dit_ms_ * letter_gap_units) {
      return FinalizeLetter();
    }
    return 0;
  }

  void OnMarkBegin(int /*relay*/) { tx_on_ = true; }

  // relay: 0=dit, 1=dah / duration_ms: 実際にトーンが出ていた時間
  void OnMarkEnd(int relay, double duration_ms) {
    if (!tx_on_) {
      return;
    }
    tx_on_ = false;

    // 正規のトン/ツーより明らかに短いものはチャタリングとして無視
    // （末尾に付くと J=.--- が 1=.---- になる）
    const double min_ms =
        (relay == 1) ? (dit_ms_ * dash_ratio_ * 0.5) : (dit_ms_ * 0.5);
    if (duration_ms < min_ms) {
      return;
    }

    buffer_.push_back((relay == 1) ? '-' : '.');
    pending_ = true;
    space_sent_ = false;
    last_finished_ = Clock::now();
  }

  char Poll() {
    if (tx_on_) {
      return 0;
    }
    const double silent =
        std::chrono::duration<double, std::milli>(Clock::now() - last_finished_)
            .count();
    if (pending_ && silent >= dit_ms_ * letter_gap_units) {
      return FinalizeLetter();
    }
    if (!pending_ && !space_sent_ && silent >= dit_ms_ * word_gap_units) {
      space_sent_ = true;
      return ' ';
    }
    return 0;
  }

  const std::string& Buffer() const { return buffer_; }
  bool TxOn() const { return tx_on_; }

 private:
  char FinalizeLetter() {
    char ch = '?';
    const auto it = Table().find(buffer_);
    if (it != Table().end()) {
      ch = it->second;
    }
    buffer_.clear();
    pending_ = false;
    space_sent_ = false;
    return ch;
  }

  static const std::unordered_map<std::string, char>& Table() {
    static const std::unordered_map<std::string, char> table = {
        {".-", 'A'},   {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'},  {".", 'E'},
        {"..-.", 'F'}, {"--.", 'G'},  {"....", 'H'}, {"..", 'I'},   {".---", 'J'},
        {"-.-", 'K'},  {".-..", 'L'}, {"--", 'M'},   {"-.", 'N'},   {"---", 'O'},
        {".--.", 'P'}, {"--.-", 'Q'}, {".-.", 'R'},  {"...", 'S'},  {"-", 'T'},
        {"..-", 'U'},  {"...-", 'V'}, {".--", 'W'},  {"-..-", 'X'}, {"-.--", 'Y'},
        {"--..", 'Z'}, {"-----", '0'}, {".----", '1'}, {"..---", '2'},
        {"...--", '3'}, {"....-", '4'}, {".....", '5'}, {"-....", '6'},
        {"--...", '7'}, {"---..", '8'}, {"----.", '9'}, {".-.-.-", '.'},
        {"--..--", ','}, {"..--..", '?'}, {".----.", '\''}, {"-.-.--", '!'},
        {"-..-.", '/'}, {"-.--.", '('}, {"-.--.-", ')'}, {".-...", '&'},
        {"---...", ':'}, {"-.-.-.", ';'}, {"-...-", '='}, {".-.-.", '+'},
        {"-....-", '-'}, {"..--.-", '_'}, {".-..-.", '"'}, {"...-..-", '$'},
        {".--.-.", '@'},
    };
    return table;
  }

  unsigned dit_ms_ = 100;
  double dash_ratio_ = 3.0;
  bool tx_on_ = false;
  Clock::time_point last_finished_ = Clock::now();
  std::string buffer_;
  bool pending_ = false;
  bool space_sent_ = true;
};
