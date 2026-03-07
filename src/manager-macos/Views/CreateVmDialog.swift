import SwiftUI

struct CreateVmDialog: View {
    @EnvironmentObject var appState: AppState
    @Environment(\.dismiss) private var dismiss

    @State private var name = ""
    @State private var kernelPath = ""
    @State private var initrdPath = ""
    @State private var diskPath = ""
    @State private var memoryMb: Int = 512
    @State private var cpuCount: Int = 2
    @State private var cmdline = ""
    @State private var netEnabled = true

    var body: some View {
        VStack(spacing: 0) {
            Text("Create New VM")
                .font(.title2)
                .fontWeight(.semibold)
                .padding()

            Form {
                Section("General") {
                    TextField("Name", text: $name)
                    Stepper("CPUs: \(cpuCount)", value: $cpuCount, in: 1...16)
                    Stepper("Memory: \(memoryMb) MB", value: $memoryMb, in: 64...16384, step: 128)
                }

                Section("Boot") {
                    HStack {
                        TextField("Kernel Image", text: $kernelPath)
                        Button("Browse...") { browseKernel() }
                    }
                    HStack {
                        TextField("Initrd (optional)", text: $initrdPath)
                        Button("Browse...") { browseInitrd() }
                    }
                    TextField("Kernel cmdline", text: $cmdline)
                        .font(.system(.body, design: .monospaced))
                }

                Section("Storage") {
                    HStack {
                        TextField("Disk Image", text: $diskPath)
                        Button("Browse...") { browseDisk() }
                    }
                }

                Section("Network") {
                    Toggle("Enable NAT Networking", isOn: $netEnabled)
                }
            }
            .formStyle(.grouped)
            .padding(.horizontal)

            HStack {
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button("Create") { createVm() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(name.isEmpty || kernelPath.isEmpty)
            }
            .padding()
        }
        .frame(width: 500, height: 450)
    }

    private func browseKernel() {
        if let url = showOpenPanel(title: "Select Kernel Image") {
            kernelPath = url.path
        }
    }

    private func browseInitrd() {
        if let url = showOpenPanel(title: "Select Initrd") {
            initrdPath = url.path
        }
    }

    private func browseDisk() {
        if let url = showOpenPanel(title: "Select Disk Image",
                                   allowedTypes: ["qcow2", "img", "raw"]) {
            diskPath = url.path
        }
    }

    private func showOpenPanel(title: String, allowedTypes: [String]? = nil) -> URL? {
        let panel = NSOpenPanel()
        panel.title = title
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK {
            return panel.url
        }
        return nil
    }

    private func createVm() {
        let config = VmCreateConfig(
            name: name,
            kernelPath: kernelPath,
            initrdPath: initrdPath,
            diskPath: diskPath,
            memoryMb: memoryMb,
            cpuCount: cpuCount,
            cmdline: cmdline,
            netEnabled: netEnabled
        )
        appState.createVm(config: config)
        dismiss()
    }
}
