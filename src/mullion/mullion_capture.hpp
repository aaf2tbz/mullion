#pragma once
#include "Metal.hpp"
#include <cstdint>

namespace mullion {

class CaptureState {
public:
  // a queue calls it before it commits the work of a frame: a capture of the device holds the frame
  // MULLION_CAPTURE_FRAME names, or the one after F10 is pressed
  void frame(WMT::Device device, uint64_t frame);

  CaptureState();

private:
  enum class NextAction {
    StartCapture,
    StopCapture,
    Nothing,
  };
  NextAction getNextAction(uint64_t frame);
  void scheduleNextFrameCapture(uint64_t next = 0);
  bool shouldCaptureNextFrame();

  enum class State {
    Freezed,
    Idle,
    WillCapture,
    Capturing,
  };
  State state = State::Freezed;
  uint64_t next_capture_frame = 0;
  bool capture_key_pressed_ = false;
};

} // namespace mullion
