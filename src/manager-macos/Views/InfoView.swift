import SwiftUI

struct InfoView: View {
    let vm: VmInfo

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                GroupBox("General") {
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 8) {
                        GridRow {
                            Text("Name").foregroundStyle(.secondary)
                            Text(vm.name)
                        }
                        GridRow {
                            Text("State").foregroundStyle(.secondary)
                            Text(vm.state.displayName)
                        }
                        GridRow {
                            Text("CPUs").foregroundStyle(.secondary)
                            Text("\(vm.cpuCount)")
                        }
                        GridRow {
                            Text("Memory").foregroundStyle(.secondary)
                            Text("\(vm.memoryMb) MB")
                        }
                    }
                    .padding(8)
                }

                GroupBox("Disk") {
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 8) {
                        GridRow {
                            Text("Image").foregroundStyle(.secondary)
                            Text(vm.diskPath)
                                .lineLimit(1)
                                .truncationMode(.middle)
                        }
                        GridRow {
                            Text("Kernel").foregroundStyle(.secondary)
                            Text(vm.kernelPath)
                                .lineLimit(1)
                                .truncationMode(.middle)
                        }
                    }
                    .padding(8)
                }

            }
            .padding()
        }
    }
}
