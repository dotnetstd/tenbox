#include "manager/ui/port_forward_dialog.h"
#include "manager/manager_service.h"
#include "manager/i18n.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#include <array>
#include <cstdio>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace {

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
        AppendWord(0);
        AppendWord(0);
        AppendWideStr(title);
        AppendWord(9);
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

    void AddButton(int id, const char* text, int x, int y, int cx, int cy, DWORD style = 0) {
        AddItem(id, 0x0080, text, x, y, cx, cy,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | style);
    }

    void AddDefButton(int id, const char* text, int x, int y, int cx, int cy) {
        AddButton(id, text, x, y, cx, cy, BS_DEFPUSHBUTTON);
    }

    LPCDLGTEMPLATE Build() {
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
                 int x, int y, int cx, int cy, DWORD style) {
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
        AppendWord(0);
        ++item_count_;
    }
};

enum PfDlgId {
    IDC_PF_LIST   = 300,
    IDC_PF_ADD    = 301,
    IDC_PF_REMOVE = 302,
};

struct PfDlgData {
    ManagerService* mgr;
    std::string vm_id;
    HWND listview;
};

static void PfRefreshList(PfDlgData* data) {
    HWND lv = data->listview;
    ListView_DeleteAllItems(lv);

    auto forwards = data->mgr->GetPortForwards(data->vm_id);
    for (size_t i = 0; i < forwards.size(); ++i) {
        const auto& pf = forwards[i];
        wchar_t buf[32];
        swprintf_s(buf, L"%u", pf.host_port);

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = buf;
        int idx = static_cast<int>(SendMessageW(lv, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));

        swprintf_s(buf, L"%u", pf.guest_port);
        LVITEMW setitem{};
        setitem.iSubItem = 1;
        setitem.pszText = buf;
        SendMessageW(lv, LVM_SETITEMTEXTW, idx, reinterpret_cast<LPARAM>(&setitem));
    }
}

enum AddPfDlgId {
    IDC_APF_HOST_PORT  = 400,
    IDC_APF_GUEST_PORT = 401,
};

struct AddPfDlgData {
    ManagerService* mgr;
    std::string vm_id;
    bool added;
};

