import SwiftUI
import MetalKit

struct DisplayView: View {
    @ObservedObject var session: VmSession
    @StateObject private var viewModel = DisplayViewModel()

    var body: some View {
        ZStack {
            MetalDisplayViewWrapper(
                renderer: session.renderer,
                inputHandler: viewModel.inputHandler
            )
            .aspectRatio(session.displayAspect, contentMode: .fit)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Color.black)
            .background(GeometryReader { geo in
                Color.clear.onChange(of: geo.size) { _, newSize in
                    viewModel.displaySizeChanged(newSize, client: session.ipcClient)
                }
                .onAppear {
                    viewModel.displaySizeChanged(geo.size, client: session.ipcClient)
                }
            })
            .onAppear {
                viewModel.attach(to: session)
            }
            .onDisappear {
                viewModel.detach()
            }

            if !session.connected {
                VStack(spacing: 12) {
                    ProgressView()
                        .scaleEffect(1.5)
                    Text("Waiting for display...")
                        .foregroundStyle(.secondary)
                }
            }
        }
    }
}

class DisplayViewModel: ObservableObject {
    let inputHandler = InputHandler()

    private let clipboardHandler = ClipboardHandler()
    private var resizeTimer: Timer?
    private weak var session: VmSession?

    func attach(to session: VmSession) {
        self.session = session
        setupInputHandler(client: session.ipcClient)
        setupClipboard(client: session.ipcClient)
    }

    func detach() {
        resizeTimer?.invalidate()
        resizeTimer = nil
        clipboardHandler.stopMonitoring()
        session = nil
    }

    func displaySizeChanged(_ size: CGSize, client: IpcClientWrapper) {
        resizeTimer?.invalidate()
        resizeTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: false) { [weak client] _ in
            guard let client = client, client.isConnected else { return }
            var w = UInt32(size.width)
            let h = UInt32(size.height)
            w = (w + 7) & ~7
            if w > 0 && h > 0 {
                client.sendDisplaySetSize(width: w, height: h)
            }
        }
    }

    private func setupInputHandler(client: IpcClientWrapper) {
        inputHandler.onEvent = { [weak client] event in
            guard let client = client, client.isConnected else { return }
            switch event.type {
            case InputHandler.EV_KEY:
                if event.code >= InputHandler.BTN_LEFT {
                    client.sendPointer(x: 0, y: 0, buttons: event.value > 0 ? UInt32(event.code) : 0)
                } else {
                    client.sendKey(code: event.code, pressed: event.value != 0)
                }
            case InputHandler.EV_ABS:
                if event.code == InputHandler.ABS_X {
                    client.sendPointer(x: event.value, y: 0, buttons: 0)
                }
            case InputHandler.EV_REL:
                if event.code == InputHandler.REL_WHEEL {
                    client.sendScroll(delta: event.value)
                }
            default:
                break
            }
        }
    }

    private func setupClipboard(client: IpcClientWrapper) {
        client.onClipboardData = { [weak self] dataType, payload in
            self?.handleGuestClipboardData(dataType: dataType, payload: payload)
        }

        client.onClipboardGrab = { [weak client] types in
            guard let client = client else { return }
            for t in types {
                client.sendClipboardRequest(dataType: t)
            }
        }

        client.onClipboardRequest = { [weak self] dataType in
            self?.handleGuestClipboardRequest(dataType: dataType)
        }

        clipboardHandler.onHostClipboardChanged = { [weak client] data, mimeType in
            guard let client = client, client.isConnected else { return }
            let dataType = Self.mimeToDataType(mimeType)
            if dataType != 0 {
                client.sendClipboardGrab(types: [dataType])
                client.sendClipboardData(dataType: dataType, payload: data)
            }
        }
        clipboardHandler.startMonitoring()
    }

    private func handleGuestClipboardData(dataType: UInt32, payload: Data) {
        if let mime = Self.dataTypeToMime(dataType) {
            clipboardHandler.setGuestClipboard(data: payload, mimeType: mime)
        }
    }

    private func handleGuestClipboardRequest(dataType: UInt32) {
        let pasteboard = NSPasteboard.general
        var data: Data?
        switch dataType {
        case 1:
            if let str = pasteboard.string(forType: .string) {
                data = str.data(using: .utf8)
            }
        case 2:
            data = pasteboard.data(forType: .png)
        case 3:
            data = pasteboard.data(forType: .init("com.microsoft.bmp"))
        default:
            break
        }
        if let data = data, let client = session?.ipcClient {
            client.sendClipboardData(dataType: dataType, payload: data)
        }
    }

    static func mimeToDataType(_ mime: String) -> UInt32 {
        switch mime {
        case "text/plain", "text/plain;charset=utf-8", "UTF8_STRING": return 1
        case "image/png": return 2
        case "image/bmp": return 3
        default: return 0
        }
    }

    static func dataTypeToMime(_ dataType: UInt32) -> String? {
        switch dataType {
        case 1: return "text/plain;charset=utf-8"
        case 2: return "image/png"
        case 3: return "image/bmp"
        default: return nil
        }
    }
}

class InputMTKView: MTKView {
    var inputHandler: InputHandler?

    override var acceptsFirstResponder: Bool { true }

    override func becomeFirstResponder() -> Bool {
        return true
    }

    override func keyDown(with event: NSEvent) {
        inputHandler?.handleKeyDown(event)
    }

    override func keyUp(with event: NSEvent) {
        inputHandler?.handleKeyUp(event)
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        inputHandler?.handleMouseDown(event)
    }

    override func mouseUp(with event: NSEvent) {
        inputHandler?.handleMouseUp(event)
    }

    override func rightMouseDown(with event: NSEvent) {
        inputHandler?.handleMouseDown(event)
    }

    override func rightMouseUp(with event: NSEvent) {
        inputHandler?.handleMouseUp(event)
    }

    override func mouseMoved(with event: NSEvent) {
        inputHandler?.handleMouseMoved(event, viewSize: bounds.size)
    }

    override func mouseDragged(with event: NSEvent) {
        inputHandler?.handleMouseMoved(event, viewSize: bounds.size)
    }

    override func rightMouseDragged(with event: NSEvent) {
        inputHandler?.handleMouseMoved(event, viewSize: bounds.size)
    }

    override func scrollWheel(with event: NSEvent) {
        inputHandler?.handleScrollWheel(event)
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        for area in trackingAreas {
            removeTrackingArea(area)
        }
        let area = NSTrackingArea(
            rect: bounds,
            options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect, .mouseEnteredAndExited],
            owner: self,
            userInfo: nil
        )
        addTrackingArea(area)
    }
}

struct MetalDisplayViewWrapper: NSViewRepresentable {
    let renderer: MetalDisplayRenderer?
    let inputHandler: InputHandler?

    func makeNSView(context: Context) -> InputMTKView {
        let view = InputMTKView()
        view.inputHandler = inputHandler
        if let renderer = renderer {
            view.device = renderer.device
            view.colorPixelFormat = .bgra8Unorm
            view.isPaused = false
            view.enableSetNeedsDisplay = false
            view.preferredFramesPerSecond = 60
            view.delegate = renderer
        }
        return view
    }

    func updateNSView(_ nsView: InputMTKView, context: Context) {
        nsView.delegate = renderer
        nsView.inputHandler = inputHandler
    }
}
