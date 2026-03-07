import SwiftUI

struct ConsoleView: View {
    @ObservedObject var session: VmSession

    @State private var inputText = ""

    var body: some View {
        VStack(spacing: 0) {
            ScrollViewReader { proxy in
                ScrollView {
                    Text(session.consoleText)
                        .font(.system(.body, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(8)
                        .textSelection(.enabled)
                        .id("console-bottom")
                }
                .background(Color(.textBackgroundColor))
                .onChange(of: session.consoleText) {
                    proxy.scrollTo("console-bottom", anchor: .bottom)
                }
            }

            Divider()

            HStack {
                TextField("Type command...", text: $inputText)
                    .textFieldStyle(.roundedBorder)
                    .font(.system(.body, design: .monospaced))
                    .onSubmit {
                        sendInput()
                    }
                Button("Send") {
                    sendInput()
                }
                .keyboardShortcut(.return, modifiers: [])
            }
            .padding(8)
        }
    }

    private func sendInput() {
        guard !inputText.isEmpty else { return }
        let text = inputText + "\n"
        inputText = ""
        session.sendConsoleInput(text)
    }
}
