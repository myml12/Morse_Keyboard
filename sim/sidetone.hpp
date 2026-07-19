#pragma once

#include <atomic>
#include <cmath>

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>

// CoreAudio で可変周波数サイドトーンをゲート再生する。
class Sidetone {
 public:
  static constexpr double kAmplitude = 0.22;
  static constexpr UInt32 kSampleRate = 48000;

  Sidetone() = default;
  ~Sidetone() { Shutdown(); }

  Sidetone(const Sidetone&) = delete;
  Sidetone& operator=(const Sidetone&) = delete;

  bool Init() {
    if (unit_ != nullptr) {
      return true;
    }

    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (comp == nullptr) {
      return false;
    }
    if (AudioComponentInstanceNew(comp, &unit_) != noErr) {
      unit_ = nullptr;
      return false;
    }

    AudioStreamBasicDescription format{};
    format.mSampleRate = kSampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                          kAudioFormatFlagIsNonInterleaved;
    format.mBytesPerPacket = sizeof(float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float);
    format.mChannelsPerFrame = 1;
    format.mBitsPerChannel = 32;

    if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0, &format,
                             sizeof(format)) != noErr) {
      Shutdown();
      return false;
    }

    AURenderCallbackStruct cb{};
    cb.inputProc = &Sidetone::Render;
    cb.inputProcRefCon = this;
    if (AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input, 0, &cb,
                             sizeof(cb)) != noErr) {
      Shutdown();
      return false;
    }

    if (AudioUnitInitialize(unit_) != noErr) {
      Shutdown();
      return false;
    }
    if (AudioOutputUnitStart(unit_) != noErr) {
      Shutdown();
      return false;
    }
    return true;
  }

  void Shutdown() {
    if (unit_ == nullptr) {
      return;
    }
    AudioOutputUnitStop(unit_);
    AudioUnitUninitialize(unit_);
    AudioComponentInstanceDispose(unit_);
    unit_ = nullptr;
    gate_.store(false, std::memory_order_relaxed);
  }

  void SetFrequency(double hz) {
    if (hz < 100.0) {
      hz = 100.0;
    }
    if (hz > 2000.0) {
      hz = 2000.0;
    }
    hz_.store(hz, std::memory_order_relaxed);
  }

  double Frequency() const { return hz_.load(std::memory_order_relaxed); }

  void Start() { gate_.store(true, std::memory_order_relaxed); }
  void Stop() { gate_.store(false, std::memory_order_relaxed); }
  bool Gated() const { return gate_.load(std::memory_order_relaxed); }

 private:
  static OSStatus Render(void* inRefCon, AudioUnitRenderActionFlags*,
                         const AudioTimeStamp*, UInt32, UInt32 inNumberFrames,
                         AudioBufferList* ioData) {
    auto* self = static_cast<Sidetone*>(inRefCon);
    float* out = static_cast<float*>(ioData->mBuffers[0].mData);
    const bool on = self->gate_.load(std::memory_order_relaxed);
    const double hz = self->hz_.load(std::memory_order_relaxed);
    const double step = 2.0 * M_PI * hz / static_cast<double>(kSampleRate);

    for (UInt32 i = 0; i < inNumberFrames; ++i) {
      if (on) {
        out[i] = static_cast<float>(std::sin(self->phase_) * kAmplitude);
        self->phase_ += step;
        if (self->phase_ > 2.0 * M_PI) {
          self->phase_ -= 2.0 * M_PI;
        }
      } else {
        out[i] = 0.0f;
      }
    }
    return noErr;
  }

  AudioComponentInstance unit_ = nullptr;
  std::atomic<bool> gate_{false};
  std::atomic<double> hz_{700.0};
  double phase_ = 0.0;
};
