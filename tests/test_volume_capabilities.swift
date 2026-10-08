import Darwin
import Foundation
import FSKit

// Direct handler tests, without changing installed extensions or user preferences.
@main
@MainActor
struct VolumeCapabilityTests {
    static var checks = 0

    static func check(_ condition: Bool, _ message: String = "check failed", line: Int = #line) throws {
        checks += 1
        if !condition { throw Failure(message: "line \(line): \(message)") }
    }

    static func expect(_ code: Int32, _ body: () async throws -> Void) async throws {
        do {
            try await body()
            throw Failure(message: "expected errno \(code)")
        } catch let error as NSError {
            try check(error.domain == NSPOSIXErrorDomain && error.code == Int(code), "unexpected error: \(error)")
        }
    }

    static func volume(_ path: String, writable: Bool) throws -> lklfuseVolume {
        var error: Int32 = 0
        guard let pointer = lkfs_backend_from_file(path, writable ? 1 : 0, &error) else { throw posixError(error) }
        return try lklfuseVolume(backend: LinuxBackend(backend: pointer, retain: nil, cleanup: {}, writable: writable),
                                activationBackend: { _, _ in nil }, onContainerStatusChange: { _ in })
    }

    static func main() async throws {
        let path = CommandLine.arguments[1]
        let writable = CommandLine.arguments[2] == "rw"
        let v = try volume(path, writable: writable)
        defer { _ = v.teardown() }
        let pathConf: any FSVolume.PathConfOperations = v
        try check(pathConf.maximumFileSize == UInt64(Int64.max), "signed 64-bit file-size limit")
        try check(pathConf.maximumFileSizeInBits == 64, "explicit file-size bits through the FSKit protocol")
        let root = lklfuseItem(ino: 2, parentIno: 1, name: FSFileName(string: "/"))
        let name = FSFileName(string: "swift-capabilities")
        let key = FSFileName(string: "com.apple.test")
        let binary = Data([0, 255, 128, 0, 1])
        let request = FSItem.GetAttributesRequest()
        request.wantedAttributes = [.size, .mode, .flags, .birthTime, .backupTime]

        if writable {
            let attributes = FSItem.SetAttributesRequest()
            attributes.mode = 0600
            attributes.birthTime = timespec(tv_sec: 1000000000, tv_nsec: 123456789)
            attributes.backupTime = timespec(tv_sec: 1000000001, tv_nsec: 987654321)
            attributes.flags = UInt32(UF_HIDDEN)
            attributes.uid = 501
            let (item, _) = try await v.createItem(named: name, type: .file, inDirectory: root, attributes: attributes)
            try check(attributes.wasAttributeConsumed(.mode) && attributes.wasAttributeConsumed(.birthTime))
            try check(attributes.wasAttributeConsumed(.flags) && attributes.wasAttributeConsumed(.backupTime))
            try check(!attributes.wasAttributeConsumed(.uid))
            var got = try await v.attributes(request, of: item)
            try check(got.mode == 0600 && got.birthTime.tv_sec == 1000000000 && got.flags == UInt32(UF_HIDDEN))
            try check(got.backupTime.tv_nsec == 987654321)
            try check(v.maximumXattrSize == 65536)
            try await v.setXattr(named: key, to: binary, on: item, policy: .mustCreate)
            try check(try await v.xattr(named: key, of: item) == binary)
            try await expect(EEXIST) { try await v.setXattr(named: key, to: binary, on: item, policy: .mustCreate) }
            let empty = FSFileName(string: "empty")
            try await v.setXattr(named: empty, to: Data(), on: item, policy: .alwaysSet)
            try check(try await v.xattr(named: empty, of: item).isEmpty)
            try await expect(EINVAL) { try await v.setXattr(named: empty, to: nil, on: item, policy: .alwaysSet) }
            try await v.setXattr(named: empty, to: nil, on: item, policy: .delete)
            try await expect(ENOATTR) { _ = try await v.xattr(named: empty, of: item) }
            let rawKey = FSFileName(data: Data([0x78, 0xff]))
            try await v.setXattr(named: rawKey, to: binary, on: item, policy: .alwaysSet)
            let names = try await v.xattrs(of: item).map(\.data)
            try check(Set(names) == Set([key.data, rawKey.data]))
            try await expect(EPERM) {
                _ = try await v.xattr(named: FSFileName(string: "com.huanchuan.xlinuxfs.birthtime"), of: item)
            }
            try await v.openItem(item, modes: [.read, .write])
            try check(try await v.write(contents: Data("abc".utf8), to: item, at: 0) == 3)
            try await v.closeItem(item, modes: .read)
            try await v.closeItem(item, modes: [])
            let update = FSItem.SetAttributesRequest()
            update.backupTime = timespec(tv_sec: 1000000002, tv_nsec: 555)
            got = try await v.setAttributes(update, on: item)
            try check(update.wasAttributeConsumed(.backupTime) && got.birthTime.tv_sec == 1000000000)

            let orphanName = FSFileName(string: "swift-orphan")
            let (orphan, _) = try await v.createItem(named: orphanName, type: .file, inDirectory: root, attributes: FSItem.SetAttributesRequest())
            try await v.openItem(orphan, modes: [.read, .write])
            try await v.removeItem(orphan, named: orphanName, fromDirectory: root)
            try check(try await v.write(contents: binary, to: orphan, at: 0) == binary.count)
            got = try await v.attributes(request, of: orphan)
            try check(got.size == binary.count && got.linkCount == 0)
            try await v.setXattr(named: key, to: binary, on: orphan, policy: .alwaysSet)
            try check(try await v.xattr(named: key, of: orphan) == binary)
            try await v.closeItem(orphan, modes: [])
            try await v.reclaimItem(orphan)
            try await expect(ENOENT) { _ = try await v.attributes(request, of: orphan) }

            let (allocation, _) = try await v.createItem(named: FSFileName(string: "swift-allocation"), type: .file,
                                                         inDirectory: root, attributes: FSItem.SetAttributesRequest())
            try await expect(ENOTSUP) { _ = try await v.preallocateSpace(for: allocation, at: 0, length: 65536, flags: .contiguous) }
            try check(try await v.preallocateSpace(for: allocation, at: 0, length: 65536, flags: [.fromEOF, .persist]) == 65536)
            got = try await v.attributes(request, of: allocation)
            try check(got.size == 0 && got.allocSize >= 65536)
            try await v.closeItem(allocation, modes: [])
            let label = FSFileName(string: "Swift-Test")
            try check(try await v.setVolumeName(label).data == label.data && v.name.data == label.data)
            if #available(macOS 27.0, *) {
                try check((v as AnyObject) is any FSVolume.SeekRegionHandler)
            }
            try await expect(ENOTSUP) {
                _ = try await v.createItem(named: FSFileName(string: "fifo"), type: .fifo, inDirectory: root,
                                           attributes: FSItem.SetAttributesRequest())
            }
            var held: [(FSItem, FSFileName)] = []
            for i in 0..<1100 {
                let name = FSFileName(string: "swift-held-\(i)")
                let result = try await v.createItem(named: name, type: .file, inDirectory: root,
                                                     attributes: FSItem.SetAttributesRequest())
                held.append(result)
            }
            for (item, name) in held {
                try await v.removeItem(item, named: name, fromDirectory: root)
                try await v.reclaimItem(item)
            }
            try check(held.count == 1100, "async cross-thread descriptor pressure")
        } else {
            let (item, _) = try await v.lookupItem(named: name, inDirectory: root)
            let got = try await v.attributes(request, of: item)
            try check(got.size == 3 && got.birthTime.tv_sec == 1000000000 && got.backupTime.tv_sec == 1000000002)
            try check(got.flags == UInt32(UF_HIDDEN))
            try check(try await v.xattr(named: key, of: item) == binary)
            try check(v.name.string == "Swift-Test")
            try await expect(EROFS) { try await v.setXattr(named: key, to: nil, on: item, policy: .delete) }
            try await expect(EROFS) { _ = try await v.setVolumeName(FSFileName(string: "blocked")) }
            try await expect(EROFS) { _ = try await v.preallocateSpace(for: item, at: 0, length: 1, flags: .persist) }
            try await expect(EROFS) { try await v.openItem(item, modes: .write) }
            let update = FSItem.SetAttributesRequest()
            update.flags = 0
            try await expect(EROFS) { _ = try await v.setAttributes(update, on: item) }
        }
        try check(v.teardown(), "clean teardown with retained items")
        print("PASS Swift \(writable ? "rw" : "ro"): \(checks) handler checks")
    }
}

private struct Failure: Error { let message: String }
