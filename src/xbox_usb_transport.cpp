// SPDX-License-Identifier: GPL-3.0-or-later
#include "xbox_usb_transport.h"
#include "cleanup_lifecycle.h"
#include "transfer_ownership.h"

#include <stddef.h>
#include <string.h>

extern "C" LONG XexGetProcedureAddress(HANDLE, DWORD, PVOID);
extern "C" volatile DWORD UsbAudioDiagnostic[64];
extern "C" volatile DWORD UsbAudioCleanupDiagnostic[16];

namespace {
typedef VOID (*DpcRoutine)(void*, void*, void*, void*);
typedef LONG (*DispatchFn)(DpcRoutine, void*, DWORD, void*, void*);
typedef LONG (*OpenDefaultFn)(void*, DWORD*);
typedef LONG (*OpenFn)(void*, int, int, int, int, DWORD*);
typedef VOID (*ControlFn)(void*, void*);
typedef VOID (*IsochFn)(void*, void*, WORD*);
typedef VOID (*CloseFn)(void*, void*);
struct CloseRequest {
  DWORD endpoint;
  DWORD callback;
  DWORD saved_endpoint;
  DWORD link;
  BYTE flags;
  BYTE controller_index;
  BYTE pad2;
  BYTE endpoint_index;
  void* buffer;
  DWORD length;
};
typedef char RequireCloseSavedEndpointAt8[
    offsetof(CloseRequest, saved_endpoint) == 8 ? 1 : -1];
typedef char RequireCloseLinkAt12[offsetof(CloseRequest, link) == 12 ? 1 : -1];
static DispatchFn g_dispatch = 0;
static const AudioHostApi* g_api = 0;
static void* g_handle = 0;
static volatile LONG g_stopped = 0;
static volatile LONG g_detached = 0;
static DWORD g_processor = 0;
static usb_transport::Ownership g_ownership;
static usb_transport::CleanupLifecycle g_cleanup;
static UsbRemoveCompleteRoutine g_remove_complete = 0;
static UsbRearmCompleteRoutine g_rearm_complete = 0;
static DWORD g_default_endpoint = 0;
static DWORD g_data_endpoints[3] = {0, 0, 0};
static unsigned g_data_endpoint_count = 0;
static CloseRequest g_close_requests[4];
static bool g_close_default[4] = {false, false, false, false};

// PCR field verified for the supported retail kernel. Read only, no IRQL change.
__declspec(naked) DWORD CurrentIrql() {
  __asm {
    lbz r3, 0x18(r13)
    blr
  }
}

DWORD Context() { return (GetCurrentProcessorNumber() << 16) | CurrentIrql(); }
bool InDomain() { return Context() == ((g_processor << 16) | 2); }

enum Operation { kOpenDefault, kOpen, kControl, kIsoch };
struct DomainRequest { UsbDomainRoutine routine; void* context; LONG result; };
VOID RunInDomain(void*, void* context, void*, void*) {
  DomainRequest* r = (DomainRequest*)context;
  if (!InDomain() || g_stopped || !g_handle) { r->result = -1; return; }
  r->routine(r->context);
  r->result = 0;
}
struct Request {
  Operation operation;
  void* handle;
  void* transfer;
  WORD* lengths;
  int address, max_packet, interval;
  unsigned generation;
  LONG result;
};

VOID Execute(void*, void* context, void*, void*) {
  Request* r = (Request*)context;
  UsbAudioDiagnostic[61] = Context();
  if (!InDomain() || g_stopped || r->handle != g_handle ||
      r->generation != g_ownership.generation()) { r->result = -1; return; }
  switch (r->operation) {
    case kOpenDefault:
      r->result = ((OpenDefaultFn)g_api->usbd_open_default_endpoint)(
          r->handle, (DWORD*)r->transfer);
      if (r->result >= 0) g_default_endpoint = *(DWORD*)r->transfer;
      break;
    case kOpen:
      r->result = ((OpenFn)g_api->usbd_open_endpoint)(r->handle, 1, r->address,
          r->max_packet, r->interval, (DWORD*)r->transfer);
      if (r->result >= 0) {
        DWORD endpoint = *(DWORD*)r->transfer;
        bool duplicate = false;
        for (unsigned i = 0; i < g_data_endpoint_count; ++i)
          if (g_data_endpoints[i] == endpoint) duplicate = true;
        if (!duplicate && endpoint && g_data_endpoint_count < 3)
          g_data_endpoints[g_data_endpoint_count++] = endpoint;
      }
      break;
    case kControl: case kIsoch:
      // Publish ownership before calling the host (completion can be immediate).
      if (!g_ownership.Submit(r->transfer, r->generation)) { r->result = -2; return; }
      r->result = 0;  // Accepted by adapter, not a USB completion status.
      if (r->operation == kControl)
        ((ControlFn)g_api->usbd_queue_async_transfer)(r->handle, r->transfer);
      else ((IsochFn)g_api->usbd_queue_isoch_transfer)(r->handle, r->transfer, r->lengths);
      break;
  }
}

LONG Dispatch(Operation op, void* handle, void* transfer, WORD* lengths,
              int address, int max_packet, int interval) {
  // Never call the blocking dispatcher from DPC/interrupt context.
  if (!g_dispatch || !handle || !transfer || CurrentIrql() != 0) return -3;
  Request request;
  request.operation = op; request.handle = handle; request.transfer = transfer;
  request.lengths = lengths; request.address = address;
  request.max_packet = max_packet; request.interval = interval;
  request.generation = g_ownership.generation(); request.result = -4;
  LONG wait_status = g_dispatch(Execute, &request, g_processor, 0, 0);
  return wait_status < 0 ? wait_status : request.result;
}
}

