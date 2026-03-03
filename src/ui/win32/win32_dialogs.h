#pragma once

#include "manager/manager_service.h"
#include "manager/update_checker.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

// Modal dialogs for VM creation and editing.
// Return true if the user confirmed and the operation succeeded.
bool ShowCreateVmDialog(HWND parent, ManagerService& mgr, std::string* error);
bool ShowEditVmDialog(HWND parent, ManagerService& mgr,
                      const VmRecord& rec, std::string* error);

// Modal dialog for managing shared folders of a VM.
void ShowSharedFoldersDialog(HWND parent, ManagerService& mgr, const std::string& vm_id);

// Show update available prompt and download progress dialog.
// Returns true if the MSI was downloaded and the installer launched (caller should exit).
bool ShowUpdateDialog(HWND parent, const update::UpdateInfo& info);
