import AppKit

// Clipboard synchronization between host macOS and guest VM.
// Uses NSPasteboard to monitor host clipboard changes, and
// forwards guest clipboard data (received via SPICE vdagent) to the host.

class ClipboardHandler {
    private var pollingTimer: Timer?
    private var lastChangeCount: Int = 0
    private var appObservers: [NSObjectProtocol] = []

    var onHostClipboardChanged: ((Data, String) -> Void)?

    func startMonitoring() {
        lastChangeCount = NSPasteboard.general.changeCount
        startTimer()

        let nc = NotificationCenter.default
        appObservers.append(nc.addObserver(
            forName: NSApplication.didBecomeActiveNotification, object: nil, queue: .main
        ) { [weak self] _ in
            self?.startTimer()
        })
        appObservers.append(nc.addObserver(
            forName: NSApplication.didResignActiveNotification, object: nil, queue: .main
        ) { [weak self] _ in
            self?.stopTimer()
        })
    }

    func stopMonitoring() {
        stopTimer()
        let nc = NotificationCenter.default
        for obs in appObservers { nc.removeObserver(obs) }
        appObservers.removeAll()
    }

    private     func startTimer() {
        guard pollingTimer == nil else { return }
        lastChangeCount = NSPasteboard.general.changeCount
        pollingTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            self?.checkForChanges()
        }
    }

    private func stopTimer() {
        pollingTimer?.invalidate()
        pollingTimer = nil
    }

    private func checkForChanges() {
        let pasteboard = NSPasteboard.general
        let current = pasteboard.changeCount
        guard current != lastChangeCount else { return }
        lastChangeCount = current

        if let string = pasteboard.string(forType: .string) {
            if let data = string.data(using: .utf8) {
                onHostClipboardChanged?(data, "text/plain;charset=utf-8")
            }
        } else if let pngData = pasteboard.data(forType: .png) {
            onHostClipboardChanged?(pngData, "image/png")
        } else if let bmpData = pasteboard.data(forType: .init("com.microsoft.bmp")) {
            onHostClipboardChanged?(bmpData, "image/bmp")
        }
    }

    func setGuestClipboard(data: Data, mimeType: String) {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()

        switch mimeType {
        case "text/plain", "text/plain;charset=utf-8", "UTF8_STRING":
            if let string = String(data: data, encoding: .utf8) {
                pasteboard.setString(string, forType: .string)
            }
        case "image/png":
            pasteboard.setData(data, forType: .png)
        case "image/bmp":
            pasteboard.setData(data, forType: .init("com.microsoft.bmp"))
        default:
            NSLog("ClipboardHandler: unsupported MIME type: %@", mimeType)
        }

        lastChangeCount = pasteboard.changeCount
    }

    deinit {
        stopMonitoring()
    }
}
