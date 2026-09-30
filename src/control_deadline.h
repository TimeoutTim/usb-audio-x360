// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_CONTROL_DEADLINE_H
#define USB_AUDIO360_CONTROL_DEADLINE_H
namespace usb_transport {
inline unsigned ControlDeadline(unsigned request_type) {
  // Standard requests use Linux USB core's 5 s policy. Class requests retain
  // their existing 1 s bound; UAC2 clock has its own state-machine deadline.
  return (request_type & 0x60) == 0 ? 5000 : 1000;
}
inline bool ControlExpired(unsigned issued, unsigned now, unsigned deadline,
                           bool completed) {
  return !completed && now - issued >= deadline;
}
}
#endif
