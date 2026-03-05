#include "manager/ui/win32_dialogs.h"
#include "manager/ui/dlg_builder.h"
#include "manager/i18n.h"
#include "manager/http_download.h"
#include "manager/update_checker.h"
#include "version.h"

#include <commctrl.h>
#include <shellapi.h>

#include <atomic>
#include <string>
#include <thread>

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
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    return i18n::wide_to_utf8(tmp) + "TenBox-" + version + ".msi";
}

static INT_PTR CALLBACK UpdateDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<UpdateDlgData*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<UpdateDlgData*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(data));

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
        SetWindowTextW(status, i18n::tr_w(i18n::S::kUpdateDownloading).c_str());
        y += line_h + margin / 2;

        HWND progress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
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
        std::string buf = i18n::fmt(i18n::S::kUpdateDownloadProgress, pct);
        SetDlgItemTextW(dlg, IDC_UP_STATUS, i18n::to_wide(buf).c_str());
        return TRUE;
    }

    case WM_UPDATE_DONE: {
        if (data->download_thread.joinable())
            data->download_thread.join();

        if (wp == 1) {
            data->download_ok = true;
            SetDlgItemTextW(dlg, IDC_UP_STATUS, i18n::tr_w(i18n::S::kUpdateInstalling).c_str());
            SHELLEXECUTEINFOW sei{sizeof(sei)};
            sei.lpVerb = L"open";
            sei.lpFile = i18n::to_wide(data->msi_path).c_str();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
            data->launched = true;
            EndDialog(dlg, IDOK);
        } else {
            if (!data->cancel_flag.load()) {
                std::string msg = i18n::fmt(i18n::S::kUpdateDownloadFailed, "download error");
                MessageBoxW(dlg, i18n::to_wide(msg).c_str(), i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
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

    DialogBoxIndirectParamW(GetModuleHandle(nullptr), b.Build(), parent,
        UpdateDlgProc, reinterpret_cast<LPARAM>(&data));
}

bool ShowUpdateDialog(HWND parent, const update::UpdateInfo& info) {
    using S = i18n::S;

    std::string msg = i18n::fmt(S::kUpdateAvailableMsg,
        info.latest_version.c_str(), TENBOX_VERSION_STR, info.release_notes.c_str());

    auto title_w = i18n::tr_w(S::kUpdateAvailableTitle);
    auto msg_w = i18n::to_wide(msg.c_str());
    auto btn_update_w = i18n::tr_w(S::kUpdateNow);
    auto btn_skip_w = i18n::tr_w(S::kUpdateSkip);

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
