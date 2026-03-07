import AVFoundation
import AudioToolbox

// Core Audio-based audio player for VM sound output.
// Receives PCM samples from VirtIO-SND via IPC and plays them through AudioQueue.

class CoreAudioPlayer {
    private var audioQueue: AudioQueueRef?
    private var buffers: [AudioQueueBufferRef?] = []
    private let bufferCount = 3
    private let bufferSize: UInt32 = 4096
    private var isRunning = false

    private var pendingData = Data()
    private let dataLock = NSLock()

    var sampleRate: Double = 44100.0
    var channelCount: UInt32 = 2
    var bitsPerSample: UInt32 = 16

    func start() {
        guard !isRunning else { return }

        var format = AudioStreamBasicDescription(
            mSampleRate: sampleRate,
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
            mBytesPerPacket: channelCount * (bitsPerSample / 8),
            mFramesPerPacket: 1,
            mBytesPerFrame: channelCount * (bitsPerSample / 8),
            mChannelsPerFrame: channelCount,
            mBitsPerChannel: bitsPerSample,
            mReserved: 0
        )

        let callbackPointer = UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque())

        let status = AudioQueueNewOutput(
            &format,
            audioQueueCallback,
            callbackPointer,
            CFRunLoopGetCurrent(),
            CFRunLoopMode.commonModes.rawValue,
            0,
            &audioQueue
        )

        guard status == noErr, let queue = audioQueue else {
            NSLog("CoreAudioPlayer: Failed to create audio queue: %d", status)
            return
        }

        buffers = Array(repeating: nil, count: bufferCount)
        for i in 0..<bufferCount {
            AudioQueueAllocateBuffer(queue, bufferSize, &buffers[i])
            if let buffer = buffers[i] {
                fillBuffer(buffer)
                AudioQueueEnqueueBuffer(queue, buffer, 0, nil)
            }
        }

        AudioQueueStart(queue, nil)
        isRunning = true
    }

    func stop() {
        guard isRunning, let queue = audioQueue else { return }
        AudioQueueStop(queue, true)
        AudioQueueDispose(queue, true)
        audioQueue = nil
        buffers = []
        isRunning = false
    }

    func enqueuePcmData(_ data: Data) {
        dataLock.lock()
        pendingData.append(data)
        dataLock.unlock()
    }

    fileprivate func fillNextBuffer(_ buffer: AudioQueueBufferRef) {
        fillBuffer(buffer)
    }

    private func fillBuffer(_ buffer: AudioQueueBufferRef) {
        dataLock.lock()
        let bytesToCopy = min(Int(bufferSize), pendingData.count)
        if bytesToCopy > 0 {
            pendingData.withUnsafeBytes { rawPtr in
                buffer.pointee.mAudioData.copyMemory(
                    from: rawPtr.baseAddress!,
                    byteCount: bytesToCopy
                )
            }
            pendingData.removeFirst(bytesToCopy)
            buffer.pointee.mAudioDataByteSize = UInt32(bytesToCopy)
        } else {
            // Fill silence
            memset(buffer.pointee.mAudioData, 0, Int(bufferSize))
            buffer.pointee.mAudioDataByteSize = bufferSize
        }
        dataLock.unlock()
    }

    deinit {
        stop()
    }
}

private func audioQueueCallback(
    userData: UnsafeMutableRawPointer?,
    queue: AudioQueueRef,
    buffer: AudioQueueBufferRef
) {
    guard let userData = userData else { return }
    let player = Unmanaged<CoreAudioPlayer>.fromOpaque(userData).takeUnretainedValue()
    player.fillNextBuffer(buffer)
    AudioQueueEnqueueBuffer(queue, buffer, 0, nil)
}
