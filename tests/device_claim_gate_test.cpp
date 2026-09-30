#include <assert.h>
#include <stdio.h>

#include "device_claim_gate.h"

int main() {
  int first = 1;
  int second = 2;
  usb_transport::DeviceClaimGate gate;

  assert(gate.available());
  assert(!gate.Activate(0));
  assert(gate.Activate(&first));
  assert(!gate.CancelActivation(&second));
  assert(gate.CancelActivation(&first));
  assert(gate.available());
  assert(gate.Activate(&first));
  assert(!gate.available());
  assert(gate.active(&first));
  assert(!gate.active(&second));
  assert(!gate.Activate(&second));

  // Removing an ignored secondary must not affect the primary claim.
  assert(!gate.BeginRemoval(&second));
  assert(gate.active(&first));

  assert(gate.BeginRemoval(&first));
  assert(gate.draining());
  assert(!gate.CancelActivation(&first));
  assert(!gate.Activate(&second));
  assert(!gate.BeginRemoval(&first));
  assert(gate.CompleteRemoval());
  assert(!gate.CompleteRemoval());

  // An already-connected secondary is not promoted automatically. It can be
  // claimed only if the USB stack enumerates it again after cleanup.
  assert(gate.available());
  assert(gate.Activate(&second));
  assert(gate.active(&second));

  puts("Single-device claim gate tests passed");
  return 0;
}
