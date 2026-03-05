#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

#include "manager/i18n.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// VM configuration option constants shared across dialogs.
inline constexpr int kMemoryOptionsMb[] = {1024, 2048, 4096, 8192, 16384};
inline const char* kMemoryLabels[]      = {"1 GB", "2 GB", "4 GB", "8 GB", "16 GB"};
inline constexpr int kCpuOptions[]      = {1, 2, 4, 8, 16};
inline const char* kCpuLabels[]         = {"1", "2", "4", "8", "16"};
inline constexpr int kNumOptions        = 5;

inline int MemoryMbToIndex(int mb) {
    for (int i = 0; i < kNumOptions; ++i)
        if (kMemoryOptionsMb[i] >= mb) return i;
    return kNumOptions - 1;
}

inline int CpuCountToIndex(int count) {
    for (int i = 0; i < kNumOptions; ++i)
        if (kCpuOptions[i] >= count) return i;
    return kNumOptions - 1;
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
