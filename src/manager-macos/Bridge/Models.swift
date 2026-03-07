import Foundation

enum VmState: String, Codable {
    case stopped
    case starting
    case running
    case rebooting
    case crashed

    var displayName: String {
        switch self {
        case .stopped: return "Stopped"
        case .starting: return "Starting"
        case .running: return "Running"
        case .rebooting: return "Rebooting"
        case .crashed: return "Crashed"
        }
    }
}

struct VmInfo: Identifiable, Codable {
    let id: String
    let name: String
    let kernelPath: String
    let initrdPath: String
    let diskPath: String
    let memoryMb: Int
    let cpuCount: Int
    let state: VmState
    let netLinkUp: Bool
}

struct VmCreateConfig {
    let name: String
    let kernelPath: String
    let initrdPath: String
    let diskPath: String
    let memoryMb: Int
    let cpuCount: Int
    let cmdline: String
    let netEnabled: Bool
}
