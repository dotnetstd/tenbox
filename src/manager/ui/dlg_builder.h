#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>

#include "manager/i18n.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

inline constexpr int kDefaultMemoryGb = 4;
inline constexpr int kDefaultVcpus = 4;

inline int GetHostMemoryGb() {
    MEMORYSTATUSEX statex{};
    statex.dwLength = sizeof(statex);
    if (GlobalMemoryStatusEx(&statex))
        return static_cast<int>(statex.ullTotalPhys / (1024ULL * 1024 * 1024));
    return 16;
}

inline int GetHostLogicalCpus() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? static_cast<int>(si.dwNumberOfProcessors) : 4;
}

inline std::wstring FormatMemoryLabel(int gb) {
    return i18n::to_wide(std::to_string(gb) + " GB");
}

inline std::wstring FormatCpuLabel(int count) {
    return i18n::to_wide(std::to_string(count));
}

struct SliderRowLayout {
    int label_w;
    int edit_x;
    int slider_w;
    int slider_h;
    int value_w;
    int form_row_h;
    int label_y_off;
};

inline SliderRowLayout CalcSliderRowLayout(int client_w, int margin, int btn_h,
                                           std::function<int(int)> scale_px) {
    SliderRowLayout l{};
    l.label_w    = scale_px(78);
    l.edit_x     = margin + l.label_w + scale_px(8);
    l.value_w    = scale_px(60);
    l.slider_w   = client_w - l.edit_x - margin - l.value_w - scale_px(4);
    l.slider_h   = scale_px(26);
    l.form_row_h = (btn_h > scale_px(32)) ? btn_h : scale_px(32);
    l.label_y_off = (l.form_row_h - scale_px(20)) / 2;
    return l;
}

inline void CreateSliderRow(HWND parent, const SliderRowLayout& l, int margin,
                            int y, int label_id, const wchar_t* label_text,
                            int slider_id, int value_id,
                            std::function<int(int)> scale_px) {
    CreateWindowExW(0, L"STATIC", label_text,
        WS_CHILD | SS_LEFT, margin, y + l.label_y_off, l.label_w, scale_px(20),
        parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(label_id)),
        nullptr, nullptr);
    CreateWindowExW(0, TRACKBAR_CLASSW, L"",
        WS_CHILD | TBS_HORZ | TBS_AUTOTICKS,
        l.edit_x, y, l.slider_w, l.slider_h,
        parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(slider_id)),
        nullptr, nullptr);
    CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | SS_CENTER,
        l.edit_x + l.slider_w + scale_px(4), y + l.label_y_off, l.value_w, scale_px(20),
        parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(value_id)),
        nullptr, nullptr);
}

inline void InitSlider(HWND dlg, int slider_id, int value_id,
                       int min_val, int max_val, int cur_val, bool is_memory) {
    HWND slider = GetDlgItem(dlg, slider_id);
    SendMessage(slider, TBM_SETRANGE, TRUE, MAKELPARAM(min_val, max_val));
    SendMessage(slider, TBM_SETPOS, TRUE, cur_val);
    HWND value_label = GetDlgItem(dlg, value_id);
    SetWindowTextW(value_label, is_memory
        ? FormatMemoryLabel(cur_val).c_str()
        : FormatCpuLabel(cur_val).c_str());
}

inline void UpdateSliderLabel(HWND dlg, int slider_id, int value_id, bool is_memory) {
    HWND slider = GetDlgItem(dlg, slider_id);
    int val = static_cast<int>(SendMessage(slider, TBM_GETPOS, 0, 0));
    SetWindowTextW(GetDlgItem(dlg, value_id), is_memory
        ? FormatMemoryLabel(val).c_str()
        : FormatCpuLabel(val).c_str());
}

inline bool HandleSliderScroll(HWND dlg, LPARAM lp,
                               int mem_slider_id, int mem_value_id,
                               int cpu_slider_id, int cpu_value_id) {
    HWND slider = reinterpret_cast<HWND>(lp);
    if (slider == GetDlgItem(dlg, mem_slider_id)) {
        UpdateSliderLabel(dlg, mem_slider_id, mem_value_id, true);
        return true;
    }
    if (slider == GetDlgItem(dlg, cpu_slider_id)) {
        UpdateSliderLabel(dlg, cpu_slider_id, cpu_value_id, false);
        return true;
    }
    return false;
}