static INT_PTR CALLBACK AddPfDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<AddPfDlgData*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG:
        data = reinterpret_cast<AddPfDlgData*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(data));
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            wchar_t host_buf[16], guest_buf[16];
            GetDlgItemTextW(dlg, IDC_APF_HOST_PORT, host_buf, static_cast<int>(std::size(host_buf)));
            GetDlgItemTextW(dlg, IDC_APF_GUEST_PORT, guest_buf, static_cast<int>(std::size(guest_buf)));

            int host_port = _wtoi(host_buf);
            int guest_port = _wtoi(guest_buf);

            if (host_port <= 0 || host_port > 65535 || guest_port <= 0 || guest_port > 65535) {
                MessageBoxW(dlg, i18n::tr_w(i18n::S::kPfInvalidPort).c_str(),
                    i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONWARNING);
                return TRUE;
            }

            PortForward pf;
            pf.host_port = static_cast<uint16_t>(host_port);
            pf.guest_port = static_cast<uint16_t>(guest_port);

            std::string error;
            if (data->mgr->AddPortForward(data->vm_id, pf, &error)) {
                data->added = true;
                EndDialog(dlg, IDOK);
            } else {
                MessageBoxW(dlg, i18n::to_wide(error).c_str(),
                    i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
            }
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

static bool ShowAddPortForwardDialog(HWND parent, ManagerService& mgr, const std::string& vm_id) {
    using S = i18n::S;
    DlgBuilder b;
    int W = 180, H = 90;
    b.Begin(i18n::tr(S::kPfAddTitle), 0, 0, W, H,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER | DS_SETFONT);

    int lx = 8, lw = 60, ex = 72, ew = 60, y = 10, rh = 14, sp = 18;

    b.AddStatic(0, i18n::tr(S::kPfLabelHostPort), lx, y, lw, rh);
    b.AddEdit(IDC_APF_HOST_PORT, ex, y - 2, ew, rh, ES_NUMBER);
    y += sp;

    b.AddStatic(0, i18n::tr(S::kPfLabelGuestPort), lx, y, lw, rh);
    b.AddEdit(IDC_APF_GUEST_PORT, ex, y - 2, ew, rh, ES_NUMBER);
    y += sp + 8;

    int btn_w = 50, btn_h = 14, btn_gap = 8;
    int btn_x = W - btn_w * 2 - btn_gap - 8;
    b.AddDefButton(IDOK, i18n::tr(S::kDlgBtnSave), btn_x, y, btn_w, btn_h);
    b.AddButton(IDCANCEL, i18n::tr(S::kDlgBtnCancel), btn_x + btn_w + btn_gap, y, btn_w, btn_h);

    AddPfDlgData data{&mgr, vm_id, false};
    DialogBoxIndirectParamW(GetModuleHandle(nullptr), b.Build(), parent,
        AddPfDlgProc, reinterpret_cast<LPARAM>(&data));
    return data.added;
}

static INT_PTR CALLBACK PfDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<PfDlgData*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<PfDlgData*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(data));

        RECT rc;
        GetClientRect(dlg, &rc);
        RECT du = {0, 0, 48, 14};
        MapDialogRect(dlg, &du);
        int btn_w = du.right, btn_h = du.bottom;
        int gap = btn_h / 2, btn_gap = btn_h / 4;
        int list_w = rc.right - btn_w - gap * 3;
        int list_h = rc.bottom - gap * 2;

        HWND lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            gap, gap, list_w, list_h,
            dlg, reinterpret_cast<HMENU>(IDC_PF_LIST),
            GetModuleHandle(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(lv,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        std::wstring col_host = i18n::tr_w(i18n::S::kPfColHostPort);
        std::wstring col_guest = i18n::tr_w(i18n::S::kPfColGuestPort);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = (list_w - 4) / 2;
        col.pszText = col_host.data();
        ListView_InsertColumn(lv, 0, &col);
        col.pszText = col_guest.data();
        ListView_InsertColumn(lv, 1, &col);

        data->listview = lv;

        int btn_x = gap + list_w + gap;
        MoveWindow(GetDlgItem(dlg, IDC_PF_ADD), btn_x, gap, btn_w, btn_h, FALSE);
        MoveWindow(GetDlgItem(dlg, IDC_PF_REMOVE), btn_x, gap + btn_h + btn_gap, btn_w, btn_h, FALSE);

        PfRefreshList(data);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PF_ADD:
            if (ShowAddPortForwardDialog(dlg, *data->mgr, data->vm_id)) {
                PfRefreshList(data);
            }
            return TRUE;

        case IDC_PF_REMOVE: {
            int sel = ListView_GetNextItem(data->listview, -1, LVNI_SELECTED);
            if (sel < 0) {
                MessageBoxW(dlg, i18n::tr_w(i18n::S::kPfNoSelection).c_str(),
                    i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONWARNING);
                return TRUE;
            }

            wchar_t host_buf[16]{}, guest_buf[16]{};
            LVITEMW lvi_get{};
            lvi_get.iSubItem = 0;
            lvi_get.pszText = host_buf;
            lvi_get.cchTextMax = static_cast<int>(std::size(host_buf));
            SendMessageW(data->listview, LVM_GETITEMTEXTW, sel, reinterpret_cast<LPARAM>(&lvi_get));
            lvi_get.iSubItem = 1;
            lvi_get.pszText = guest_buf;
            lvi_get.cchTextMax = static_cast<int>(std::size(guest_buf));
            SendMessageW(data->listview, LVM_GETITEMTEXTW, sel, reinterpret_cast<LPARAM>(&lvi_get));

            uint16_t host_port = static_cast<uint16_t>(_wtoi(host_buf));
            uint16_t guest_port = static_cast<uint16_t>(_wtoi(guest_buf));

            std::string prompt = i18n::fmt(i18n::S::kPfConfirmRemoveMsg, host_port, guest_port);

            if (MessageBoxW(dlg, i18n::to_wide(prompt).c_str(),
                    i18n::tr_w(i18n::S::kPfConfirmRemoveTitle).c_str(),
                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
                std::string error;
                if (data->mgr->RemovePortForward(data->vm_id, host_port, &error)) {
                    PfRefreshList(data);
                } else {
                    MessageBoxW(dlg, i18n::to_wide(error).c_str(),
                        i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
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

}  // namespace

void ShowPortForwardsDialog(HWND parent, ManagerService& mgr, const std::string& vm_id) {
    using S = i18n::S;
    DlgBuilder b;
    int W = 300, H = 180;
    b.Begin(i18n::tr(S::kDlgPortForwards), 0, 0, W, H,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER | DS_SETFONT);

    int btn_h = 14, btn_w = 50;
    b.AddButton(IDC_PF_ADD, i18n::tr(S::kPfBtnAdd), 0, 0, btn_w, btn_h);
    b.AddButton(IDC_PF_REMOVE, i18n::tr(S::kPfBtnRemove), 0, 0, btn_w, btn_h);

    PfDlgData data{&mgr, vm_id, nullptr};
    DialogBoxIndirectParamW(GetModuleHandle(nullptr), b.Build(), parent,
        PfDlgProc, reinterpret_cast<LPARAM>(&data));
}
