#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

#include <atomic>
#include <chrono>
#include <string>

#include "decoder.hpp"
#include "keyers.h"
#include "sidetone.hpp"

namespace {

constexpr CGKeyCode kKeyD = 0x02;
constexpr CGKeyCode kKeyF = 0x03;
constexpr CGKeyCode kKeySpace = 0x31;

constexpr int kMinWpm = 6;
constexpr int kMaxWpm = 48;
constexpr int kDefaultWpm = 22;
constexpr int kDefaultKeyer = 7;  // Firmware と同じ Iambic A
constexpr int kDefaultToneHz = 700;
constexpr double kDefaultLetterGap = 3.0;
constexpr double kDefaultDashRatio = 3.0;

bool KeyDown(CGKeyCode code) {
  return CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, code);
}

unsigned DitMsFromWpm(int wpm) {
  if (wpm < 1) {
    wpm = 1;
  }
  return static_cast<unsigned>(1200 / wpm);
}

NSColor* Hex(unsigned rgb) {
  return [NSColor colorWithCalibratedRed:((rgb >> 16) & 0xff) / 255.0
                                   green:((rgb >> 8) & 0xff) / 255.0
                                    blue:(rgb & 0xff) / 255.0
                                   alpha:1.0];
}

}  // namespace

class SimTransmitter : public Transmitter {
 public:
  using Clock = std::chrono::steady_clock;

  Sidetone* tone = nullptr;
  MorseDecoder* decoder = nullptr;
  std::atomic<bool> closed{false};
  std::atomic<int> relay{-1};

  void BeginTx() override { BeginTx(0); }
  void EndTx() override { EndTx(0); }

  void BeginTx(int r) override {
    if (sounding_) {
      return;
    }
    sounding_ = true;
    mark_relay_ = r;
    mark_started_ = Clock::now();
    closed.store(true);
    relay.store(r);
    if (decoder) {
      const char letter = decoder->OnSilenceBeforeMark();
      if (letter != 0) {
        pending_output_.push_back(letter);
      }
      const char space = decoder->Poll();
      if (space != 0) {
        pending_output_.push_back(space);
      }
      decoder->OnMarkBegin(r);
    }
    if (tone) {
      tone->Start();
    }
  }

  void EndTx(int /*r*/) override {
    if (!sounding_) {
      return;
    }
    sounding_ = false;
    closed.store(false);
    relay.store(-1);
    const double ms =
        std::chrono::duration<double, std::milli>(Clock::now() - mark_started_)
            .count();
    if (tone) {
      tone->Stop();
    }
    if (decoder) {
      decoder->OnMarkEnd(mark_relay_, ms);
    }
    mark_relay_ = -1;
  }

  char TakePendingChar() {
    if (pending_output_.empty()) {
      return 0;
    }
    const char c = pending_output_.front();
    pending_output_.erase(0, 1);
    return c;
  }

  std::string pending_output_;
  bool sounding_ = false;
  int mark_relay_ = -1;
  Clock::time_point mark_started_{};
};

@interface LedView : NSView
@property(nonatomic, copy) NSString* label;
@property(nonatomic, strong) NSColor* onColor;
@property(nonatomic, assign) BOOL lit;
@end

