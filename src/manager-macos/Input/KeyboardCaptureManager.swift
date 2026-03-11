import AppKit
import ApplicationServices

struct KeyboardCapturePermissions {
    let inputMonitoringGranted: Bool
    let accessibilityGranted: Bool

    var canAttemptEventTap: Bool {
        accessibilityGranted
    }

    var missingPermissionNames: [String] {
        var missing: [String] = []
        if !inputMonitoringGranted {
            missing.append("Input Monitoring")
        }
        if !accessibilityGranted {
            missing.append("Accessibility")
        }
        return missing
    }
}

enum KeyboardCaptureMode: Equatable {
    case inactive
    case localOnly
    case fullCapture
}

final class KeyboardCaptureManager {
    var onKeyDown: ((UInt16) -> Void)?
    var onKeyUp: ((UInt16) -> Void)?
    var onFlagsChanged: ((UInt16, NSEvent.ModifierFlags) -> Void)?
    var onModeChanged: ((KeyboardCaptureMode) -> Void)?
    var onCaptureEnded: (() -> Void)?

    private(set) var mode: KeyboardCaptureMode = .inactive {
        didSet {
            guard oldValue != mode else { return }
            onModeChanged?(mode)
        }
    }

    private var eventTap: CFMachPort?
    private var runLoopSource: CFRunLoopSource?

    var isCaptureActive: Bool {
        mode != .inactive
    }

    var handlesKeyboardLocally: Bool {
        mode != .fullCapture
    }

    /// Called by the NSView when it receives a keyboard event while in fullCapture mode.
    /// If the event tap were truly intercepting, the NSView would never see the event.
    /// This means the tap is silently broken (common with non-bundled debug builds),
    /// so we fall back to localOnly processing.
    func demoteToLocalOnlyIfNeeded() {
        guard mode == .fullCapture else { return }
        uninstallEventTap()
        mode = .localOnly
    }

    func beginCapture() {
        guard mode == .inactive else { return }
        if installEventTap() {
            mode = .fullCapture
            return
        }

        mode = .localOnly
    }

    func endCapture() {
        guard mode != .inactive else { return }
        uninstallEventTap()
        mode = .inactive
        onCaptureEnded?()
    }

    func isReleaseGesture(keyCode: UInt16, modifierFlags: NSEvent.ModifierFlags) -> Bool {
        keyCode == Self.releaseKeyCode && modifierFlags.contains(.option)
    }

    static func currentPermissions() -> KeyboardCapturePermissions {
        KeyboardCapturePermissions(
            inputMonitoringGranted: CGPreflightListenEventAccess(),
            accessibilityGranted: AXIsProcessTrusted()
        )
    }

    static func requestFullCapturePermissions() {
        let options = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
        _ = AXIsProcessTrustedWithOptions(options)
    }

    deinit {
        uninstallEventTap()
    }

    private func installEventTap() -> Bool {
        uninstallEventTap()

        guard Self.currentPermissions().canAttemptEventTap else {
            return false
        }

        let refcon = UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque())
        for location in [CGEventTapLocation.cghidEventTap, .cgSessionEventTap] {
            guard let tap = CGEvent.tapCreate(
                tap: location,
                place: .headInsertEventTap,
                options: .defaultTap,
                eventsOfInterest: Self.tapEventMask,
                callback: Self.eventTapCallback,
                userInfo: refcon
            ) else {
                continue
            }

            guard let source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0) else {
                CFMachPortInvalidate(tap)
                continue
            }

            eventTap = tap
            runLoopSource = source
            CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
            CGEvent.tapEnable(tap: tap, enable: true)
            return true
        }

        return false
    }

    private func uninstallEventTap() {
        if let source = runLoopSource {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), source, .commonModes)
            runLoopSource = nil
        }
        if let tap = eventTap {
            CFMachPortInvalidate(tap)
            eventTap = nil
        }
    }

    private static let releaseKeyCode: UInt16 = 0x3D
    private static let tapEventMask: CGEventMask =
        (CGEventMask(1) << CGEventType.keyDown.rawValue) |
        (CGEventMask(1) << CGEventType.keyUp.rawValue) |
        (CGEventMask(1) << CGEventType.flagsChanged.rawValue)

    private static let eventTapCallback: CGEventTapCallBack = { _, type, event, refcon in
        guard let refcon else {
            return Unmanaged.passUnretained(event)
        }
        let manager = Unmanaged<KeyboardCaptureManager>.fromOpaque(refcon).takeUnretainedValue()
        return manager.handleTapEvent(type: type, event: event)
    }

    private func handleTapEvent(type: CGEventType, event: CGEvent) -> Unmanaged<CGEvent>? {
        switch type {
        case .tapDisabledByTimeout, .tapDisabledByUserInput:
            if let tap = eventTap {
                CGEvent.tapEnable(tap: tap, enable: true)
            }
            return Unmanaged.passUnretained(event)
        case .keyDown, .keyUp, .flagsChanged:
            break
        default:
            return Unmanaged.passUnretained(event)
        }

        guard mode == .fullCapture else {
            return Unmanaged.passUnretained(event)
        }

        let keyCode = UInt16(event.getIntegerValueField(.keyboardEventKeycode))
        let modifierFlags = NSEvent.ModifierFlags(rawValue: UInt(event.flags.rawValue))

        if keyCode == Self.releaseKeyCode {
            if modifierFlags.contains(NSEvent.ModifierFlags.option) {
                DispatchQueue.main.async { [weak self] in
                    self?.endCapture()
                }
            }
            return nil
        }

        switch type {
        case .keyDown:
            onKeyDown?(keyCode)
        case .keyUp:
            onKeyUp?(keyCode)
        case .flagsChanged:
            onFlagsChanged?(keyCode, modifierFlags)
        default:
            break
        }

        return nil
    }
}
