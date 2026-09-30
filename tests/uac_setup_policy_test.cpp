#include <assert.h>

#include "uac_setup_policy.h"

int main() {
  assert(uac::ActionAfterInactive(1) == uac::kActivateStreamingInterface);
  assert(uac::ActionAfterActive(1, true) == uac::kProgramEndpointRate);
  assert(uac::ActionAfterActive(1, false) == uac::kVerifyStreamingInterface);

  assert(uac::ActionAfterInactive(2) == uac::kBeginClockSetup);
  assert(uac::ActionAfterActive(2, true) == uac::kVerifyStreamingInterface);
  return 0;
}
