#include "ui/win32/win32_dialogs.h"
#include "ui/common/vm_forms.h"
#include "ui/common/i18n.h"
#include "manager/app_settings.h"
#include "manager/manager_service.h"
#include "manager/http_download.h"
#include "version.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

// ── Shared helpers ──

static constexpr int kMemoryOptionsMb[] = {1024, 2048, 4096, 8192, 16384};
static const char* kMemoryLabels[]      = {"1 GB", "2 GB", "4 GB", "8 GB", "16 GB"};
static constexpr int kCpuOptions[]      = {1, 2, 4, 8, 16};
static const char* kCpuLabels[]         = {"1", "2", "4", "8", "16"};
static constexpr int kNumOptions        = 5;

static int MemoryMbToIndex(int mb) {
    for (int i = 0; i < kNumOptions; ++i)
        if (kMemoryOptionsMb[i] >= mb) return i;
    return kNumOptions - 1;
}

static int CpuCountToIndex(int count) {
    for (int i = 0; i < kNumOptions; ++i)
        if (kCpuOptions[i] >= count) return i;
    return kNumOptions - 1;
}

static std::string NextAgentName(const std::vector<VmRecord>& records) {
    int max_n = 0;
    for (const auto& rec : records) {
        const auto& name = rec.spec.name;
        if (name.size() > 6 && name.substr(0, 6) == "Agent_") {
            try { max_n = std::max(max_n, std::stoi(name.substr(6))); }
            catch (...) {}
        }
    }
    return "Agent_" + std::to_string(max_n + 1);
}

static std::string ExeDirectory() {
    char buf[MAX_PATH]{};
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    std::string dir(buf, len);
    auto sep = dir.find_last_of("\\/");
    return (sep != std::string::npos) ? dir.substr(0, sep + 1) : std::string{};
}

static std::string FindShareFile(const std::string& exe_dir, const char* filename) {
    namespace fs = std::filesystem;
    for (const char* prefix : {"share\\", "..\\share\\"}) {
        auto path = fs::path(exe_dir) / prefix / filename;
        if (fs::exists(path)) {
            std::error_code ec;
            auto canon = fs::canonical(path, ec);
            return ec ? path.string() : canon.string();
        }
    }
    return {};
}

static std::string GetDlgText(HWND dlg, int id) {
    char buf[1024]{};
    GetDlgItemTextA(dlg, id, buf, sizeof(buf));
    return buf;
}

// ── In-memory dialog template builder ──
// Builds a DLGTEMPLATE + items in a flat buffer, properly aligned.

class DlgBuilder {
public:
    void Begin(const char* title, int x, int y, int cx, int cy, DWORD style) {
        buf_.clear();
        Align(4);
        DLGTEMPLATE dt{};
        dt.style = style | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT | DS_MODALFRAME;
        dt.x = static_cast<short>(x);
        dt.y = static_cast<short>(y);
        dt.cx = static_cast<short>(cx);
        dt.cy = static_cast<short>(cy);
        Append(&dt, sizeof(dt));
        AppendWord(0); // menu
        AppendWord(0); // class
        AppendWideStr(title);
        AppendWord(9); // font size
        AppendWideStr("Segoe UI");
        count_offset_ = offsetof(DLGTEMPLATE, cdit);
    }

    void AddStatic(int id, const char* text, int x, int y, int cx, int cy) {
        AddItem(id, 0x0082, text, x, y, cx, cy,
            WS_CHILD | WS_VISIBLE | SS_LEFT);
    }

    void AddEdit(int id, int x, int y, int cx, int cy, DWORD extra = 0) {
        AddItem(id, 0x0081, "", x, y, cx, cy,
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | extra);
    }

    void AddComboBox(int id, int x, int y, int cx, int cy) {
        AddItem(id, 0x0085, "", x, y, cx, cy,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL);
    }

    void AddCheckBox(int id, const char* text, int x, int y, int cx, int cy) {
        AddItem(id, 0x0080, text, x, y, cx, cy,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX);
    }

    void AddButton(int id, const char* text, int x, int y, int cx, int cy, DWORD style = 0) {
        AddItem(id, 0x0080, text, x, y, cx, cy,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | style);
    }

