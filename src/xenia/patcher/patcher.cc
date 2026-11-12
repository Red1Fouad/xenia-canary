/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <cstring>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/patcher/patcher.h"

DEFINE_bool(mm10_dlc_unlock, true,
            "Make the Mega Man 10 DLC (Forte, Endless Mode and Special Stages) "
            "available without an external patch.",
            "Game");

namespace xe {
namespace patcher {

namespace {

constexpr uint32_t kMegaMan10TitleId = 0x584109EE;

// 8209BD10: DLC license mask. Caller 820D9D40 tests bit 0x2; return 2.
constexpr uint32_t kMegaMan10DlcCheck = 0x8209BD10;
constexpr uint32_t kMegaMan10DlcCheckReturn = 0x38600002;
constexpr uint32_t kMegaMan10DlcCheckBlr = 0x4E800020;

}  // namespace

Patcher::Patcher(const std::filesystem::path patches_root) {
  is_any_patch_applied_ = false;
  patch_db_ = new PatchDB(patches_root);
}

void Patcher::ApplyPatchesForTitle(Memory* memory, const uint32_t title_id,
                                   const std::optional<uint64_t> hash) {
  const auto title_patches = patch_db_->GetTitlePatches(title_id, hash);

  for (const PatchFileEntry& patchFile : title_patches) {
    for (const PatchInfoEntry& patchEntry : patchFile.patch_info) {
      if (!patchEntry.is_enabled) {
        continue;
      }
      XELOGE("Patcher: Applying patch for: {}({:08X}) - {}",
             patchFile.title_name, patchFile.title_id, patchEntry.patch_name);
      ApplyPatch(memory, &patchEntry);
    }
  }
}

void Patcher::ApplyTitleFixes(Memory* memory, const uint32_t title_id) {
  if (title_id != kMegaMan10TitleId || !cvars::mm10_dlc_unlock) {
    return;
  }

  PatchInfoEntry patch{};
  patch.id = 0;
  patch.patch_name = "Mega Man 10 DLC unlock";
  patch.patch_desc =
      "Forces the DLC entitlement check to report Forte, Endless Mode and "
      "the Special Stages as owned.";
  patch.patch_author = "Xenia Canary Netplay";
  patch.is_enabled = true;
  patch.patch_data.emplace_back(
      kMegaMan10DlcCheck,
      PatchDataValue(sizeof(uint32_t),
                     xe::byte_swap(kMegaMan10DlcCheckReturn)));
  patch.patch_data.emplace_back(
      kMegaMan10DlcCheck + 4u,
      PatchDataValue(sizeof(uint32_t), xe::byte_swap(kMegaMan10DlcCheckBlr)));

  XELOGI("Patcher: applying built-in patch for: MEGA MAN 10 ({:08X}) - {}",
         title_id, patch.patch_name);
  ApplyPatch(memory, &patch);
}

void Patcher::ApplyPatch(Memory* memory, const PatchInfoEntry* patch) {
  for (const PatchDataEntry& patch_data_entry : patch->patch_data) {
    uint32_t old_address_protect = 0;
    uint8_t* address = memory->TranslateVirtual(patch_data_entry.address);
    xe::BaseHeap* heap = memory->LookupHeap(patch_data_entry.address);
    if (!heap) {
      continue;
    }

    heap->QueryProtect(patch_data_entry.address, &old_address_protect);

    heap->Protect(patch_data_entry.address,
                  (uint32_t)patch_data_entry.data.alloc_size,
                  kMemoryProtectRead | kMemoryProtectWrite);

    std::memcpy(address, patch_data_entry.data.patch_data.data(),
                patch_data_entry.data.alloc_size);

    // Restore previous protection
    heap->Protect(patch_data_entry.address,
                  (uint32_t)patch_data_entry.data.alloc_size,
                  old_address_protect);

    is_any_patch_applied_ = true;
  }
}

}  // namespace patcher
}  // namespace xe