@implementation LedView
- (void)drawRect:(NSRect)dirtyRect {
  (void)dirtyRect;
  NSColor* bg = self.lit ? self.onColor : Hex(0x303030);
  NSColor* fg = self.lit ? NSColor.whiteColor : Hex(0xb8b8b8);
  [bg setFill];
  [[NSBezierPath bezierPathWithRoundedRect:self.bounds xRadius:6 yRadius:6] fill];
  NSDictionary* attrs = @{
    NSFontAttributeName : [NSFont boldSystemFontOfSize:15],
    NSForegroundColorAttributeName : fg,
  };
  NSSize size = [self.label sizeWithAttributes:attrs];
  NSPoint p = NSMakePoint(NSMidX(self.bounds) - size.width / 2.0,
                          NSMidY(self.bounds) - size.height / 2.0);
  [self.label drawAtPoint:p withAttributes:attrs];
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation AppDelegate {
  NSWindow* window_;
  NSPopUpButton* keyerPopup_;
  NSSlider* wpmSlider_;
  NSTextField* wpmLabel_;
  NSSlider* toneSlider_;
  NSTextField* toneLabel_;
  NSSlider* letterGapSlider_;
  NSTextField* letterGapLabel_;
  NSSlider* dashRatioSlider_;
  NSTextField* dashRatioLabel_;
  NSPopUpButton* radioPresetPopup_;
  NSTextField* statusLabel_;
  LedView* ditLed_;
  LedView* dahLed_;
  NSTextView* outputView_;
  NSTimer* timer_;

  Sidetone sidetone_;
  MorseDecoder decoder_;
  SimTransmitter tx_;
  Keyer* keyer_;
  int keyer_type_;
  int wpm_;
  double dash_ratio_;
  bool dit_held_;
  bool dah_held_;
  bool straight_held_;
  char last_active_;
  NSString* last_status_;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
  (void)notification;
  keyer_type_ = kDefaultKeyer;
  wpm_ = kDefaultWpm;
  dash_ratio_ = kDefaultDashRatio;
  dit_held_ = dah_held_ = straight_held_ = false;
  last_active_ = 0;
  last_status_ = @"";

  sidetone_.Init();
  sidetone_.SetFrequency(kDefaultToneHz);
  tx_.tone = &sidetone_;
  tx_.decoder = &decoder_;
  decoder_.SetDitMs(DitMsFromWpm(wpm_));
  decoder_.SetLetterGapUnits(kDefaultLetterGap);
  decoder_.SetDashRatio(dash_ratio_);
  keyer_ = GetKeyerByNumber(keyer_type_, &tx_);
  keyer_->SetDitDuration(DitMsFromWpm(wpm_));
  keyer_->SetDashRatio(dash_ratio_);

  const NSRect contentRect = NSMakeRect(0, 0, 500, 656);
  window_ =
      [[NSWindow alloc] initWithContentRect:contentRect
                                  styleMask:NSWindowStyleMaskTitled |
                                            NSWindowStyleMaskClosable |
                                            NSWindowStyleMaskMiniaturizable
                                    backing:NSBackingStoreBuffered
                                      defer:NO];
  window_.title = @"Morse Keyboard Simulator";
  window_.delegate = self;
  [window_ center];

  NSView* root = window_.contentView;
  CGFloat y = contentRect.size.height - 32;

  NSTextField* title = [self label:@"Morse Keyboard — PC体感版" bold:YES size:16];
  title.frame = NSMakeRect(18, y, 460, 24);
  [root addSubview:title];
  y -= 34;

  NSTextField* presetTitle = [self label:@"無線機プリセット" bold:YES size:13];
  presetTitle.frame = NSMakeRect(18, y, 120, 22);
  [root addSubview:presetTitle];
  radioPresetPopup_ =
      [[NSPopUpButton alloc] initWithFrame:NSMakeRect(140, y - 2, 340, 26)
                                 pullsDown:NO];
  [radioPresetPopup_ addItemWithTitle:@"ファーム標準（1:1:3.0 / Iambic A）"];
  [radioPresetPopup_ addItemWithTitle:@"Iambic B（1:1:3.0）"];
  [radioPresetPopup_ addItemWithTitle:@"軽め（1:1:2.8）"];
  [radioPresetPopup_ addItemWithTitle:@"重め（1:1:3.2）"];
  [radioPresetPopup_ selectItemAtIndex:0];
  radioPresetPopup_.target = self;
  radioPresetPopup_.action = @selector(radioPresetChanged:);
  [root addSubview:radioPresetPopup_];
  y -= 36;

  NSTextField* keyerTitle = [self label:@"キーヤー" bold:YES size:13];
  keyerTitle.frame = NSMakeRect(18, y, 80, 22);
  [root addSubview:keyerTitle];

  keyerPopup_ = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(100, y - 2, 280, 26)
                                           pullsDown:NO];
  for (int i = 1; i <= 9; i++) {
    NSString* item =
        [NSString stringWithFormat:@"%d. %s", i, KeyerTypeName(i)];
    [keyerPopup_ addItemWithTitle:item];
  }
  [keyerPopup_ selectItemAtIndex:keyer_type_ - 1];
  keyerPopup_.target = self;
  keyerPopup_.action = @selector(keyerChanged:);
  [root addSubview:keyerPopup_];
  y -= 40;

  NSTextField* wpmTitle = [self label:@"速度 (WPM)" bold:YES size:13];
  wpmTitle.frame = NSMakeRect(18, y, 100, 22);
  [root addSubview:wpmTitle];
  wpmLabel_ = [self label:[NSString stringWithFormat:@"%d WPM", wpm_] bold:YES size:13];
  wpmLabel_.alignment = NSTextAlignmentRight;
  wpmLabel_.frame = NSMakeRect(360, y, 120, 22);
  [root addSubview:wpmLabel_];
  y -= 28;
  wpmSlider_ = [[NSSlider alloc] initWithFrame:NSMakeRect(18, y, 460, 24)];
  wpmSlider_.minValue = kMinWpm;
  wpmSlider_.maxValue = kMaxWpm;
  wpmSlider_.intValue = wpm_;
  wpmSlider_.target = self;
  wpmSlider_.action = @selector(wpmChanged:);
  [root addSubview:wpmSlider_];
  y -= 36;

  NSTextField* toneTitle = [self label:@"サイドトーン" bold:YES size:13];
  toneTitle.frame = NSMakeRect(18, y, 110, 22);
  [root addSubview:toneTitle];
  toneLabel_ =
      [self label:[NSString stringWithFormat:@"%d Hz", kDefaultToneHz] bold:YES size:13];
  toneLabel_.alignment = NSTextAlignmentRight;
  toneLabel_.frame = NSMakeRect(360, y, 120, 22);
  [root addSubview:toneLabel_];
  y -= 28;
  toneSlider_ = [[NSSlider alloc] initWithFrame:NSMakeRect(18, y, 460, 24)];
  toneSlider_.minValue = 300;
  toneSlider_.maxValue = 1200;
  toneSlider_.intValue = kDefaultToneHz;
  toneSlider_.target = self;
  toneSlider_.action = @selector(toneChanged:);
  [root addSubview:toneSlider_];
  y -= 36;

  NSTextField* gapTitle = [self label:@"文字区切り" bold:YES size:13];
  gapTitle.frame = NSMakeRect(18, y, 110, 22);
  [root addSubview:gapTitle];
  letterGapLabel_ = [self
      label:[NSString stringWithFormat:@"%.1f 単位", kDefaultLetterGap]
       bold:YES
       size:13];
  letterGapLabel_.alignment = NSTextAlignmentRight;
  letterGapLabel_.frame = NSMakeRect(360, y, 120, 22);
  [root addSubview:letterGapLabel_];
  y -= 28;
  letterGapSlider_ = [[NSSlider alloc] initWithFrame:NSMakeRect(18, y, 460, 24)];
  letterGapSlider_.minValue = 1.5;
  letterGapSlider_.maxValue = 3.5;
  letterGapSlider_.doubleValue = kDefaultLetterGap;
  letterGapSlider_.target = self;
  letterGapSlider_.action = @selector(letterGapChanged:);
  [root addSubview:letterGapSlider_];
  y -= 36;

  NSTextField* weightTitle = [self label:@"Dot/Dash Ratio" bold:YES size:13];
  weightTitle.frame = NSMakeRect(18, y, 140, 22);
  [root addSubview:weightTitle];
  dashRatioLabel_ = [self
      label:[NSString stringWithFormat:@"1:1:%.1f", kDefaultDashRatio]
       bold:YES
       size:13];
  dashRatioLabel_.alignment = NSTextAlignmentRight;
  dashRatioLabel_.frame = NSMakeRect(360, y, 120, 22);
  [root addSubview:dashRatioLabel_];
  y -= 28;
  dashRatioSlider_ = [[NSSlider alloc] initWithFrame:NSMakeRect(18, y, 460, 24)];
  dashRatioSlider_.minValue = 2.8;
  dashRatioSlider_.maxValue = 4.5;
  dashRatioSlider_.doubleValue = kDefaultDashRatio;
  dashRatioSlider_.target = self;
  dashRatioSlider_.action = @selector(dashRatioChanged:);
  [root addSubview:dashRatioSlider_];
  y -= 36;

  statusLabel_ = [self label:@"" bold:NO size:13];
  statusLabel_.font = [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightRegular];
  statusLabel_.frame = NSMakeRect(18, y, 460, 22);
  [root addSubview:statusLabel_];
  y -= 66;

  ditLed_ = [[LedView alloc] initWithFrame:NSMakeRect(80, y, 140, 52)];
  ditLed_.label = @"DIT ・";
  ditLed_.onColor = Hex(0x20c55a);
  [root addSubview:ditLed_];
  dahLed_ = [[LedView alloc] initWithFrame:NSMakeRect(280, y, 140, 52)];
  dahLed_.label = @"DAH －";
  dahLed_.onColor = Hex(0xef4444);
  [root addSubview:dahLed_];
  y -= 44;

  NSTextField* help =
      [self label:
          @"復号=トーンと同期。符号欄が .--- (4) なら J、.---- (5) なら 1 です。\n"
           @"J のつもりで (5) になるときは、DAH を少し早く離すか速度を下げてください。"
             bold:NO
             size:11];
  help.frame = NSMakeRect(18, y - 16, 460, 40);
  [root addSubview:help];
  y -= 60;

  NSScrollView* scroll =
      [[NSScrollView alloc] initWithFrame:NSMakeRect(18, y - 120, 460, 130)];
  scroll.hasVerticalScroller = YES;
  scroll.borderType = NSBezelBorder;
  outputView_ = [[NSTextView alloc] initWithFrame:scroll.bounds];
  outputView_.editable = NO;
  outputView_.richText = YES;
  outputView_.font = [NSFont monospacedSystemFontOfSize:22 weight:NSFontWeightSemibold];
  outputView_.textColor = [NSColor colorWithCalibratedWhite:0.08 alpha:1.0];
  outputView_.backgroundColor = [NSColor colorWithCalibratedWhite:0.97 alpha:1.0];
  outputView_.insertionPointColor = outputView_.textColor;
  outputView_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  scroll.documentView = outputView_;
  [root addSubview:scroll];
  y -= 156;

  NSButton* reset = [[NSButton alloc] initWithFrame:NSMakeRect(18, y, 160, 28)];
  reset.title = @"初期設定に戻す";
  reset.bezelStyle = NSBezelStyleRounded;
  reset.target = self;
  reset.action = @selector(resetToFactory:);
  [root addSubview:reset];

  NSButton* clear = [[NSButton alloc] initWithFrame:NSMakeRect(370, y, 108, 28)];
  clear.title = @"出力を消去";
  clear.bezelStyle = NSBezelStyleRounded;
  clear.target = self;
  clear.action = @selector(clearOutput:);
  [root addSubview:clear];

  [self applyFirmwareDefaultsUpdatingPreset:YES];

  [window_ makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];

  timer_ = [NSTimer scheduledTimerWithTimeInterval:0.001
                                            target:self
                                          selector:@selector(poll:)
                                          userInfo:nil
                                           repeats:YES];
  [[NSRunLoop mainRunLoop] addTimer:timer_ forMode:NSRunLoopCommonModes];
}