    void AddDefButton(int id, const char* text, int x, int y, int cx, int cy) {
        AddButton(id, text, x, y, cx, cy, BS_DEFPUSHBUTTON);
    }

    LPCDLGTEMPLATE Build() {
        // Patch item count
        auto* dt = reinterpret_cast<DLGTEMPLATE*>(buf_.data());
        dt->cdit = static_cast<WORD>(item_count_);
        return reinterpret_cast<LPCDLGTEMPLATE>(buf_.data());
    }

private:
    std::vector<BYTE> buf_;
    int item_count_ = 0;
    size_t count_offset_ = 0;

    void Append(const void* data, size_t len) {
        auto* p = static_cast<const BYTE*>(data);
        buf_.insert(buf_.end(), p, p + len);
    }

    void AppendWord(WORD w) { Append(&w, 2); }

    void AppendWideStr(const char* s) {
        if (!s || !*s) {
            AppendWord(0);
            return;
        }
        int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        if (len <= 0) {
            AppendWord(0);
            return;
        }
        std::vector<wchar_t> wstr(len);
        MultiByteToWideChar(CP_UTF8, 0, s, -1, wstr.data(), len);
        for (int i = 0; i < len; ++i) {
            AppendWord(static_cast<WORD>(wstr[i]));
        }
    }

    void Align(size_t a) {
        while (buf_.size() % a) buf_.push_back(0);
    }

    void AddItem(int id, WORD cls, const char* text,
                 int x, int y, int cx, int cy, DWORD style)
    {
        Align(4);
        DLGITEMTEMPLATE dit{};
        dit.style = style;
        dit.x  = static_cast<short>(x);
        dit.y  = static_cast<short>(y);
        dit.cx = static_cast<short>(cx);
        dit.cy = static_cast<short>(cy);
        dit.id = static_cast<WORD>(id);
        Append(&dit, sizeof(dit));
        AppendWord(0xFFFF);
        AppendWord(cls);
        AppendWideStr(text);
        AppendWord(0); // extra data
        ++item_count_;
    }
};

// ════════════════════════════════════════════════════════════
// Create VM Dialog
// ════════════════════════════════════════════════════════════

enum CreateDlgId {
    IDC_CR_NAME       = 100,
    IDC_CR_KERNEL     = 101,
    IDC_CR_INITRD     = 102,
    IDC_CR_DISK       = 103,
    IDC_CR_MEMORY     = 104,
    IDC_CR_CPUS       = 105,
    IDC_CR_NAT        = 106,
    IDC_CR_LOCATION   = 107,
    IDC_CR_BR_KERNEL  = 108,
    IDC_CR_BR_INITRD  = 109,
    IDC_CR_BR_DISK    = 110,
    IDC_CR_BR_LOC     = 111,
    IDC_CR_OK         = IDOK,
};

struct CreateDlgData {
    ManagerService* mgr;
    bool created;
    std::string error;
};

static std::string BrowseForFile(HWND owner, const char* filter, const char* current_path) {
    char file_buf[MAX_PATH]{};
    if (current_path && *current_path)
        strncpy(file_buf, current_path, MAX_PATH - 1);

    std::string init_dir;
    if (current_path && *current_path) {
        namespace fs = std::filesystem;
        init_dir = fs::path(current_path).parent_path().string();
    }

    OPENFILENAMEA ofn{};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = owner;
    ofn.lpstrFilter  = filter;
    ofn.lpstrFile    = file_buf;
    ofn.nMaxFile     = MAX_PATH;
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!init_dir.empty())
        ofn.lpstrInitialDir = init_dir.c_str();

    if (GetOpenFileNameA(&ofn))
        return std::string(file_buf);
    return {};
}

static int CALLBACK BrowseFolderCallback(HWND hwnd, UINT msg, LPARAM, LPARAM data) {
    if (msg == BFFM_INITIALIZED && data)
        SendMessageA(hwnd, BFFM_SETSELECTIONA, TRUE, data);
    return 0;
}

