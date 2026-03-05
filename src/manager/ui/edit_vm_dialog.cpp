#include "manager/ui/win32_dialogs.h"
#include "manager/ui/dlg_builder.h"
#include "manager/vm_forms.h"
#include "manager/i18n.h"
#include "manager/manager_service.h"

#include <commctrl.h>
#include <string>

enum EditDlgId {
    IDC_ED_NAME     = 200,
    IDC_ED_MEMORY   = 201,
    IDC_ED_CPUS     = 202,
    IDC_ED_NAT      = 203,
    IDC_ED_WARN     = 204,
    IDC_ED_OK       = IDOK,
};

struct EditDlgData {
    ManagerService* mgr;
    VmRecord rec;
    bool saved;
    std::string error;
};

static INT_PTR CALLBACK EditDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<EditDlgData*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<EditDlgData*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(data));

        std::string title = std::string(i18n::tr(i18n::S::kDlgEditTitlePrefix)) + data->rec.spec.name;
        SetWindowTextW(dlg, i18n::to_wide(title).c_str());

        SetDlgItemTextW(dlg, IDC_ED_NAME, i18n::to_wide(data->rec.spec.name).c_str());

        HWND mem_cb = GetDlgItem(dlg, IDC_ED_MEMORY);
        for (int i = 0; i < kNumOptions; ++i)
            SendMessageW(mem_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::to_wide(kMemoryLabels[i]).c_str()));
        SendMessage(mem_cb, CB_SETCURSEL,
            MemoryMbToIndex(static_cast<int>(data->rec.spec.memory_mb)), 0);

        HWND cpu_cb = GetDlgItem(dlg, IDC_ED_CPUS);
        for (int i = 0; i < kNumOptions; ++i)
            SendMessageW(cpu_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::to_wide(kCpuLabels[i]).c_str()));
        SendMessage(cpu_cb, CB_SETCURSEL,
            CpuCountToIndex(static_cast<int>(data->rec.spec.cpu_count)), 0);

        CheckDlgButton(dlg, IDC_ED_NAT, data->rec.spec.nat_enabled ? BST_CHECKED : BST_UNCHECKED);

        bool running = data->rec.state == VmPowerState::kRunning ||
                       data->rec.state == VmPowerState::kStarting;
        EnableWindow(mem_cb, !running);
        EnableWindow(cpu_cb, !running);

        if (running) {
            SetDlgItemTextW(dlg, IDC_ED_WARN, i18n::tr_w(i18n::S::kCpuMemoryChangeWarning).c_str());
        }

        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            int mem_idx = static_cast<int>(SendMessage(GetDlgItem(dlg, IDC_ED_MEMORY), CB_GETCURSEL, 0, 0));
            int cpu_idx = static_cast<int>(SendMessage(GetDlgItem(dlg, IDC_ED_CPUS), CB_GETCURSEL, 0, 0));

            bool running = data->rec.state == VmPowerState::kRunning ||
                           data->rec.state == VmPowerState::kStarting;

            VmEditForm form;
            form.vm_id             = data->rec.spec.vm_id;
            form.name              = GetDlgText(dlg, IDC_ED_NAME);
            form.memory_mb         = (mem_idx >= 0 && mem_idx < kNumOptions)
                                         ? kMemoryOptionsMb[mem_idx] : 4096;
            form.cpu_count         = (cpu_idx >= 0 && cpu_idx < kNumOptions)
                                         ? kCpuOptions[cpu_idx] : 4;
            form.nat_enabled       = IsDlgButtonChecked(dlg, IDC_ED_NAT) == BST_CHECKED;
            form.apply_on_next_boot = running;

            auto patch = BuildVmPatch(form, data->rec.spec);
            std::string error;
            if (data->mgr->EditVm(data->rec.spec.vm_id, patch, &error)) {
                data->saved = true;
                EndDialog(dlg, IDOK);
            } else {
                MessageBoxW(dlg, i18n::to_wide(error).c_str(), i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
            }
            return TRUE;
        }
        }
        break;

    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

bool ShowEditVmDialog(HWND parent, ManagerService& mgr,
                      const VmRecord& rec, std::string* error) {
    using S = i18n::S;
    DlgBuilder b;
    int W = 220, H = 148;
    b.Begin(i18n::tr(S::kDlgEditVm), 0, 0, W, H,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER);

    int lx = 8, lw = 40, ex = 52, ew = W - 60, y = 8, rh = 14, sp = 18;

    b.AddStatic(0,         i18n::tr(S::kDlgLabelName),   lx, y, lw, rh);
    b.AddEdit(IDC_ED_NAME,             ex, y-2, ew, rh); y += sp;
    b.AddStatic(0,         i18n::tr(S::kDlgLabelMemory), lx, y, lw, rh);
    b.AddComboBox(IDC_ED_MEMORY,       ex, y-2, ew, 100); y += sp;
    b.AddStatic(0,         i18n::tr(S::kDlgLabelVcpus),  lx, y, lw, rh);
    b.AddComboBox(IDC_ED_CPUS,         ex, y-2, ew, 100); y += sp;
    b.AddCheckBox(IDC_ED_NAT, i18n::tr(S::kDlgEnableNat), ex, y, ew, rh); y += sp;
    b.AddStatic(IDC_ED_WARN, "",       lx, y, W - 16, rh); y += sp + 4;

    b.AddDefButton(IDOK,     i18n::tr(S::kDlgBtnSave),  W - 56, y, 48, 14);

    EditDlgData data{&mgr, rec, false, ""};
    DialogBoxIndirectParamW(GetModuleHandle(nullptr), b.Build(), parent,
        EditDlgProc, reinterpret_cast<LPARAM>(&data));

    if (error) *error = data.error;
    return data.saved;
}