- (void)appendOutputChar:(char)emitted {
  if (emitted == 0) {
    return;
  }
  NSString* piece =
      (emitted == ' ') ? @" " : [NSString stringWithFormat:@"%c", emitted];
  NSDictionary* attrs = @{
    NSFontAttributeName :
        [NSFont monospacedSystemFontOfSize:22 weight:NSFontWeightSemibold],
    NSForegroundColorAttributeName :
        [NSColor colorWithCalibratedRed:0.05 green:0.12 blue:0.28 alpha:1.0],
    NSBackgroundColorAttributeName :
        [NSColor colorWithCalibratedWhite:0.97 alpha:1.0],
  };
  NSAttributedString* attr =
      [[NSAttributedString alloc] initWithString:piece attributes:attrs];
  [outputView_.textStorage appendAttributedString:attr];
  [outputView_ scrollRangeToVisible:NSMakeRange(outputView_.string.length, 0)];
}

- (NSTextField*)label:(NSString*)text bold:(BOOL)bold size:(CGFloat)size {
  NSTextField* label = [[NSTextField alloc] initWithFrame:NSZeroRect];
  label.stringValue = text;
  label.bezeled = NO;
  label.drawsBackground = NO;
  label.editable = NO;
  label.selectable = NO;
  label.font = bold ? [NSFont boldSystemFontOfSize:size]
                    : [NSFont systemFontOfSize:size];
  return label;
}

