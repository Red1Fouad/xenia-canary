/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NETPLAY_SESSION_APP_H_
#define XENIA_KERNEL_NETPLAY_SESSION_APP_H_

#include "xenia/kernel/xam/app_manager.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

// App 0xFB session service. Intercepts reference-replaced messages while the
// link is active; XGI keeps the rest.
class SessionApp final : public App {
 public:
  explicit SessionApp(KernelState* kernel_state);

  X_HRESULT ExecuteDispatchMessage(uint32_t message, uint32_t buffer_ptr,
                                   uint32_t buffer_length,
                                   uint32_t* extended_error) override;

  X_HRESULT ExecuteDispatchMessageWithOverlapped(
      uint32_t message, uint32_t buffer_ptr, uint32_t buffer_length,
      uint32_t overlapped_ptr, uint32_t* extended_error) override;

  bool CompletesOverlappedItself(uint32_t message) const override;

  bool HandlesMessage(uint32_t message) const override;
};

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NETPLAY_SESSION_APP_H_
