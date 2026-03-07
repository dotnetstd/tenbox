import SwiftUI

struct ContentView: View {
    @EnvironmentObject var appState: AppState

    var body: some View {
        NavigationSplitView {
            VmListView()
        } detail: {
            if let vmId = appState.selectedVmId,
               let vm = appState.vms.first(where: { $0.id == vmId }) {
                VmDetailView(vm: vm)
            } else {
                Text("Select a VM")
                    .font(.title2)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                Button(action: { appState.showCreateVmDialog = true }) {
                    Label("New VM", systemImage: "plus.rectangle")
                }

                if let vmId = appState.selectedVmId,
                   let vm = appState.vms.first(where: { $0.id == vmId }) {
                    Divider()

                    if vm.state == .stopped || vm.state == .crashed {
                        Button(action: { appState.startVm(id: vmId) }) {
                            Label("Start", systemImage: "play.fill")
                        }
                    }

                    if vm.state == .running {
                        Button(action: { appState.shutdownVm(id: vmId) }) {
                            Label("Shutdown", systemImage: "power")
                        }

                        Button(action: { appState.stopVm(id: vmId) }) {
                            Label("Force Stop", systemImage: "stop.fill")
                        }

                        Button(action: { appState.rebootVm(id: vmId) }) {
                            Label("Reboot", systemImage: "arrow.clockwise")
                        }
                    }

                    Divider()

                    Button(role: .destructive, action: {
                        appState.deleteVm(id: vmId)
                    }) {
                        Label("Delete", systemImage: "trash")
                    }
                    .disabled(vm.state == .running)
                }
            }
        }
        .sheet(isPresented: $appState.showCreateVmDialog) {
            CreateVmDialog()
        }
    }
}