inline std::string GetDlgText(HWND dlg, int id) {
    wchar_t buf[1024]{};
    GetDlgItemTextW(dlg, id, buf, static_cast<int>(std::size(buf)));
    return i18n::wide_to_utf8(buf);
}

inline std::string BrowseForFile(HWND owner, const char* filter, const char* current_path) {
    wchar_t file_buf[MAX_PATH]{};
    if (current_path && *current_path)
        MultiByteToWideChar(CP_UTF8, 0, current_path, -1, file_buf, MAX_PATH);

    std::wstring init_dir_w;
    if (current_path && *current_path) {
        namespace fs = std::filesystem;
        auto u8dir = fs::path(current_path).parent_path().u8string();
        std::string init_dir(reinterpret_cast<const char*>(u8dir.data()), u8dir.size());
        init_dir_w = i18n::to_wide(init_dir);
    }

    // Convert filter (null-separated pairs: "Desc\0Pattern\0") to wide
    std::wstring filter_w;
    if (filter && *filter) {
        const char* p = filter;
        while (*p) {
            size_t len = std::strlen(p);
            filter_w += i18n::to_wide(std::string(p, len));
            filter_w += L'\0';
            p += len + 1;
        }
        filter_w += L'\0';
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = owner;
    ofn.lpstrFilter  = filter_w.empty() ? nullptr : filter_w.c_str();
    ofn.lpstrFile    = file_buf;
    ofn.nMaxFile     = MAX_PATH;
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!init_dir_w.empty())
        ofn.lpstrInitialDir = init_dir_w.c_str();

    if (GetOpenFileNameW(&ofn))
        return i18n::wide_to_utf8(file_buf);
    return {};
}

inline int CALLBACK BrowseFolderCallback(HWND hwnd, UINT msg, LPARAM, LPARAM data) {
    if (msg == BFFM_INITIALIZED && data)
        SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

inline std::string BrowseForFolder(HWND owner, const char* title, const char* current_path) {
    std::wstring init_dir_w;
    if (current_path && *current_path) {
        namespace fs = std::filesystem;
        fs::path p(current_path);
        std::string init_dir;
        if (fs::is_directory(p)) {
            auto u8 = p.u8string();
            init_dir.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
        } else if (p.has_parent_path()) {
            auto u8 = p.parent_path().u8string();
            init_dir.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
        }
        if (!init_dir.empty())
            init_dir_w = i18n::to_wide(init_dir);
    }

    std::wstring title_w = title ? i18n::to_wide(title) : std::wstring();
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = title_w.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (!init_dir_w.empty()) {
        bi.lpfn = BrowseFolderCallback;
        bi.lParam = reinterpret_cast<LPARAM>(init_dir_w.c_str());
    }

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path_buf[MAX_PATH]{};
        SHGetPathFromIDListW(pidl, path_buf);
        CoTaskMemFree(pidl);
        return i18n::wide_to_utf8(path_buf);
    }
    return {};
}

inline std::string ExeDirectory() {
    wchar_t buf[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    std::string dir = i18n::wide_to_utf8(buf);
    auto sep = dir.find_last_of("\\/");
    return (sep != std::string::npos) ? dir.substr(0, sep + 1) : dir;
}

inline std::string FindShareFile(const std::string& exe_dir, const char* filename) {
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

// Center a dialog window over its parent (or owner) instead of the screen.
inline void CenterDialogToParent(HWND dlg) {
    HWND parent = GetParent(dlg);
    if (!parent) parent = GetWindow(dlg, GW_OWNER);
    if (!parent) return;
    RECT pr, dr;
    GetWindowRect(parent, &pr);
    GetWindowRect(dlg, &dr);
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    int x = pr.left + ((pr.right - pr.left) - dw) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - dh) / 2;
    SetWindowPos(dlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// In-memory dialog template builder.
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