- (void)keyerChanged:(NSPopUpButton*)sender {
  const int n = (int)sender.indexOfSelectedItem + 1;
  if (n == keyer_type_) {
    return;
  }
  keyer_->Release();
  keyer_type_ = n;
  keyer_ = GetKeyerByNumber(keyer_type_, &tx_);
  keyer_->SetDitDuration(DitMsFromWpm(wpm_));
  keyer_->SetDashRatio(dash_ratio_);
  decoder_.SetDashRatio(dash_ratio_);
  dit_held_ = dah_held_ = straight_held_ = false;
}

- (void)applyDashRatio:(double)ratio {
  dash_ratio_ = ratio;
  if (dash_ratio_ < 2.8) {
    dash_ratio_ = 2.8;
  }
  if (dash_ratio_ > 4.5) {
    dash_ratio_ = 4.5;
  }
  keyer_->SetDashRatio(dash_ratio_);
  decoder_.SetDashRatio(dash_ratio_);
  dashRatioSlider_.doubleValue = dash_ratio_;
  dashRatioLabel_.stringValue =
      [NSString stringWithFormat:@"1:1:%.1f", dash_ratio_];
}

- (void)radioPresetChanged:(NSPopUpButton*)sender {
  switch (sender.indexOfSelectedItem) {
    case 0:  // ファーム標準 / Iambic A
      [self applyFirmwareDefaultsUpdatingPreset:NO];
      [radioPresetPopup_ selectItemAtIndex:0];
      break;
    case 1: {  // Iambic B
      [self applyFirmwareDefaultsUpdatingPreset:NO];
      keyer_->Release();
      keyer_type_ = 8;
      keyer_ = GetKeyerByNumber(keyer_type_, &tx_);
      keyer_->SetDitDuration(DitMsFromWpm(wpm_));
      keyer_->SetDashRatio(dash_ratio_);
      [keyerPopup_ selectItemAtIndex:7];
      [radioPresetPopup_ selectItemAtIndex:1];
      break;
    }
    case 2:
      [self applyDashRatio:2.8];
      break;
    case 3:
      [self applyDashRatio:3.2];
      break;
    default:
      break;
  }
}

