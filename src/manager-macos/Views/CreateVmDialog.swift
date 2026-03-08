import SwiftUI

private let hostMaxCpus = ProcessInfo.processInfo.activeProcessorCount
private let hostMaxMemoryGb = max(1, Int(ProcessInfo.processInfo.physicalMemory / (1024 * 1024 * 1024)))

struct CreateVmDialog: View {
    @EnvironmentObject var appState: AppState
    @Environment(\.dismiss) private var dismiss

    @State private var name = ""
    @State private var kernelPath = ""
    @State private var initrdPath = ""
    @State private var diskPath = ""
    @State private var memoryGb: Int = min(4, hostMaxMemoryGb)
    @State private var cpuCount: Int = min(4, hostMaxCpus)

    var body: some View {
        VStack(spacing: 0) {
            Text("Create New VM")
                .font(.title2)
                .fontWeight(.semibold)
                .padding()

            Form {
                Section("General") {
                    TextField("Name", text: $name)
                    Stepper("CPUs: \(cpuCount)", value: $cpuCount, in: 1...hostMaxCpus)
                    Stepper("Memory: \(memoryGb) GB", value: $memoryGb, in: 1...hostMaxMemoryGb)
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
                }

                Section("Storage") {
                    HStack {
                        TextField("Disk Image", text: $diskPath)
                        Button("Browse...") { browseDisk() }
                    }
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
            memoryMb: memoryGb * 1024,
            cpuCount: cpuCount,
            netEnabled: true
        )
        appState.createVm(config: config)
        dismiss()
    }
}

struct EditVmDialog: View {
    @EnvironmentObject var appState: AppState
    @Environment(\.dismiss) private var dismiss

    let vm: VmInfo

    @State private var name: String
    @State private var memoryGb: Int
    @State private var cpuCount: Int

    init(vm: VmInfo) {
        self.vm = vm
        _name = State(initialValue: vm.name)
        _memoryGb = State(initialValue: max(1, vm.memoryMb / 1024))
        _cpuCount = State(initialValue: vm.cpuCount)
    }

    var body: some View {
        VStack(spacing: 0) {
            Text("Edit VM")
                .font(.title2)
                .fontWeight(.semibold)
                .padding()

            Form {
                Section("General") {
                    TextField("Name", text: $name)
                    Stepper("CPUs: \(cpuCount)", value: $cpuCount, in: 1...hostMaxCpus)
                    Stepper("Memory: \(memoryGb) GB", value: $memoryGb, in: 1...hostMaxMemoryGb)
                }

                Section("Paths (read-only)") {
                    LabeledContent("Kernel") {
                        Text(vm.kernelPath)
                            .lineLimit(1)
                            .truncationMode(.middle)
                            .foregroundStyle(.secondary)
                    }
                    LabeledContent("Disk") {
                        Text(vm.diskPath.isEmpty ? "None" : vm.diskPath)
                            .lineLimit(1)
                            .truncationMode(.middle)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .formStyle(.grouped)
            .padding(.horizontal)

            HStack {
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button("Save") { saveVm() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(name.isEmpty)
            }
            .padding()
        }
        .frame(width: 450, height: 380)
    }

    private func saveVm() {
        appState.editVm(
            id: vm.id,
            name: name,
            memoryMb: memoryGb * 1024,
            cpuCount: cpuCount,
            netEnabled: true
        )
        dismiss()
    }
}
