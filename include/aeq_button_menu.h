#pragma once

#include <stdint.h>

enum class AeqMenuItem : uint8_t {
  None,
  Sensitivity,
  Afterglow,
  IdleBrightness,
};

class AeqButtonMenu {
 public:
  static constexpr uint32_t TIMEOUT_MS = 30000;

  bool active() const { return item_ != AeqMenuItem::None; }
  AeqMenuItem item() const { return item_; }
  bool forcesIdleOutput() const
  {
    return item_ == AeqMenuItem::IdleBrightness;
  }

  void advance(uint32_t nowMs)
  {
    switch (item_) {
      case AeqMenuItem::None:
        item_ = AeqMenuItem::Sensitivity;
        break;
      case AeqMenuItem::Sensitivity:
        item_ = AeqMenuItem::Afterglow;
        break;
      case AeqMenuItem::Afterglow:
        item_ = AeqMenuItem::IdleBrightness;
        break;
      case AeqMenuItem::IdleBrightness:
        item_ = AeqMenuItem::Sensitivity;
        break;
    }
    lastInteractionMs_ = nowMs;
    cueStartedMs_ = nowMs;
    cuePulses_ = selectionPulseCount();
    exitCue_ = false;
  }

  void touch(uint32_t nowMs)
  {
    if (active()) lastInteractionMs_ = nowMs;
  }

  bool update(uint32_t nowMs)
  {
    if (!active() || (uint32_t)(nowMs - lastInteractionMs_) < TIMEOUT_MS) {
      return false;
    }
    exit(nowMs);
    return true;
  }

  void exit(uint32_t nowMs)
  {
    item_ = AeqMenuItem::None;
    cueStartedMs_ = nowMs;
    cuePulses_ = 1;
    exitCue_ = true;
  }

  void cancel()
  {
    item_ = AeqMenuItem::None;
    cuePulses_ = 0;
    exitCue_ = false;
  }

  // Returns true while menu feedback should temporarily own the LED. Level is
  // normalized to 0..1 so both firmware platforms can use their own output.
  bool feedbackLevel(uint32_t nowMs, float &level) const
  {
    if (cuePulses_ == 0) return false;
    const uint32_t elapsed = nowMs - cueStartedMs_;
    if (exitCue_) {
      if (elapsed >= 350) return false;
      level = 1.0f;
      return true;
    }

    constexpr uint32_t PULSE_PHASE_MS = 110;
    const uint32_t phase = elapsed / PULSE_PHASE_MS;
    if (phase >= (uint32_t)cuePulses_ * 2U) return false;
    level = (phase & 1U) == 0 ? 1.0f : 0.0f;
    return true;
  }

 private:
  uint8_t selectionPulseCount() const
  {
    switch (item_) {
      case AeqMenuItem::Sensitivity: return 2;
      case AeqMenuItem::Afterglow: return 3;
      case AeqMenuItem::IdleBrightness: return 4;
      case AeqMenuItem::None: return 0;
    }
    return 0;
  }

  AeqMenuItem item_ = AeqMenuItem::None;
  uint32_t lastInteractionMs_ = 0;
  uint32_t cueStartedMs_ = 0;
  uint8_t cuePulses_ = 0;
  bool exitCue_ = false;
};