- (void)wpmChanged:(NSSlider*)sender {
  wpm_ = sender.intValue;
  if (wpm_ < kMinWpm) {
    wpm_ = kMinWpm;
  }
  if (wpm_ > kMaxWpm) {
    wpm_ = kMaxWpm;
  }
  const unsigned dit = DitMsFromWpm(wpm_);
  keyer_->SetDitDuration(dit);
  decoder_.SetDitMs(dit);
  wpmLabel_.stringValue = [NSString stringWithFormat:@"%d WPM", wpm_];
}

- (void)toneChanged:(NSSlider*)sender {
  const int hz = sender.intValue;
  sidetone_.SetFrequency(hz);
  toneLabel_.stringValue = [NSString stringWithFormat:@"%d Hz", hz];
}

- (void)letterGapChanged:(NSSlider*)sender {
  const double units = sender.doubleValue;
  decoder_.SetLetterGapUnits(units);
  letterGapLabel_.stringValue =
      [NSString stringWithFormat:@"%.1f 単位", units];
}

- (void)dashRatioChanged:(NSSlider*)sender {
  [self applyDashRatio:sender.doubleValue];
}

- (void)applyFirmwareDefaultsUpdatingPreset:(BOOL)update_preset {
  // ファーム既定値（22 WPM / 700 Hz / Iambic A / 1:1:3.0）
  wpm_ = kDefaultWpm;
  dash_ratio_ = kDefaultDashRatio;

  if (keyer_) {
    keyer_->Release();
  }
  keyer_type_ = kDefaultKeyer;
  keyer_ = GetKeyerByNumber(keyer_type_, &tx_);
  keyer_->SetDitDuration(DitMsFromWpm(wpm_));
  keyer_->SetDashRatio(dash_ratio_);

  sidetone_.SetFrequency(kDefaultToneHz);
  decoder_.SetDitMs(DitMsFromWpm(wpm_));
  decoder_.SetDashRatio(dash_ratio_);
  decoder_.SetLetterGapUnits(kDefaultLetterGap);

  dit_held_ = dah_held_ = straight_held_ = false;

  wpmSlider_.intValue = wpm_;
  wpmLabel_.stringValue = [NSString stringWithFormat:@"%d WPM", wpm_];
  toneSlider_.intValue = kDefaultToneHz;
  toneLabel_.stringValue = [NSString stringWithFormat:@"%d Hz", kDefaultToneHz];
  letterGapSlider_.doubleValue = kDefaultLetterGap;
  letterGapLabel_.stringValue =
      [NSString stringWithFormat:@"%.1f 単位", kDefaultLetterGap];
  dashRatioSlider_.doubleValue = dash_ratio_;
  dashRatioLabel_.stringValue =
      [NSString stringWithFormat:@"1:1:%.1f", dash_ratio_];
  [keyerPopup_ selectItemAtIndex:keyer_type_ - 1];
  if (update_preset) {
    [radioPresetPopup_ selectItemAtIndex:0];
  }
}

