import SwiftUI

struct ContentView: View {
    @EnvironmentObject var appState: AppState
    @State private var columnVisibility = NavigationSplitViewVisibility.all
    @State private var showDeleteConfirm = false
    @State private var showForceStopConfirm = false
    @State private var showSharedFoldersSheet = false
    @State private var showPortForwardsSheet = false

    var body: some View {
        NavigationSplitView(columnVisibility: $columnVisibility) {
            VmListView()
        } detail: {
            if let vmId = appState.selectedVmId,
               let vm = appState.vms.first(where: { $0.id == vmId }) {
                VmDetailView(vm: vm, appState: appState)
                    .id(vmId)
            } else {
                Text("Select a VM")
                    .font(.title2)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .onChange(of: columnVisibility, perform: { newValue in
            if newValue != .all {
                columnVisibility = .all
            }
        })
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                Button(action: { appState.showCreateVmDialog = true }) {
                    Label("New VM", systemImage: "plus.rectangle")
                }
                .help("Create a new virtual machine")

                if let vmId = appState.selectedVmId,
                   let vm = appState.vms.first(where: { $0.id == vmId }) {
                    Button(action: { appState.showEditVmDialog = true }) {
                        Label("Edit", systemImage: "pencil")
                    }
                    .disabled(vm.state == .running)
                    .help("Edit VM settings")

                    Button(role: .destructive, action: {
                        showDeleteConfirm = true
                    }) {
                        Label("Delete", systemImage: "trash")
                    }
                    .disabled(vm.state == .running)
                    .help("Delete this VM")
                }
            }

            if let vmId = appState.selectedVmId,
               let vm = appState.vms.first(where: { $0.id == vmId }) {
                ToolbarItem(placement: .primaryAction) { Divider() }

                ToolbarItemGroup(placement: .primaryAction) {
                    if vm.state == .stopped || vm.state == .crashed {
                        Button(action: { appState.requestStartVm(id: vmId) }) {
                            Label("Start", systemImage: "play.fill")
                        }
                        .help("Start VM")
                    }

                    if vm.state == .running {
                        Button(action: { showForceStopConfirm = true }) {
                            Label("Force Stop", systemImage: "stop.fill")
                        }
                        .help("Force stop VM immediately")

                        Button(action: { appState.rebootVm(id: vmId) }) {
                            Label("Reboot", systemImage: "arrow.clockwise")
                        }
                        .help("Reboot VM")

                        Button(action: { appState.shutdownVm(id: vmId) }) {
                            Label("Shutdown", systemImage: "power")
                        }
                        .help("Gracefully shut down VM")
                    }
                }

                if vm.state == .running {
                    ToolbarItem(placement: .primaryAction) { Divider() }

                    ToolbarItemGroup(placement: .primaryAction) {
                        Button(action: {
                            appState.setDisplayScale(vm.displayScale == 1 ? 2 : 1, forVm: vmId)
                        }) {
                            Label(vm.displayScale == 2 ? "Display 1x" : "Display 2x",
                                  systemImage: vm.displayScale == 2 ? "minus.magnifyingglass" : "plus.magnifyingglass")
                        }
                        .help(vm.displayScale == 2 ? "Switch to 1x display scale" : "Switch to 2x Retina display scale")
                    }
                }

                ToolbarItem(placement: .primaryAction) { Divider() }

                ToolbarItemGroup(placement: .primaryAction) {
                    Button(action: { showSharedFoldersSheet = true }) {
                        ToolbarBadgeLabel(
                            title: "Shared Folders",
                            systemImage: "folder",
                            count: vm.sharedFolders.count
                        )
                    }
                    .help("Manage shared folders")

                    Button(action: { showPortForwardsSheet = true }) {
                        ToolbarBadgeLabel(
                            title: "Port Forwards",
                            systemImage: "network.badge.shield.half.filled",
                            count: vm.portForwards.count
                        )
                    }
                    .help("Manage port forwards")
                }
            }
        }
        .sheet(isPresented: $appState.showCreateVmDialog) {
            CreateVmDialog()
        }
        .sheet(isPresented: $appState.showEditVmDialog) {
            if let vmId = appState.selectedVmId,
               let vm = appState.vms.first(where: { $0.id == vmId }) {
                EditVmDialog(vm: vm)
            }
        }
        .sheet(isPresented: $showSharedFoldersSheet) {
            if let vmId = appState.selectedVmId {
                SharedFoldersSheet(vmId: vmId)
            }
        }
        .sheet(isPresented: $showPortForwardsSheet) {
            if let vmId = appState.selectedVmId {
                PortForwardsSheet(vmId: vmId)
            }
        }
        .alert("Delete VM", isPresented: $showDeleteConfirm) {
            Button("Cancel", role: .cancel) {}
            Button("Delete", role: .destructive) {
                if let vmId = appState.selectedVmId {
                    appState.deleteVm(id: vmId)
                }
            }
        } message: {
            if let vmId = appState.selectedVmId,
               let vm = appState.vms.first(where: { $0.id == vmId }) {
                Text("Are you sure you want to delete \"\(vm.name)\"? This action cannot be undone.")
            }
        }
        .alert("Force Stop VM", isPresented: $showForceStopConfirm) {
            Button("Cancel", role: .cancel) {}
            Button("Force Stop", role: .destructive) {
                if let vmId = appState.selectedVmId {
                    appState.stopVm(id: vmId)
                }
            }
        } message: {
            if let vmId = appState.selectedVmId,
               let vm = appState.vms.first(where: { $0.id == vmId }) {
                Text("Are you sure you want to force stop \"\(vm.name)\"? Unsaved data may be lost.")
            }
        }
        .alert("Enable Full Keyboard Capture?", isPresented: $appState.showKeyboardCapturePermissionAlert) {
            Button("Request Permissions") {
                appState.requestKeyboardCapturePermissions()
            }
            .keyboardShortcut(.defaultAction)
            Button("Start Without It") {
                appState.startPendingVmWithoutPermissionPrompt()
            }
            Button("Cancel", role: .cancel) {
                appState.dismissKeyboardCapturePermissionPrompt()
            }
        } message: {
            keyboardCapturePermissionMessage
        }
    }

    private var keyboardCapturePermissionMessage: Text {
        return Text("Full keyboard capture needs ") +
            Text("Accessibility").bold() +
            Text(" permission.\n\nYou can continue without full capture, or request permissions now.")
    }
}

private struct ToolbarBadgeLabel: View {
    let title: String
    let systemImage: String
    let count: Int

    var body: some View {
        Label {
            Text(title)
        } icon: {
            ZStack(alignment: .topTrailing) {
                Image(systemName: systemImage)
                if count > 0 {
                    Text("\(count)")
                        .font(.system(size: 8, weight: .bold))
                        .foregroundStyle(.white)
                        .frame(minWidth: 12, minHeight: 12)
                        .background(.secondary, in: Circle())
                        .offset(x: 4, y: -4)
                }
            }
        }
    }
}
