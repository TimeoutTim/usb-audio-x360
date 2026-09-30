// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_UAC_SETUP_POLICY_H_
#define USB_AUDIO360_UAC_SETUP_POLICY_H_

namespace uac {

enum InactiveAction {
  kBeginClockSetup,
  kActivateStreamingInterface
};

enum ActiveAction {
  kProgramEndpointRate,
  kVerifyStreamingInterface
};

// UAC1 endpoint controls belong to the active streaming alternate setting.
// UAC2 clock controls, by contrast, are configured before that setting is
// selected.
inline InactiveAction ActionAfterInactive(unsigned audio_class_version) {
  return audio_class_version == 2 ? kBeginClockSetup
                                  : kActivateStreamingInterface;
}

inline ActiveAction ActionAfterActive(unsigned audio_class_version,
                                      bool endpoint_rate_control) {
  return audio_class_version == 1 && endpoint_rate_control
             ? kProgramEndpointRate
             : kVerifyStreamingInterface;
}

}  // namespace uac

#endif  // USB_AUDIO360_UAC_SETUP_POLICY_H_
