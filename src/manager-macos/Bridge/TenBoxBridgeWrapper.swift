import Foundation
import TenBoxBridge

class TenBoxBridgeWrapper {
    private let bridge = TBBridge()

    func getVmList() -> [VmInfo] {
        let objcList = bridge.getVmList()
        return objcList.map { info in
            VmInfo(
                id: info.vmId,
                name: info.name,
                kernelPath: info.kernelPath,
                initrdPath: info.initrdPath,
                diskPath: info.diskPath,
                memoryMb: Int(info.memoryMb),
                cpuCount: Int(info.cpuCount),
                state: VmState(rawValue: info.state) ?? .stopped,
                netLinkUp: info.netLinkUp
            )
        }
    }

    func createVm(config: VmCreateConfig) {
        let objcConfig = TBVmCreateConfig()
        objcConfig.name = config.name
        objcConfig.kernelPath = config.kernelPath
        objcConfig.initrdPath = config.initrdPath
        objcConfig.diskPath = config.diskPath
        objcConfig.memoryMb = config.memoryMb
        objcConfig.cpuCount = config.cpuCount
        objcConfig.cmdline = config.cmdline
        objcConfig.netEnabled = config.netEnabled
        bridge.createVm(with: objcConfig)
    }

    func deleteVm(id: String) {
        bridge.deleteVm(withId: id)
    }

    func startVm(id: String) {
        bridge.startVm(withId: id)
    }

    func stopVm(id: String) {
        bridge.stopVm(withId: id)
    }

    func rebootVm(id: String) {
        bridge.rebootVm(withId: id)
    }

    func shutdownVm(id: String) {
        bridge.shutdownVm(withId: id)
    }

    func waitForRuntimeConnection(vmId: String, timeout: TimeInterval = 30) -> Bool {
        return bridge.wait(forRuntimeConnection: vmId, timeout: timeout)
    }

    func takeAcceptedFd(vmId: String) -> Int32 {
        return bridge.takeAcceptedFd(forVm: vmId)
    }
}
