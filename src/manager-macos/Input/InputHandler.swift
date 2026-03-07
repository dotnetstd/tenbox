import AppKit

// Maps macOS keyboard/mouse events to Linux evdev codes for VirtIO-input.
// The key mapping table covers standard US keyboard layout.

struct EvdevEvent {
    let type: UInt16    // EV_KEY, EV_REL, EV_ABS
    let code: UInt16
    let value: Int32    // 0 = release, 1 = press, 2 = repeat (for keys)
}

class InputHandler {
    static let EV_KEY: UInt16 = 0x01
    static let EV_REL: UInt16 = 0x02
    static let EV_ABS: UInt16 = 0x03

    // Linux evdev button codes
    static let BTN_LEFT: UInt16 = 0x110
    static let BTN_RIGHT: UInt16 = 0x111
    static let BTN_MIDDLE: UInt16 = 0x112

    // Linux evdev relative axis codes
    static let REL_X: UInt16 = 0x00
    static let REL_Y: UInt16 = 0x01
    static let REL_WHEEL: UInt16 = 0x08

    // Linux evdev absolute axis codes
    static let ABS_X: UInt16 = 0x00
    static let ABS_Y: UInt16 = 0x01

    var onEvent: ((EvdevEvent) -> Void)?

    func handleKeyDown(_ event: NSEvent) {
        guard let code = macKeyCodeToEvdev(event.keyCode) else { return }
        onEvent?(EvdevEvent(type: Self.EV_KEY, code: code, value: 1))
    }

    func handleKeyUp(_ event: NSEvent) {
        guard let code = macKeyCodeToEvdev(event.keyCode) else { return }
        onEvent?(EvdevEvent(type: Self.EV_KEY, code: code, value: 0))
    }

    func handleMouseDown(_ event: NSEvent) {
        let btn: UInt16
        switch event.buttonNumber {
        case 0: btn = Self.BTN_LEFT
        case 1: btn = Self.BTN_RIGHT
        case 2: btn = Self.BTN_MIDDLE
        default: return
        }
        onEvent?(EvdevEvent(type: Self.EV_KEY, code: btn, value: 1))
    }

    func handleMouseUp(_ event: NSEvent) {
        let btn: UInt16
        switch event.buttonNumber {
        case 0: btn = Self.BTN_LEFT
        case 1: btn = Self.BTN_RIGHT
        case 2: btn = Self.BTN_MIDDLE
        default: return
        }
        onEvent?(EvdevEvent(type: Self.EV_KEY, code: btn, value: 0))
    }

    func handleMouseMoved(_ event: NSEvent, viewSize: CGSize) {
        let x = Int32(event.locationInWindow.x / viewSize.width * 32767)
        let y = Int32((1.0 - event.locationInWindow.y / viewSize.height) * 32767)
        onEvent?(EvdevEvent(type: Self.EV_ABS, code: Self.ABS_X, value: x))
        onEvent?(EvdevEvent(type: Self.EV_ABS, code: Self.ABS_Y, value: y))
    }

    func handleScrollWheel(_ event: NSEvent) {
        let delta = Int32(event.scrollingDeltaY)
        if delta != 0 {
            onEvent?(EvdevEvent(type: Self.EV_REL, code: Self.REL_WHEEL, value: delta))
        }
    }

    // macOS virtual key codes to Linux evdev key codes
    private func macKeyCodeToEvdev(_ keyCode: UInt16) -> UInt16? {
        return Self.keyMap[keyCode]
    }

    private static let keyMap: [UInt16: UInt16] = [
        // Row: number keys
        0x12: 2,    // KEY_1
        0x13: 3,    // KEY_2
        0x14: 4,    // KEY_3
        0x15: 5,    // KEY_4
        0x17: 6,    // KEY_5
        0x16: 7,    // KEY_6
        0x1A: 8,    // KEY_7
        0x1C: 9,    // KEY_8
        0x19: 10,   // KEY_9
        0x1D: 11,   // KEY_0
        0x1B: 12,   // KEY_MINUS
        0x18: 13,   // KEY_EQUAL
        0x33: 14,   // KEY_BACKSPACE
        // Row: QWERTY
        0x30: 15,   // KEY_TAB
        0x0C: 16,   // KEY_Q
        0x0D: 17,   // KEY_W
        0x0E: 18,   // KEY_E
        0x0F: 19,   // KEY_R
        0x11: 20,   // KEY_T
        0x10: 21,   // KEY_Y
        0x20: 22,   // KEY_U
        0x22: 23,   // KEY_I
        0x1F: 24,   // KEY_O
        0x23: 25,   // KEY_P
        0x21: 26,   // KEY_LEFTBRACE
        0x1E: 27,   // KEY_RIGHTBRACE
        0x24: 28,   // KEY_ENTER
        // Row: ASDF
        0x00: 30,   // KEY_A
        0x01: 31,   // KEY_S
        0x02: 32,   // KEY_D
        0x03: 33,   // KEY_F
        0x05: 34,   // KEY_G
        0x04: 35,   // KEY_H
        0x26: 36,   // KEY_J
        0x28: 37,   // KEY_K
        0x25: 38,   // KEY_L
        0x29: 39,   // KEY_SEMICOLON
        0x27: 40,   // KEY_APOSTROPHE
        0x32: 41,   // KEY_GRAVE
        0x2A: 43,   // KEY_BACKSLASH
        // Row: ZXCV
        0x06: 44,   // KEY_Z
        0x07: 45,   // KEY_X
        0x08: 46,   // KEY_C
        0x09: 47,   // KEY_V
        0x0B: 48,   // KEY_B
        0x2D: 49,   // KEY_N
        0x2E: 50,   // KEY_M
        0x2B: 51,   // KEY_COMMA
        0x2F: 52,   // KEY_DOT
        0x2C: 53,   // KEY_SLASH
        // Modifiers & special
        0x38: 42,   // KEY_LEFTSHIFT
        0x3C: 54,   // KEY_RIGHTSHIFT
        0x3B: 29,   // KEY_LEFTCTRL
        0x3E: 97,   // KEY_RIGHTCTRL
        0x3A: 56,   // KEY_LEFTALT
        0x3D: 100,  // KEY_RIGHTALT
        0x37: 125,  // KEY_LEFTMETA (Command)
        0x36: 126,  // KEY_RIGHTMETA
        0x31: 57,   // KEY_SPACE
        0x39: 58,   // KEY_CAPSLOCK
        0x35: 1,    // KEY_ESC
        // Function keys
        0x7A: 59,   // KEY_F1
        0x78: 60,   // KEY_F2
        0x63: 61,   // KEY_F3
        0x76: 62,   // KEY_F4
        0x60: 63,   // KEY_F5
        0x61: 64,   // KEY_F6
        0x62: 65,   // KEY_F7
        0x64: 66,   // KEY_F8
        0x65: 67,   // KEY_F9
        0x6D: 68,   // KEY_F10
        0x67: 87,   // KEY_F11
        0x6F: 88,   // KEY_F12
        // Arrow keys
        0x7E: 103,  // KEY_UP
        0x7D: 108,  // KEY_DOWN
        0x7B: 105,  // KEY_LEFT
        0x7C: 106,  // KEY_RIGHT
        // Navigation
        0x73: 102,  // KEY_HOME
        0x77: 107,  // KEY_END
        0x74: 104,  // KEY_PAGEUP
        0x79: 109,  // KEY_PAGEDOWN
        0x75: 111,  // KEY_DELETE
    ]
}