BOOL UsbTransportInitialize(const AudioHostApi* api) {
  if (!api || !api->usbd_close_default_endpoint ||
      !api->usbd_close_endpoint) return FALSE;
  HANDLE kernel = GetModuleHandleA("xboxkrnl.exe");
  if (!kernel || XexGetProcedureAddress(kernel, 886, &g_dispatch) < 0 || !g_dispatch)
    return FALSE;
  // All controller ISR affinities must agree. A default-target DPC is queued
  // from its ISR onto that processor. No kernel DPC fields are modified.
  BYTE** controllers = (BYTE**)0x801A8230;
  for (unsigned i = 0; i < 4; ++i) {
    const BYTE* hcd = controllers[i];
    if (!hcd || hcd[0x17] != 0 || hcd[0x13] >= 6) return FALSE;
    if (!i) g_processor = hcd[0x13];
    else if (g_processor != hcd[0x13]) return FALSE;
  }
  g_api = api;
  return TRUE;
}

BOOL UsbTransportAttach(void* handle) {
  UsbAudioDiagnostic[56] = Context();
  if (!g_api || !handle || g_handle || g_stopped || g_detached ||
      g_cleanup.begun() || !InDomain())
    return FALSE;
  g_default_endpoint = 0;
  memset(g_data_endpoints, 0, sizeof(g_data_endpoints));
  g_data_endpoint_count = 0;
  g_remove_complete = 0;
  g_rearm_complete = 0;
  g_handle = handle;
  return TRUE;
}

LONG __cdecl CloseComplete(DWORD request, LONG status) {
  UsbAudioCleanupDiagnostic[7] = Context();
  UsbAudioCleanupDiagnostic[6] = (DWORD)status;
  if (!InDomain()) {
    UsbAudioCleanupDiagnostic[15] = 0xe301;
    return 0;
  }
  for (unsigned i = 0; i < 4; ++i) {
    if (request != (DWORD)&g_close_requests[i]) continue;
    if (!g_cleanup.Complete(i)) {
      UsbAudioCleanupDiagnostic[15] = 0xe302;
      return 0;
    }
    ++UsbAudioCleanupDiagnostic[3];
    UsbAudioCleanupDiagnostic[2] = g_cleanup.pending();
    return 0;
  }
  UsbAudioCleanupDiagnostic[15] = 0xe303;
  return 0;
}

BOOL UsbTransportDetach(void* handle,
                        UsbRemoveCompleteRoutine remove_complete,
                        UsbRearmCompleteRoutine rearm_complete) {
  if (handle != g_handle || !remove_complete || !rearm_complete) return FALSE;
  InterlockedExchange(&g_stopped, 1);
  if (!InDomain()) {
    UsbAudioCleanupDiagnostic[15] = 0xe304;
    return FALSE;
  }
  g_ownership.Stop();
  InterlockedExchange(&g_detached, 1);
  memset((void*)UsbAudioCleanupDiagnostic, 0,
         sizeof(DWORD) * 16);
  UsbAudioCleanupDiagnostic[0] = 1;
  UsbAudioCleanupDiagnostic[4] = g_ownership.pending();
  UsbAudioCleanupDiagnostic[7] = Context();
  g_remove_complete = remove_complete;
  g_rearm_complete = rearm_complete;

  unsigned count = 0;
  for (unsigned i = 0; i < g_data_endpoint_count; ++i) {
    memset(&g_close_requests[count], 0, sizeof(CloseRequest));
    g_close_requests[count].endpoint = g_data_endpoints[i];
    g_close_requests[count].callback = (DWORD)CloseComplete;
    g_close_requests[count].saved_endpoint = g_data_endpoints[i];
    g_close_default[count++] = false;
  }
  if (g_default_endpoint) {
    memset(&g_close_requests[count], 0, sizeof(CloseRequest));
    g_close_requests[count].endpoint = g_default_endpoint;
    g_close_requests[count].callback = (DWORD)CloseComplete;
    g_close_requests[count].saved_endpoint = g_default_endpoint;
    g_close_default[count++] = true;
  }
  if (!g_cleanup.Begin(count)) {
    UsbAudioCleanupDiagnostic[15] = 0xe305;
    return FALSE;
  }
  UsbAudioCleanupDiagnostic[1] = count;
  UsbAudioCleanupDiagnostic[2] = count;
  for (unsigned i = 0; i < count; ++i) {
    if (g_close_default[i])
      ((CloseFn)g_api->usbd_close_default_endpoint)(
          g_handle, &g_close_requests[i]);
    else
      ((CloseFn)g_api->usbd_close_endpoint)(g_handle, &g_close_requests[i]);
  }
  return TRUE;
}

