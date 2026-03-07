import SwiftUI

struct VmListView: View {
    @EnvironmentObject var appState: AppState

    var body: some View {
        List(appState.vms, selection: $appState.selectedVmId) { vm in
            VmRowView(vm: vm)
                .tag(vm.id)
        }
        .listStyle(.sidebar)
        .navigationTitle("TenBox")
    }
}

struct VmRowView: View {
    let vm: VmInfo

    var body: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(stateColor)
                .frame(width: 8, height: 8)

            VStack(alignment: .leading, spacing: 2) {
                Text(vm.name)
                    .font(.body)
                    .fontWeight(.medium)
                Text(vm.state.displayName)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 2)
    }

    private var stateColor: Color {
        switch vm.state {
        case .running: return .green
        case .starting, .rebooting: return .orange
        case .stopped: return .gray
        case .crashed: return .red
        }
    }
}
