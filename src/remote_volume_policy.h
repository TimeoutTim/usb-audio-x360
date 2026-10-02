// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace remote_volume {
enum Action { None, Up, Down, ToggleMute };

// RC6 toggle changes on a new press and stays constant during a hold.
// Missing release packets are normal; an idle gap also starts a fresh press.
class Policy {
 public:
  Policy() : valid_(false), command_(0), toggle_(0), last_(0),
             start_(0), repeated_(false), action_time_(0) {}
  Action Input(const unsigned char* message, unsigned int size,
               unsigned int now) {
    if (!message || size != 16 || message[0] != 0x83 || message[1] != 0x23)
      return None;
    unsigned char command = message[3];
    unsigned char toggle = message[2] & 0x80;
    bool fresh = !valid_ || command != command_ || toggle != toggle_ ||
                 now - last_ >= 500u;
    valid_ = true;
    command_ = command;
    toggle_ = toggle;
    last_ = now;
    if (fresh) {
      start_ = action_time_ = now;
      repeated_ = false;
    }
    if (command == 0x0e) return fresh ? ToggleMute : None;
    if (command != 0x10 && command != 0x11) return None;
    if (!fresh) {
      if (now - start_ < 500u || (repeated_ && now - action_time_ < 200u))
        return None;
      repeated_ = true;
      action_time_ = now;
    }
    return command == 0x10 ? Up : Down;
  }
 private:
  bool valid_;
  unsigned char command_, toggle_;
  unsigned int last_, start_;
  bool repeated_;
  unsigned int action_time_;
};
}  // namespace remote_volume
