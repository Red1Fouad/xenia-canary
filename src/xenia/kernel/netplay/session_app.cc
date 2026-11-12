/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/netplay/session_app.h"

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/netplay/netplay_state.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

namespace {

constexpr uint32_t kAppSession = 0xFB;

constexpr uint32_t kMsgSetProperty = 0xB0007;
constexpr uint32_t kMsgPresence = 0xB0006;
constexpr uint32_t kMsgCreate = 0xB0010;
constexpr uint32_t kMsgDelete = 0xB0011;
constexpr uint32_t kMsgJoin = 0xB0012;
constexpr uint32_t kMsgLeave = 0xB0013;
constexpr uint32_t kMsgStart = 0xB0014;
constexpr uint32_t kMsgEnd = 0xB0015;
constexpr uint32_t kMsgSearch = 0xB0016;
constexpr uint32_t kMsgModify = 0xB0025;
constexpr uint32_t kMsgGetContext = 0xB0041;

// Session arg block offsets (reference layout).
constexpr uint32_t kCreateFlags = 0x04;
constexpr uint32_t kCreateInfo = 0x14;
constexpr uint32_t kCreateNonce = 0x18;
constexpr uint32_t kSearchSize = 0x18;
constexpr uint32_t kSearchResults = 0x1C;

}  // namespace

SessionApp::SessionApp(KernelState* kernel_state)
    : App(kernel_state, kAppSession) {}

X_HRESULT SessionApp::ExecuteDispatchMessage(uint32_t message,
                                             uint32_t buffer_ptr,
                                             uint32_t buffer_length,
                                             uint32_t* extended_error) {
  return ExecuteDispatchMessageWithOverlapped(message, buffer_ptr,
                                              buffer_length, 0,
                                              extended_error);
}

X_HRESULT SessionApp::ExecuteDispatchMessageWithOverlapped(
    uint32_t message, uint32_t buffer_ptr, uint32_t buffer_length,
    uint32_t overlapped_ptr, uint32_t* extended_error) {
  if (extended_error) {
    *extended_error = X_ERROR_SUCCESS;
  }

  NetplayState* netplay = kernel_state_ ? kernel_state_->netplay() : nullptr;
  if (!netplay || !netplay->IsActive()) {
    return X_E_NOTFOUND;
  }

  if (netplay->verbose()) {
    XELOGI("netplay: session message {:05X} block {:08X} size {:X}", message,
           buffer_ptr, buffer_length);
  }

  switch (message) {
    case kMsgPresence:
    case kMsgSetProperty:
      // Presence: nothing reads it back.
      return X_ERROR_SUCCESS;

    case kMsgGetContext: {
      // block+0x14 -> { context, value }; value stays 0.
      if (!buffer_ptr) {
        return X_ERROR_INVALID_PARAMETER;
      }
      if (buffer_length && buffer_length < 0x18) {
        return X_ERROR_INVALID_PARAMETER;
      }
      const uint32_t context_ptr = netplay->Read32(buffer_ptr + 0x14);
      if (context_ptr) {
        netplay->Write32(context_ptr + 4, 0);
      }
      return X_ERROR_SUCCESS;
    }

    case kMsgCreate: {
      // session, flags, slots, user, XSESSION_INFO*, nonce (82206160).
      if (!buffer_ptr) {
        return X_ERROR_INVALID_PARAMETER;
      }
      if (buffer_length && buffer_length < 0x1C) {
        return X_ERROR_INVALID_PARAMETER;
      }
      const uint32_t flags = netplay->Read32(buffer_ptr + kCreateFlags);
      const uint32_t info_ptr = netplay->Read32(buffer_ptr + kCreateInfo);
      const uint32_t nonce_ptr = netplay->Read32(buffer_ptr + kCreateNonce);
      netplay->SessionCreate(flags, info_ptr, nonce_ptr);
      return X_ERROR_SUCCESS;
    }

    case kMsgDelete:
      netplay->SessionDelete();
      return X_ERROR_SUCCESS;

    case kMsgJoin:
    case kMsgLeave:
    case kMsgStart:
    case kMsgEnd:
    case kMsgModify:
      // Members/states are the game's; link carries traffic.
      return X_ERROR_SUCCESS;

    case kMsgSearch: {
      // user, procedure, results, size, results ptr (82206718); stays pending.
      if (!buffer_ptr || !overlapped_ptr) {
        return X_ERROR_INVALID_PARAMETER;
      }
      if (buffer_length && buffer_length < 0x20) {
        return X_ERROR_INVALID_PARAMETER;
      }
      const uint32_t results_size =
          netplay->Read32(buffer_ptr + kSearchSize);
      const uint32_t results_ptr =
          netplay->Read32(buffer_ptr + kSearchResults);
      netplay->SessionSearch(overlapped_ptr, results_ptr, results_size);
      return X_ERROR_SUCCESS;
    }

    default:
      return X_ERROR_NOT_SUPPORTED;
  }
}

bool SessionApp::CompletesOverlappedItself(uint32_t message) const {
  // Search completes when host answers or times out.
  return message == kMsgSearch;
}

bool SessionApp::HandlesMessage(uint32_t message) const {
  // Replaced messages; everything else stays with XGI.
  switch (message) {
    case kMsgPresence:
    case kMsgSetProperty:
    case kMsgGetContext:
    case kMsgCreate:
    case kMsgDelete:
    case kMsgJoin:
    case kMsgLeave:
    case kMsgStart:
    case kMsgEnd:
    case kMsgSearch:
    case kMsgModify:
      return true;
    default:
      return false;
  }
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