- (void)resetToFactory:(id)sender {
  (void)sender;
  [self applyFirmwareDefaultsUpdatingPreset:YES];
}

- (void)clearOutput:(id)sender {
  (void)sender;
  outputView_.string = @"";
}

// macOS のキー状態はチャタリングしないため、押下・離しとも即時に反映する。
- (void)setPaddle:(Paddle)paddle
             held:(bool*)slot
              now:(bool)now {
  if (*slot == now) {
    return;
  }
  *slot = now;
  keyer_->Key(paddle, now);
}

- (void)poll:(NSTimer*)timer {
  (void)timer;
  const auto now_ms = static_cast<unsigned long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());

  bool dit = false, dah = false, straight = false;
  if (window_.isKeyWindow) {
    dit = KeyDown(kKeyD);
    dah = KeyDown(kKeyF);
    straight = KeyDown(kKeySpace);
  }

  if (keyer_type_ == 1) {
    const bool sk = dit || straight;
    [self setPaddle:PADDLE_DIT
               held:&dit_held_
                now:sk];
    if (dah_held_) {
      dah_held_ = false;
      keyer_->Key(PADDLE_DAH, false);
    }
    straight_held_ = false;
    (void)dah;
  } else {
    // Space は Dit の OR（別スロットだと Key(DIT) が二重になる）
    const bool dit_raw = dit || straight;
    straight_held_ = straight;
    [self setPaddle:PADDLE_DIT
               held:&dit_held_
                now:dit_raw];
    [self setPaddle:PADDLE_DAH
               held:&dah_held_
                now:dah];
  }

  keyer_->Tick(now_ms);

  [self appendOutputChar:tx_.TakePendingChar()];
  [self appendOutputChar:decoder_.Poll()];

  const bool tx = tx_.closed.load();
  const int relay = tx_.relay.load();
  char active = 0;
  if (tx) {
    active = (relay == 1) ? '-' : '.';
  }
  if (active != last_active_) {
    last_active_ = active;
    ditLed_.lit = (active == '.');
    dahLed_.lit = (active == '-');
    [ditLed_ setNeedsDisplay:YES];
    [dahLed_ setNeedsDisplay:YES];
  }

  NSString* buf = decoder_.Buffer().empty()
                      ? @"-"
                      : [NSString stringWithUTF8String:decoder_.Buffer().c_str()];
  NSMutableString* paddle = [NSMutableString string];
  if (dit_held_ || (keyer_type_ == 1 && dit_held_)) {
    [paddle appendString:@"D"];
  }
  if (dah_held_) {
    [paddle appendString:@"F"];
  }
  if (straight_held_) {
    [paddle appendString:@"␣"];
  }
  if (paddle.length == 0) {
    [paddle appendString:@"-"];
  }
  NSString* status = [NSString
      stringWithFormat:@"符号: %@ (%lu)  パドル: %-3@  TX: %s  %s", buf,
                       (unsigned long)decoder_.Buffer().size(), paddle,
                       tx ? "ON" : "off", KeyerTypeName(keyer_type_)];
  if (![status isEqualToString:last_status_]) {
    last_status_ = status;
    statusLabel_.stringValue = status;
  }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  (void)sender;
  return YES;
}

- (void)windowWillClose:(NSNotification*)notification {
  (void)notification;
  [timer_ invalidate];
  timer_ = nil;
  if (keyer_) {
    keyer_->Release();
  }
  sidetone_.Stop();
  sidetone_.Shutdown();
}
@end

int main(int /*argc*/, const char* /*argv*/[]) {
  @autoreleasepool {
    NSApplication* app = [NSApplication sharedApplication];
    app.activationPolicy = NSApplicationActivationPolicyRegular;

    NSMenu* menubar = [[NSMenu alloc] init];
    NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
    [menubar addItem:appMenuItem];
    app.mainMenu = menubar;
    NSMenu* appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"Quit Morse Simulator"
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    appMenuItem.submenu = appMenu;

    AppDelegate* delegate = [[AppDelegate alloc] init];
    app.delegate = delegate;
    [app run];
  }
  return 0;
}
