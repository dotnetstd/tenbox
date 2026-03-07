#include "manager/ui/console_tab.h"
#include "manager/i18n.h"

#include <commctrl.h>

void ConsoleTab::Create(HWND parent, HINSTANCE hinst, HFONT mono_font, HFONT ui_font) {
    console_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kConsoleId)), hinst, nullptr);
    SendMessage(console_, EM_SETLIMITTEXT,
        static_cast<WPARAM>(kMaxLen * 2), 0);
    SendMessage(console_, WM_SETFONT,
        reinterpret_cast<WPARAM>(mono_font), FALSE);

    console_in_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kInputId)), hinst, nullptr);
    SendMessage(console_in_, WM_SETFONT,
        reinterpret_cast<WPARAM>(mono_font), FALSE);
    auto placeholder = i18n::tr_w(i18n::S::kConsolePlaceholder);
    SendMessageW(console_in_, EM_SETCUEBANNER, FALSE,
        reinterpret_cast<LPARAM>(placeholder.c_str()));

    send_btn_ = CreateWindowExW(0, L"BUTTON", i18n::tr_w(i18n::S::kSend).c_str(),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kSendBtnId)), hinst, nullptr);
    SendMessage(send_btn_, WM_SETFONT,
        reinterpret_cast<WPARAM>(ui_font), FALSE);

    SetWindowSubclass(console_in_, InputSubclass, 0, 0);
}

void ConsoleTab::Show(bool visible) {
    int cmd = visible ? SW_SHOW : SW_HIDE;
    ShowWindow(console_, cmd);
    ShowWindow(console_in_, cmd);
    ShowWindow(send_btn_, cmd);
}

void ConsoleTab::Layout(int px, int py, int pw, int ph) {
    int input_h = 24;
    int send_w = 60;
    int gap = 4;
    int console_h = ph - input_h - gap;
    if (console_h < 20) console_h = 20;

    MoveWindow(console_, px, py, pw, console_h, TRUE);
    int input_y = py + console_h + gap;
    MoveWindow(console_in_, px, input_y, pw - send_w - gap, input_h, TRUE);
    MoveWindow(send_btn_, px + pw - send_w, input_y, send_w, input_h, TRUE);
}

void ConsoleTab::SetEnabled(bool enabled) {
    EnableWindow(console_in_, enabled);
    EnableWindow(send_btn_, enabled);
}

void ConsoleTab::SetText(const char* text) {
    SetWindowTextW(console_, i18n::to_wide(text).c_str());
}

void ConsoleTab::AppendText(const std::string& text) {
    int ctl_len = GetWindowTextLengthW(console_);
    if (ctl_len > static_cast<int>(kTrimAt)) {
        int cut = ctl_len / 2;
        SendMessageW(console_, EM_SETSEL, 0, cut);
        SendMessageW(console_, EM_REPLACESEL, FALSE,
            reinterpret_cast<LPARAM>(L""));
        ctl_len = GetWindowTextLengthW(console_);
    }
    SendMessageW(console_, EM_SETSEL, ctl_len, ctl_len);
    std::wstring wtext = i18n::to_wide(text);
    SendMessageW(console_, EM_REPLACESEL, FALSE,
        reinterpret_cast<LPARAM>(wtext.c_str()));
}

std::string ConsoleTab::FilterAndAppend(TextState& state, const std::string& raw) {
    std::string added;
    for (unsigned char ch : raw) {
        switch (state.ansi) {
        case TextState::AnsiPhase::kNormal:
            if (ch == 0x1b) { state.ansi = TextState::AnsiPhase::kEsc; break; }
            if (ch == '\n') { state.text += "\r\n"; added += "\r\n"; break; }
            if (ch == '\t') { state.text.push_back('\t'); added.push_back('\t'); break; }
            if (ch >= 0x20 && ch <= 0x7e) { state.text.push_back(ch); added.push_back(ch); break; }
            break;
        case TextState::AnsiPhase::kEsc:
            state.ansi = (ch == '[') ? TextState::AnsiPhase::kCsi : TextState::AnsiPhase::kNormal;
            break;
        case TextState::AnsiPhase::kCsi:
            if (ch >= 0x40 && ch <= 0x7e) state.ansi = TextState::AnsiPhase::kNormal;
            break;
        }
    }
    if (state.text.size() > kMaxLen) {
        state.text.erase(0, state.text.size() - kMaxLen);
    }
    return added;
}

void ConsoleTab::ResetState(TextState& state) {
    state.text.clear();
    state.ansi = TextState::AnsiPhase::kNormal;
}

LRESULT CALLBACK ConsoleTab::InputSubclass(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR /*id*/, DWORD_PTR /*ref*/)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessage(GetParent(hwnd), WM_COMMAND,
            MAKEWPARAM(kSendBtnId, BN_CLICKED), 0);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