struct RearmRequest { LONG result; };
VOID RearmInDomain(void*, void* context, void*, void*) {
  RearmRequest* r = (RearmRequest*)context;
  ++UsbAudioDiagnostic[58];
  UsbAudioDiagnostic[59] = g_ownership.pending();
  UsbAudioCleanupDiagnostic[2] = g_cleanup.pending();
  UsbAudioCleanupDiagnostic[4] = g_ownership.pending();
  if (!InDomain() || !g_detached || !g_stopped || !g_handle ||
      !g_remove_complete ||
      !g_cleanup.Finalize(g_ownership.pending())) {
    UsbAudioDiagnostic[60] = 0xe200;
    r->result = -1;
    return;
  }
  UsbAudioCleanupDiagnostic[5] = 1;
  UsbAudioCleanupDiagnostic[7] = Context();
  UsbRemoveCompleteRoutine remove_complete = g_remove_complete;
  g_remove_complete = 0;  // The original completion is strictly one-shot.
  LONG remove_result = remove_complete(g_handle);
  UsbAudioCleanupDiagnostic[6] = (DWORD)remove_result;
  if (!g_ownership.Rearm() || !g_cleanup.Rearm()) {
    UsbAudioCleanupDiagnostic[15] = 0xe306;
    r->result = -1;
    return;
  }
  g_handle = 0;
  g_default_endpoint = 0;
  memset(g_data_endpoints, 0, sizeof(g_data_endpoints));
  g_data_endpoint_count = 0;
  InterlockedExchange(&g_detached, 0);
  InterlockedExchange(&g_stopped, 0);
  UsbAudioDiagnostic[60] = 0;
  UsbAudioCleanupDiagnostic[0] = 2;
  UsbRearmCompleteRoutine rearm_complete = g_rearm_complete;
  g_rearm_complete = 0;
  rearm_complete();
  r->result = 0;
}

BOOL UsbTransportRearm() {
  if (!g_dispatch || CurrentIrql() != 0) return FALSE;
  RearmRequest request = {-1};
  LONG status = g_dispatch(RearmInDomain, &request, g_processor, 0, 0);
  return status >= 0 && request.result == 0;
}

BOOL UsbTransportComplete(void* transfer) {
  UsbAudioDiagnostic[62] = Context();
  if (!InDomain()) { InterlockedExchange(&g_stopped, 1); return FALSE; }
  return g_ownership.Complete(transfer) ? TRUE : FALSE;
}

BOOL UsbTransportIdleInDomain() {
  return InDomain() && !g_stopped && g_handle && !g_ownership.pending();
}

LONG UsbTransportOpenDefault(void* h, DWORD* ep) {
  return Dispatch(kOpenDefault, h, ep, 0, 0, 0, 0);
}
LONG UsbTransportOpen(void* h, int address, int max_packet, int interval, DWORD* ep) {
  return Dispatch(kOpen, h, ep, 0, address, max_packet, interval);
}
LONG UsbTransportControl(void* h, void* trb) {
  return Dispatch(kControl, h, trb, 0, 0, 0, 0);
}
LONG UsbTransportIsoch(void* h, void* trb, WORD* lengths) {
  if (!lengths) return -1;
  return Dispatch(kIsoch, h, trb, lengths, 0, 0, 0);
}

LONG UsbTransportRun(UsbDomainRoutine routine, void* context) {
  if (!routine || !g_dispatch || CurrentIrql() != 0) return -3;
  DomainRequest r = { routine, context, -4 };
  LONG status = g_dispatch(RunInDomain, &r, g_processor, 0, 0);
  return status < 0 ? status : r.result;
}

LONG UsbTransportIsochInDomain(void* h, void* trb, WORD* lengths) {
  if (!InDomain() || !h || !trb || !lengths) return -3;
  Request r;
  r.operation = kIsoch; r.handle = h; r.transfer = trb;
  r.lengths = lengths; r.generation = g_ownership.generation(); r.result = -4;
  Execute(0, &r, 0, 0);
  return r.result;
}
