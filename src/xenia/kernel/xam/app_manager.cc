/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/app_manager.h"

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/netplay/netplay_state.h"
#include "xenia/kernel/netplay/session_app.h"
#include "xenia/kernel/xam/apps/messenger_app.h"
#include "xenia/kernel/xam/apps/xam_app.h"
#include "xenia/kernel/xam/apps/xgi_app.h"
#include "xenia/kernel/xam/apps/xlivebase_app.h"
#include "xenia/kernel/xam/apps/xmp_app.h"

namespace xe {
namespace kernel {
namespace xam {

App::App(KernelState* kernel_state, uint32_t app_id)
    : kernel_state_(kernel_state),
      memory_(kernel_state->memory()),
      app_id_(app_id) {}

void AppManager::RegisterApps(KernelState* kernel_state, AppManager* manager) {
  manager->RegisterApp(std::make_unique<apps::MessengerApp>(kernel_state));
  manager->RegisterApp(std::make_unique<apps::XmpApp>(kernel_state));
  manager->RegisterApp(std::make_unique<apps::XgiApp>(kernel_state));
  manager->RegisterApp(std::make_unique<apps::XLiveBaseApp>(kernel_state));
  manager->RegisterApp(std::make_unique<apps::XamApp>(kernel_state));

  // Session service beside XGI.
  auto session_app = std::make_unique<apps::SessionApp>(kernel_state);
  manager->SetNetplayApp(session_app.get());
  manager->RegisterApp(std::move(session_app));
}

void AppManager::RegisterApp(std::unique_ptr<App> app) {
  App* raw_app = app.get();
  if (app_lookup_.contains(raw_app->app_id())) {
    // Shared 0xFB: ResolveApp picks per message.
    assert_true(raw_app == netplay_app_);
    apps_.push_back(std::move(app));
    return;
  }
  app_lookup_.insert({raw_app->app_id(), raw_app});
  apps_.push_back(std::move(app));
}

App* AppManager::ResolveApp(uint32_t app_id, uint32_t message) const {
  if (app_id == 0xFB && netplay_app_ && netplay_app_->kernel_state_ &&
      netplay_app_->kernel_state_->netplay() &&
      netplay_app_->kernel_state_->netplay()->IsActive() &&
      netplay_app_->HandlesMessage(message)) {
    return netplay_app_;
  }

  const auto it = app_lookup_.find(app_id);
  return it == app_lookup_.end() ? nullptr : it->second;
}

X_HRESULT AppManager::DispatchMessageSync(uint32_t app_id, uint32_t message,
                                          uint32_t buffer_ptr,
                                          uint32_t buffer_length) {
  App* app = ResolveApp(app_id, message);
  if (!app) {
    return X_E_NOTFOUND;
  }

  return app->ExecuteDispatchMessage(message, buffer_ptr, buffer_length,
                                     nullptr);
}

X_HRESULT AppManager::DispatchMessageAsync(uint32_t app_id, uint32_t message,
                                           uint32_t buffer_ptr,
                                           uint32_t buffer_length,
                                           uint32_t overlapped_ptr) {
  App* app = ResolveApp(app_id, message);
  if (!app) {
    return X_E_NOTFOUND;
  }

  Memory* memory = app->kernel_state_->memory();

  if (overlapped_ptr && app->CompletesOverlappedItself(message)) {
    // Copied now, completed later.
    app->kernel_state_->MarkOverlappedPending(overlapped_ptr);
    uint32_t extended_error = 0;
    const X_HRESULT result = app->ExecuteDispatchMessageWithOverlapped(
        message, buffer_ptr, buffer_length, overlapped_ptr, &extended_error);
    if (result != X_ERROR_SUCCESS) {
      // Refused: release the overlapped.
      app->kernel_state_->CompleteDeferredOverlapped(overlapped_ptr, result);
      return result;
    }
    return X_ERROR_IO_PENDING;
  }

  auto buffer_in = memory->SystemHeapAlloc(buffer_length);

  auto hostOldBufferAddr = memory->TranslateVirtual(buffer_ptr);
  auto hostBufferAddr = memory->TranslateVirtual(buffer_in);

  memcpy(hostBufferAddr, hostOldBufferAddr, buffer_length);

  auto post = [memory, buffer_in]() { memory->SystemHeapFree(buffer_in); };

  auto run = [app, message, buffer_in, buffer_length](
                 uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    return app->ExecuteDispatchMessage(message, buffer_in, buffer_length,
                                       &extended_error);
  };

  if (overlapped_ptr) {
    app->kernel_state_->CompleteOverlappedDeferredEx(run, overlapped_ptr,
                                                     nullptr, post);
    return X_ERROR_IO_PENDING;
  };

  // Sync does not set extended error codes.
  const X_HRESULT result =
      DispatchMessageSync(app_id, message, buffer_in, buffer_length);
  post();
  return result;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
