#include <assert.h>

#include "uac_setup_policy.h"

int main() {
  assert(uac::ActionAfterInactive(1) == uac::kActivateStreamingInterface);
  assert(uac::ActionAfterActive(1, true, false, false) ==
         uac::kProgramEndpointRate);
  assert(uac::ActionAfterActive(1, true, true, false) ==
         uac::kVerifyStreamingInterface);
  assert(uac::ActionAfterActive(1, true, false, true) ==
         uac::kVerifyStreamingInterface);
  assert(uac::ActionAfterActive(1, false, false, false) ==
         uac::kVerifyStreamingInterface);

  assert(uac::ActionAfterInactive(2) == uac::kBeginClockSetup);
  assert(uac::ActionAfterActive(2, true, false, false) ==
         uac::kVerifyStreamingInterface);
  assert(uac::AllowRateBeforeInterfaceFallback(1, false, false));
  assert(!uac::AllowRateBeforeInterfaceFallback(1, true, false));
  assert(!uac::AllowRateBeforeInterfaceFallback(1, false, true));
  assert(!uac::AllowRateBeforeInterfaceFallback(2, false, false));
  return 0;
}