static std::string BrowseForFolder(HWND owner, const char* title, const char* current_path) {
    std::string init_dir;
    if (current_path && *current_path) {
        namespace fs = std::filesystem;
        fs::path p(current_path);
        if (fs::is_directory(p))
            init_dir = p.string();
        else if (p.has_parent_path())
            init_dir = p.parent_path().string();
    }

    BROWSEINFOA bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (!init_dir.empty()) {
        bi.lpfn = BrowseFolderCallback;
        bi.lParam = reinterpret_cast<LPARAM>(init_dir.c_str());
    }

    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl) {
        char path_buf[MAX_PATH]{};
        SHGetPathFromIDListA(pidl, path_buf);
        CoTaskMemFree(pidl);
        return std::string(path_buf);
    }
    return {};
}

static INT_PTR CALLBACK CreateDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<CreateDlgData*>(GetWindowLongPtrA(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<CreateDlgData*>(lp);
        SetWindowLongPtrA(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(data));

        auto records = data->mgr->ListVms();
        SetDlgItemTextA(dlg, IDC_CR_NAME, NextAgentName(records).c_str());

        std::string dir = ExeDirectory();
        SetDlgItemTextA(dlg, IDC_CR_KERNEL, FindShareFile(dir, "vmlinuz").c_str());
        SetDlgItemTextA(dlg, IDC_CR_INITRD, FindShareFile(dir, "initramfs.cpio.gz").c_str());
        SetDlgItemTextA(dlg, IDC_CR_DISK, FindShareFile(dir, "rootfs.qcow2").c_str());

        HWND mem_cb = GetDlgItem(dlg, IDC_CR_MEMORY);
        for (int i = 0; i < kNumOptions; ++i)
            SendMessageA(mem_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kMemoryLabels[i]));
        SendMessage(mem_cb, CB_SETCURSEL, 2, 0);

        HWND cpu_cb = GetDlgItem(dlg, IDC_CR_CPUS);
        for (int i = 0; i < kNumOptions; ++i)
            SendMessageA(cpu_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kCpuLabels[i]));
        SendMessage(cpu_cb, CB_SETCURSEL, 2, 0);

        CheckDlgButton(dlg, IDC_CR_NAT, BST_CHECKED);

        auto vm_storage = settings::DefaultVmStorageDir();
        SetDlgItemTextA(dlg, IDC_CR_LOCATION, vm_storage.c_str());

        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_CR_BR_KERNEL: {
            auto cur = GetDlgText(dlg, IDC_CR_KERNEL);
            auto path = BrowseForFile(dlg, "Kernel Image (vmlinuz*)\0vmlinuz*\0All Files (*.*)\0*.*\0", cur.c_str());
            if (!path.empty()) SetDlgItemTextA(dlg, IDC_CR_KERNEL, path.c_str());
            return TRUE;
        }
        case IDC_CR_BR_INITRD: {
            auto cur = GetDlgText(dlg, IDC_CR_INITRD);
            auto path = BrowseForFile(dlg, "Initrd Image (*.cpio.gz;*.img)\0*.cpio.gz;*.img\0All Files (*.*)\0*.*\0", cur.c_str());
            if (!path.empty()) SetDlgItemTextA(dlg, IDC_CR_INITRD, path.c_str());
            return TRUE;
        }
        case IDC_CR_BR_DISK: {
            auto cur = GetDlgText(dlg, IDC_CR_DISK);
            auto path = BrowseForFile(dlg, "Disk Image (*.qcow2;*.img;*.raw)\0*.qcow2;*.img;*.raw\0All Files (*.*)\0*.*\0", cur.c_str());
            if (!path.empty()) SetDlgItemTextA(dlg, IDC_CR_DISK, path.c_str());
            return TRUE;
        }
        case IDC_CR_BR_LOC: {
            auto cur = GetDlgText(dlg, IDC_CR_LOCATION);
            auto path = BrowseForFolder(dlg, i18n::tr(i18n::S::kDlgLabelLocation), cur.c_str());
            if (!path.empty()) SetDlgItemTextA(dlg, IDC_CR_LOCATION, path.c_str());
            return TRUE;
        }
        case IDOK: {
            int mem_idx = static_cast<int>(SendMessage(GetDlgItem(dlg, IDC_CR_MEMORY), CB_GETCURSEL, 0, 0));
            int cpu_idx = static_cast<int>(SendMessage(GetDlgItem(dlg, IDC_CR_CPUS), CB_GETCURSEL, 0, 0));

            VmCreateRequest req;
            req.name          = GetDlgText(dlg, IDC_CR_NAME);
            req.source_kernel = GetDlgText(dlg, IDC_CR_KERNEL);
            req.source_initrd = GetDlgText(dlg, IDC_CR_INITRD);
            req.source_disk   = GetDlgText(dlg, IDC_CR_DISK);
            req.storage_dir   = GetDlgText(dlg, IDC_CR_LOCATION);
            req.memory_mb     = (mem_idx >= 0 && mem_idx < kNumOptions)
                                    ? kMemoryOptionsMb[mem_idx] : 4096;
            req.cpu_count     = (cpu_idx >= 0 && cpu_idx < kNumOptions)
                                    ? kCpuOptions[cpu_idx] : 4;
            req.nat_enabled   = IsDlgButtonChecked(dlg, IDC_CR_NAT) == BST_CHECKED;

            auto v = ValidateCreateRequest(req);
            if (!v.ok) {
                MessageBoxA(dlg, v.message.c_str(), i18n::tr(i18n::S::kValidationError), MB_OK | MB_ICONWARNING);
                return TRUE;
            }

            std::string error;
            if (data->mgr->CreateVm(req, &error)) {
                data->created = true;
                EndDialog(dlg, IDOK);
            } else {
                MessageBoxA(dlg, error.c_str(), i18n::tr(i18n::S::kError), MB_OK | MB_ICONERROR);
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

bool ShowCreateVmDialog(HWND parent, ManagerService& mgr, std::string* error) {
    using S = i18n::S;
    DlgBuilder b;
    int W = 260, H = 210;
    b.Begin(i18n::tr(S::kDlgCreateVm), 0, 0, W, H,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER);

    int lx = 8, lw = 40, ex = 52, ew = W - 60, y = 8, rh = 14, sp = 18;
    int bw = 16;              // browse button width
    int ew_br = ew - bw - 2;  // edit width when browse button is present
    int bx = ex + ew_br + 2;  // browse button x

    b.AddStatic(0,          i18n::tr(S::kDlgLabelName),   lx, y, lw, rh);
    b.AddEdit(IDC_CR_NAME,              ex, y-2, ew, rh); y += sp;
    b.AddStatic(0,          i18n::tr(S::kDlgLabelKernel), lx, y, lw, rh);
    b.AddEdit(IDC_CR_KERNEL,            ex, y-2, ew_br, rh);
    b.AddButton(IDC_CR_BR_KERNEL, i18n::tr(S::kDlgBtnBrowse), bx, y-2, bw, rh); y += sp;
    b.AddStatic(0,          i18n::tr(S::kDlgLabelInitrd), lx, y, lw, rh);
    b.AddEdit(IDC_CR_INITRD,            ex, y-2, ew_br, rh);
    b.AddButton(IDC_CR_BR_INITRD, i18n::tr(S::kDlgBtnBrowse), bx, y-2, bw, rh); y += sp;
    b.AddStatic(0,          i18n::tr(S::kDlgLabelDisk),   lx, y, lw, rh);
    b.AddEdit(IDC_CR_DISK,              ex, y-2, ew_br, rh);
    b.AddButton(IDC_CR_BR_DISK, i18n::tr(S::kDlgBtnBrowse), bx, y-2, bw, rh); y += sp;
    b.AddStatic(0,          i18n::tr(S::kDlgLabelMemory), lx, y, lw, rh);
    b.AddComboBox(IDC_CR_MEMORY,        ex, y-2, ew, 100); y += sp;
    b.AddStatic(0,          i18n::tr(S::kDlgLabelVcpus),  lx, y, lw, rh);
    b.AddComboBox(IDC_CR_CPUS,          ex, y-2, ew, 100); y += sp;
    b.AddCheckBox(IDC_CR_NAT, i18n::tr(S::kDlgEnableNat), ex, y, ew, rh); y += sp;
    b.AddStatic(0,          i18n::tr(S::kDlgLabelLocation), lx, y, lw, rh);
    b.AddEdit(IDC_CR_LOCATION,          ex, y-2, ew_br, rh);
    b.AddButton(IDC_CR_BR_LOC, i18n::tr(S::kDlgBtnBrowse), bx, y-2, bw, rh); y += sp + 4;

    b.AddDefButton(IDOK,     i18n::tr(S::kDlgBtnCreate), W - 56, y, 48, 14);

    CreateDlgData data{&mgr, false, ""};
    DialogBoxIndirectParamA(GetModuleHandle(nullptr), b.Build(), parent,
        CreateDlgProc, reinterpret_cast<LPARAM>(&data));

    if (error) *error = data.error;
    return data.created;
}

// ════════════════════════════════════════════════════════════
// Edit VM Dialog
// ════════════════════════════════════════════════════════════

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
    auto* data = reinterpret_cast<EditDlgData*>(GetWindowLongPtrA(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<EditDlgData*>(lp);
        SetWindowLongPtrA(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(data));

        std::string title = std::string(i18n::tr(i18n::S::kDlgEditTitlePrefix)) + data->rec.spec.name;
        SetWindowTextA(dlg, title.c_str());

        SetDlgItemTextA(dlg, IDC_ED_NAME, data->rec.spec.name.c_str());

        HWND mem_cb = GetDlgItem(dlg, IDC_ED_MEMORY);
        for (int i = 0; i < kNumOptions; ++i)
            SendMessageA(mem_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kMemoryLabels[i]));
        SendMessage(mem_cb, CB_SETCURSEL,
            MemoryMbToIndex(static_cast<int>(data->rec.spec.memory_mb)), 0);

        HWND cpu_cb = GetDlgItem(dlg, IDC_ED_CPUS);
        for (int i = 0; i < kNumOptions; ++i)
            SendMessageA(cpu_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kCpuLabels[i]));
        SendMessage(cpu_cb, CB_SETCURSEL,
            CpuCountToIndex(static_cast<int>(data->rec.spec.cpu_count)), 0);

        CheckDlgButton(dlg, IDC_ED_NAT, data->rec.spec.nat_enabled ? BST_CHECKED : BST_UNCHECKED);

        bool running = data->rec.state == VmPowerState::kRunning ||
                       data->rec.state == VmPowerState::kStarting;
        EnableWindow(mem_cb, !running);
        EnableWindow(cpu_cb, !running);

        if (running) {
            SetDlgItemTextA(dlg, IDC_ED_WARN, i18n::tr(i18n::S::kCpuMemoryChangeWarning));
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
                MessageBoxA(dlg, error.c_str(), i18n::tr(i18n::S::kError), MB_OK | MB_ICONERROR);
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
    DialogBoxIndirectParamA(GetModuleHandle(nullptr), b.Build(), parent,
        EditDlgProc, reinterpret_cast<LPARAM>(&data));

    if (error) *error = data.error;
    return data.saved;
}

// ════════════════════════════════════════════════════════════
// Shared Folders Dialog
// ════════════════════════════════════════════════════════════

enum SfDlgId {
    IDC_SF_LIST    = 300,
    IDC_SF_ADD     = 301,
    IDC_SF_REMOVE  = 302,
};

struct SfDlgData {
    ManagerService* mgr;
    std::string vm_id;
    HWND listview;
};

static void SfRefreshList(SfDlgData* data) {
    HWND lv = data->listview;
    ListView_DeleteAllItems(lv);

    auto folders = data->mgr->GetSharedFolders(data->vm_id);
    for (size_t i = 0; i < folders.size(); ++i) {
        const auto& sf = folders[i];
        LVITEMA item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<char*>(sf.tag.c_str());
        int idx = ListView_InsertItem(lv, &item);
        ListView_SetItemText(lv, idx, 1, const_cast<char*>(sf.host_path.c_str()));
        ListView_SetItemText(lv, idx, 2,
            const_cast<char*>(sf.readonly
                ? i18n::tr(i18n::S::kSfModeReadOnly)
                : i18n::tr(i18n::S::kSfModeReadWrite)));
    }
}

static INT_PTR CALLBACK SfDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<SfDlgData*>(GetWindowLongPtrA(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<SfDlgData*>(lp);
        SetWindowLongPtrA(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(data));

        RECT rc;
        GetClientRect(dlg, &rc);
        // Convert dialog units to pixels to match other dialogs (48x14 DU)
        RECT du = {0, 0, 48, 14};
        MapDialogRect(dlg, &du);
        int btn_w = du.right, btn_h = du.bottom;
        int gap = btn_h / 2, btn_gap = btn_h / 4;
        // ListView occupies left area; right column holds the buttons
        int list_w = rc.right - btn_w - gap * 3;
        int list_h = rc.bottom - gap * 2;

        HWND lv = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            gap, gap, list_w, list_h,
            dlg, reinterpret_cast<HMENU>(IDC_SF_LIST),
            GetModuleHandle(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(lv,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        // Setup columns
        LVCOLUMNA col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 110;
        col.pszText = const_cast<char*>(i18n::tr(i18n::S::kSfColTag));
        ListView_InsertColumn(lv, 0, &col);
        col.cx = list_w - 110 - 90 - 4;
        if (col.cx < 80) col.cx = 80;
        col.pszText = const_cast<char*>(i18n::tr(i18n::S::kSfColHostPath));
        ListView_InsertColumn(lv, 1, &col);
        col.cx = 90;
        col.pszText = const_cast<char*>(i18n::tr(i18n::S::kSfColMode));
        ListView_InsertColumn(lv, 2, &col);

        data->listview = lv;

        // Buttons stacked on the right
        int btn_x = gap + list_w + gap;
        MoveWindow(GetDlgItem(dlg, IDC_SF_ADD),    btn_x, gap,                          btn_w, btn_h, FALSE);
        MoveWindow(GetDlgItem(dlg, IDC_SF_REMOVE), btn_x, gap + btn_h + btn_gap,        btn_w, btn_h, FALSE);

        SfRefreshList(data);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SF_ADD: {
            auto path_str = BrowseForFolder(dlg, i18n::tr(i18n::S::kSfBrowseTitle), nullptr);
            if (!path_str.empty()) {
                size_t last_sep = path_str.find_last_of("\\/");
                std::string tag = (last_sep != std::string::npos)
                    ? path_str.substr(last_sep + 1) : "share";
                if (tag.empty()) tag = "share";

                SharedFolder sf;
                sf.tag = tag;
                sf.host_path = path_str;
                sf.readonly = false;

                std::string error;
                if (data->mgr->AddSharedFolder(data->vm_id, sf, &error)) {
                    SfRefreshList(data);
                } else {
                    MessageBoxA(dlg, error.c_str(),
                        i18n::tr(i18n::S::kError), MB_OK | MB_ICONERROR);
                }
            }
            return TRUE;
        }
        case IDC_SF_REMOVE: {
            int sel = ListView_GetNextItem(data->listview, -1, LVNI_SELECTED);
            if (sel < 0) {
                MessageBoxA(dlg, i18n::tr(i18n::S::kSfNoSelection),
                    i18n::tr(i18n::S::kError), MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            char tag_buf[64]{};
            ListView_GetItemText(data->listview, sel, 0, tag_buf, sizeof(tag_buf));
            std::string prompt = i18n::fmt(i18n::S::kSfConfirmRemoveMsg, tag_buf);
            if (MessageBoxA(dlg, prompt.c_str(),
                    i18n::tr(i18n::S::kSfConfirmRemoveTitle),
                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
                std::string error;
                if (data->mgr->RemoveSharedFolder(data->vm_id, tag_buf, &error)) {
                    SfRefreshList(data);
                } else {
                    MessageBoxA(dlg, error.c_str(),
                        i18n::tr(i18n::S::kError), MB_OK | MB_ICONERROR);
                }
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

void ShowSharedFoldersDialog(HWND parent, ManagerService& mgr, const std::string& vm_id) {
    using S = i18n::S;
    DlgBuilder b;
    int W = 380, H = 200;
    b.Begin(i18n::tr(S::kDlgSharedFolders), 0, 0, W, H,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER | DS_SETFONT);

    // Placeholder buttons (positions will be set in WM_INITDIALOG)
    int btn_h = 14, btn_w = 50;
    b.AddButton(IDC_SF_ADD,    i18n::tr(S::kSfBtnAdd),    0, 0, btn_w, btn_h);
    b.AddButton(IDC_SF_REMOVE, i18n::tr(S::kSfBtnRemove), 0, 0, btn_w, btn_h);

    SfDlgData data{&mgr, vm_id, nullptr};
    DialogBoxIndirectParamA(GetModuleHandle(nullptr), b.Build(), parent,
        SfDlgProc, reinterpret_cast<LPARAM>(&data));
}

// ════════════════════════════════════════════════════════════
// Update Dialog
// ════════════════════════════════════════════════════════════

enum UpdateDlgId {
    IDC_UP_STATUS   = 400,
    IDC_UP_PROGRESS = 401,
    IDC_UP_CANCEL   = 402,
};

static constexpr UINT WM_UPDATE_PROGRESS = WM_APP + 100;
static constexpr UINT WM_UPDATE_DONE     = WM_APP + 101;

struct UpdateDlgData {
    update::UpdateInfo info;
    std::atomic<bool> cancel_flag{false};
    std::thread download_thread;
    std::string msi_path;
    bool download_ok = false;
    bool launched = false;
    std::string download_error;
};

static std::string GetMsiTempPath(const std::string& version) {
    char tmp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, tmp);
    return std::string(tmp) + "TenBox-" + version + ".msi";
}

static INT_PTR CALLBACK UpdateDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<UpdateDlgData*>(GetWindowLongPtrA(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<UpdateDlgData*>(lp);
        SetWindowLongPtrA(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(data));

        RECT rc;
        GetClientRect(dlg, &rc);
        RECT du = {0, 0, 4, 8};
        MapDialogRect(dlg, &du);
        int margin = du.right * 2;
        int line_h = du.bottom * 2;

        int cw = rc.right - margin * 2;
        int y = margin;

        HWND status = GetDlgItem(dlg, IDC_UP_STATUS);
        MoveWindow(status, margin, y, cw, line_h, FALSE);
        SetWindowTextA(status, i18n::tr(i18n::S::kUpdateDownloading));
        y += line_h + margin / 2;

        HWND progress = CreateWindowExA(0, PROGRESS_CLASSA, "",
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            margin, y, cw, line_h,
            dlg, reinterpret_cast<HMENU>(IDC_UP_PROGRESS),
            GetModuleHandle(nullptr), nullptr);
        SendMessage(progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessage(progress, PBM_SETPOS, 0, 0);
        y += line_h + margin;

        RECT btn_du = {0, 0, 48, 14};
        MapDialogRect(dlg, &btn_du);
        int btn_w = btn_du.right, btn_h = btn_du.bottom;
        HWND cancel_btn = GetDlgItem(dlg, IDC_UP_CANCEL);
        MoveWindow(cancel_btn, rc.right - margin - btn_w, y, btn_w, btn_h, FALSE);

        data->msi_path = GetMsiTempPath(data->info.latest_version);
        data->cancel_flag.store(false);

        HWND dlg_handle = dlg;
        std::string url = data->info.download_url;
        std::string dest = data->msi_path;
        std::string sha = data->info.sha256;
        std::atomic<bool>* cancel_ptr = &data->cancel_flag;

        data->download_thread = std::thread([dlg_handle, url, dest, sha, cancel_ptr]() {
            auto result = http::DownloadFile(url, dest, sha,
                [dlg_handle](uint64_t downloaded, uint64_t total) {
                    int pct = (total > 0) ? static_cast<int>(downloaded * 100 / total) : 0;
                    PostMessage(dlg_handle, WM_UPDATE_PROGRESS, static_cast<WPARAM>(pct), 0);
                },
                cancel_ptr);
            PostMessage(dlg_handle, WM_UPDATE_DONE, result.success ? 1 : 0, 0);
        });

        return TRUE;
    }

    case WM_UPDATE_PROGRESS: {
        int pct = static_cast<int>(wp);
        HWND progress = GetDlgItem(dlg, IDC_UP_PROGRESS);
        if (progress) SendMessage(progress, PBM_SETPOS, pct, 0);
        char buf[64];
        snprintf(buf, sizeof(buf), i18n::tr(i18n::S::kUpdateDownloadProgress), pct);
        SetDlgItemTextA(dlg, IDC_UP_STATUS, buf);
        return TRUE;
    }

    case WM_UPDATE_DONE: {
        if (data->download_thread.joinable())
            data->download_thread.join();

        if (wp == 1) {
            data->download_ok = true;
            SetDlgItemTextA(dlg, IDC_UP_STATUS, i18n::tr(i18n::S::kUpdateInstalling));
            ShellExecuteA(nullptr, "open", data->msi_path.c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
            data->launched = true;
            EndDialog(dlg, IDOK);
        } else {
            if (!data->cancel_flag.load()) {
                std::string msg = i18n::fmt(i18n::S::kUpdateDownloadFailed, "download error");
                MessageBoxA(dlg, msg.c_str(), i18n::tr(i18n::S::kError), MB_OK | MB_ICONERROR);
            }
            EndDialog(dlg, IDCANCEL);
        }
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_UP_CANCEL) {
            data->cancel_flag.store(true);
            EnableWindow(GetDlgItem(dlg, IDC_UP_CANCEL), FALSE);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        data->cancel_flag.store(true);
        return TRUE;
    }
    return FALSE;
}

static void ShowDownloadDialog(HWND parent, UpdateDlgData& data) {
    using S = i18n::S;
    DlgBuilder b;
    int W = 220, H = 70;
    b.Begin(i18n::tr(S::kUpdateDownloading), 0, 0, W, H,
        WS_POPUP | WS_CAPTION | DS_CENTER | DS_SETFONT);

    b.AddStatic(IDC_UP_STATUS, "", 0, 0, 0, 0);
    b.AddButton(IDC_UP_CANCEL, i18n::tr(S::kDlgBtnCancel), 0, 0, 48, 14);

    DialogBoxIndirectParamA(GetModuleHandle(nullptr), b.Build(), parent,
        UpdateDlgProc, reinterpret_cast<LPARAM>(&data));
}

bool ShowUpdateDialog(HWND parent, const update::UpdateInfo& info) {
    using S = i18n::S;

    std::string msg = i18n::fmt(S::kUpdateAvailableMsg,
        info.latest_version.c_str(), TENBOX_VERSION_STR, info.release_notes.c_str());

    auto title_w = i18n::to_wide(i18n::tr(S::kUpdateAvailableTitle));
    auto msg_w = i18n::to_wide(msg.c_str());
    auto btn_update_w = i18n::to_wide(i18n::tr(S::kUpdateNow));
    auto btn_skip_w = i18n::to_wide(i18n::tr(S::kUpdateSkip));

    TASKDIALOG_BUTTON buttons[] = {
        { IDYES, btn_update_w.c_str() },
        { IDNO,  btn_skip_w.c_str() },
    };

    TASKDIALOGCONFIG tdc{};
    tdc.cbSize = sizeof(tdc);
    tdc.hwndParent = parent;
    tdc.pszWindowTitle = title_w.c_str();
    tdc.pszMainIcon = TD_INFORMATION_ICON;
    tdc.pszContent = msg_w.c_str();
    tdc.cButtons = _countof(buttons);
    tdc.pButtons = buttons;
    tdc.nDefaultButton = IDYES;

    int clicked = 0;
    HRESULT hr = TaskDialogIndirect(&tdc, &clicked, nullptr, nullptr);
    if (FAILED(hr) || clicked != IDYES) {
        return false;
    }

    UpdateDlgData data;
    data.info = info;
    ShowDownloadDialog(parent, data);
    return data.launched;
}
