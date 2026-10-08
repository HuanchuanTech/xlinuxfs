//
//  lklfuseVolume.swift
//  An FSVolume backed by a Linux filesystem (ext2/3/4, XFS, Btrfs) through the
//  in-process LKL kernel via the lkfs_* bridge.
//
//  LKL syscalls are serialized through `withLock` (a synchronous critical section).
//

import FSKit
import Foundation

@inline(__always)
func posixError(_ code: Int32) -> NSError {
    NSError(domain: NSPOSIXErrorDomain, code: Int(code == 0 ? EIO : code))
}

struct LinuxBackend {
    let backend: UnsafeMutableRawPointer
    let retain: AnyObject?
    let cleanup: () -> Void
    let writable: Bool
}

@available(macOS 15.4, *)
final class lklfuseVolume: FSVolume, FSVolume.Operations, FSVolume.PathConfOperations,
                           FSVolume.ReadWriteOperations, FSVolume.OpenCloseOperations,
                           FSVolume.XattrOperations, FSVolume.RenameOperations,
                           FSVolume.PreallocateOperations {

    private var handle: OpaquePointer?            // lkfs_volume*
    private var backend: LinuxBackend?
    private let activationBackend: ([String], Bool) throws -> LinuxBackend?
    private let onContainerStatusChange: (FSContainerStatus) -> Void
    private var tornDown = false
    private var readOnly: Bool
    private var activated = false
    private let lock = NSLock()
    private var items: [UInt64: lklfuseItem] = [:]
    private let rootItem: lklfuseItem

    /// `backend` comes from lkfs_backend_from_block / lkfs_backend_from_file and is
    /// owned by this volume (freed in teardown).
    init(backend: LinuxBackend,
         activationBackend: @escaping ([String], Bool) throws -> LinuxBackend?,
         onContainerStatusChange: @escaping (FSContainerStatus) -> Void) throws {
        var err: Int32 = 0
        guard let h = lkfs_mount(backend.backend, !backend.writable, &err) else {
            lkfs_backend_free(backend.backend)
            backend.cleanup()
            throw posixError(err)
        }
        var st = lkfs_statfs_t()
        _ = lkfs_statfs(h, &st)
        let label = withUnsafeBytes(of: st.volume_name) { raw -> String in
            String(cString: raw.bindMemory(to: CChar.self).baseAddress!)
        }
        let fsType = withUnsafeBytes(of: st.fs_type) { raw -> String in
            String(cString: raw.bindMemory(to: CChar.self).baseAddress!)
        }
        let volName = label.isEmpty ? (fsType.isEmpty ? "Linux" : fsType) : label
        let totalBytes = st.total_blocks &* UInt64(st.block_size)
        let rawUUID = withUnsafeBytes(of: st.uuid) { Array($0.bindMemory(to: UInt8.self)) }
        let root = lklfuseItem(ino: lklfuseItem.rootIno,
                               parentIno: FSItem.Identifier.parentOfRoot.rawValue,
                               name: FSFileName(string: volName))

        self.handle = h
        self.backend = backend
        self.readOnly = !backend.writable
        self.activationBackend = activationBackend
        self.onContainerStatusChange = onContainerStatusChange
        self.rootItem = root

        let vid = FSVolume.Identifier(uuid: Ext4VolumeSupport.volumeUUID(raw: rawUUID, label: volName, sizeBytes: totalBytes))
        super.init(volumeID: vid, volumeName: FSFileName(string: volName))
        self.items[lklfuseItem.rootIno] = root
    }

    @discardableResult
    func teardown() -> Bool {
        withLock {
            if tornDown { return true }
            if let h = handle {
                let rc = lkfs_umount(h)
                if rc != 0 {
                    // Teardown incomplete: the LKL disk is still registered and still
                    // references `backend` via disk.handle. Freeing the backend (or the
                    // resource it wraps) would dangle, and removing the disk would
                    // surprise-abort the fs. Keep the handle + backend + resource owned
                    // and leave tornDown=false so a later unload can retry. (Edge case:
                    // a busy unmount or a >2s teardown — never seen; teardown is ~15ms.)
                    NSLog("[xlinuxfs] teardown incomplete (errno \(rc)); retaining disk+backend")
                    return false
                }
                handle = nil   // lkfs_umount removed the disk and freed the handle
            }
            if let b = backend { lkfs_backend_free(b.backend); b.cleanup(); backend = nil }
            tornDown = true
            items.removeAll()
            return true
        }
    }

    @discardableResult
    private func withLock<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock(); defer { lock.unlock() }
        return try body()
    }

    private func item(for ino: UInt64, parentIno: UInt64, name: FSFileName) -> lklfuseItem {
        if let existing = items[ino] { return existing }
        let it = lklfuseItem(ino: ino, parentIno: parentIno, name: name)
        items[ino] = it
        return it
    }

    fileprivate func makeAttributes(_ a: lkfs_attr_t, parentIno: UInt64, preferContext: Bool = false) -> FSItem.Attributes {
        let attrs = FSItem.Attributes()
        attrs.type = FSItem.ItemType(rawValue: Int(a.type)) ?? .file
        attrs.mode = a.mode
        attrs.linkCount = a.nlink
        attrs.size = a.size
        attrs.allocSize = a.alloc_size
        attrs.fileID = FSItem.Identifier(rawValue: a.ino) ?? .invalid
        let parent = preferContext ? parentIno : (a.parent_ino != 0 ? a.parent_ino : parentIno)
        attrs.parentID = FSItem.Identifier(rawValue: parent) ?? .invalid
        attrs.uid = 0
        attrs.gid = 0
        attrs.flags = a.flags
        attrs.modifyTime = timespec(tv_sec: Int(a.mtime_sec), tv_nsec: Int(a.mtime_nsec))
        attrs.accessTime = timespec(tv_sec: Int(a.atime_sec), tv_nsec: Int(a.atime_nsec))
        attrs.changeTime = timespec(tv_sec: Int(a.ctime_sec), tv_nsec: Int(a.ctime_nsec))
        if a.has_btime != 0 {
            attrs.birthTime = timespec(tv_sec: Int(a.btime_sec), tv_nsec: Int(a.btime_nsec))
        }
        if a.has_backup_time != 0 {
            attrs.backupTime = timespec(tv_sec: Int(a.backup_sec), tv_nsec: Int(a.backup_nsec))
        }
        return attrs
    }

    // MARK: properties
    var supportedVolumeCapabilities: FSVolume.SupportedCapabilities {
        let caps = FSVolume.SupportedCapabilities()
        caps.supportsHardLinks = true           // via LKL link(2); writable mounts honor ln
        caps.supportsSymbolicLinks = true       // full: read + create (via LKL symlink); writable mounts honor ln -s
        caps.supportsPersistentObjectIDs = true
        caps.supports64BitObjectIDs = true
        caps.supports2TBFiles = true
        caps.supportsSparseFiles = true
        caps.supportsHiddenFiles = true
        caps.caseFormat = .sensitive            // Linux filesystems are case-sensitive
        return caps
    }

    var volumeStatistics: FSStatFSResult {
        // Must match the extension's FSShortName (lklfuse/Info.plist) so the mounted
        // volume's statfs f_fstypename reads "xlinuxfs".
        let stats = FSStatFSResult(fileSystemTypeName: "xlinuxfs")
        return withLock {
            guard let h = handle else { return stats }
            var st = lkfs_statfs_t()
            guard lkfs_statfs(h, &st) == 0 else { return stats }
            let bs = Int(st.block_size == 0 ? 4096 : st.block_size)
            stats.blockSize = bs
            stats.ioSize = bs
            stats.totalBlocks = st.total_blocks
            stats.availableBlocks = st.free_blocks
            stats.freeBlocks = st.free_blocks
            stats.usedBlocks = st.total_blocks > st.free_blocks ? st.total_blocks - st.free_blocks : 0
            stats.totalFiles = st.total_files
            stats.freeFiles = st.free_files
            return stats
        }
    }

    @available(macOS 26.4, *)
    var requestedMountOptions: FSVolume.MountOptions {
        get { withLock { readOnly ? [.readOnly] : [] } }
        set { }
    }

    // MARK: lifecycle
    func activate(options: FSTaskOptions) async throws -> FSItem {
        try withLock {
            guard !tornDown, backend != nil else { throw posixError(ENXIO) }
            guard !activated else { throw posixError(EBUSY) }
            if let replacement = try activationBackend(options.taskOptions, readOnly) {
                // A failed LKL unmount still owns the old backend. Release only
                // the unused replacement, so unload can retry the original disk.
                let rc = handle.map { lkfs_umount($0) } ?? 0
                guard rc == 0 else {
                    lkfs_backend_free(replacement.backend)
                    replacement.cleanup()
                    throw posixError(rc)
                }
                handle = nil
                if let b = backend { lkfs_backend_free(b.backend); b.cleanup() }
                backend = replacement
                readOnly = !replacement.writable
                items = [lklfuseItem.rootIno: rootItem]
            }
            try openMountIfNeeded()
            activated = true
        }
        onContainerStatusChange(.active)
        return rootItem
    }

    func deactivate(options: FSDeactivateOptions = []) async throws {
        withLock { activated = false }
        onContainerStatusChange(.ready)
    }

    // Called only under withLock, including when FSKit remounts a loaded volume.
    private func openMountIfNeeded() throws {
        guard !tornDown, let backend else { throw posixError(ENXIO) }
        guard handle == nil else { return }
        var error: Int32 = 0
        guard let reopened = lkfs_mount(backend.backend, readOnly, &error) else {
            throw posixError(error)
        }
        handle = reopened
        items = [lklfuseItem.rootIno: rootItem]
    }

    func mount(options: FSTaskOptions) async throws {
        try withLock { try openMountIfNeeded() }
    }
    func unmount() async {
        withLock {
            guard let h = handle else { return }
            // FSKit can revoke I/O before unloadResource. Finish the filesystem's
            // clean unmount here; sync alone can leave XFS metadata in its log.
            let rc = lkfs_umount(h)
            if rc == 0 {
                handle = nil
                items = [lklfuseItem.rootIno: rootItem]
            } else {
                NSLog("[xlinuxfs] unmount incomplete (errno \(rc)); retaining disk+backend")
            }
        }
    }

    func synchronize(flags: FSSyncFlags) async throws {
        try withLock {
            guard let h = handle else { return }
            let rc = lkfs_sync(h)
            if rc != 0 { throw posixError(-rc) }
        }
    }

    // MARK: attributes
    func attributes(_ desiredAttributes: FSItem.GetAttributesRequest, of item: FSItem) async throws -> FSItem.Attributes {
        try withLock {
            guard let it = item as? lklfuseItem, let h = handle else { throw posixError(EINVAL) }
            var a = lkfs_attr_t()
            let rc = lkfs_getattr(h, it.ino, &a)
            if rc != 0 { throw posixError(-rc) }
            return makeAttributes(a, parentIno: it.parentIno)
        }
    }

    func setAttributes(_ newAttributes: FSItem.SetAttributesRequest, on item: FSItem) async throws -> FSItem.Attributes {
        try withLock {
            if readOnly { throw posixError(EROFS) }
            guard let it = item as? lklfuseItem, let h = handle else { throw posixError(EINVAL) }
            try applyAttributes(newAttributes, ino: it.ino, handle: h)
            var a = lkfs_attr_t()
            let rc = lkfs_getattr(h, it.ino, &a)
            if rc != 0 { throw posixError(-rc) }
            return makeAttributes(a, parentIno: it.parentIno)
        }
    }

    // Caller holds the volume lock. Ownership remains deliberately unconsumed.
    private func applyAttributes(_ newAttributes: FSItem.SetAttributesRequest, ino: UInt64, handle h: OpaquePointer) throws {
        var consumed: FSItem.Attribute = []
        defer { newAttributes.consumedAttributes = consumed }
        if newAttributes.isValid(.size) {
            let rc = lkfs_truncate(h, ino, newAttributes.size)
            if rc != 0 { throw posixError(-rc) }
            consumed.insert(.size)
        }
        if newAttributes.isValid(.mode) {
            let rc = lkfs_chmod(h, ino, newAttributes.mode)
            if rc != 0 { throw posixError(-rc) }
            consumed.insert(.mode)
        }
        let wantM = newAttributes.isValid(.modifyTime)
        let wantA = newAttributes.isValid(.accessTime)
        if wantM || wantA {
            let m = newAttributes.modifyTime
            let a = newAttributes.accessTime
            let rc = lkfs_set_times(h, ino,
                                    wantM ? Int64(m.tv_sec) : Int64.min, Int64(m.tv_nsec),
                                    wantA ? Int64(a.tv_sec) : Int64.min, Int64(a.tv_nsec))
            if rc != 0 { throw posixError(-rc) }
            if wantM { consumed.insert(.modifyTime) }
            if wantA { consumed.insert(.accessTime) }
        }
        for (attribute, time, birth) in [
            (FSItem.Attribute.birthTime, newAttributes.birthTime, true),
            (FSItem.Attribute.backupTime, newAttributes.backupTime, false)
        ] where newAttributes.isValid(attribute) {
            let rc = lkfs_set_extra_time(h, ino, birth, Int64(time.tv_sec), Int64(time.tv_nsec))
            if rc != 0 { throw posixError(-rc) }
            consumed.insert(attribute)
        }
        if newAttributes.isValid(.flags) {
            let rc = lkfs_set_flags(h, ino, newAttributes.flags)
            if rc != 0 { throw posixError(-rc) }
            consumed.insert(.flags)
        }
    }

    // MARK: lookup / reclaim
    func lookupItem(named name: FSFileName, inDirectory directory: FSItem) async throws -> (FSItem, FSFileName) {
        try withLock {
            guard let dir = directory as? lklfuseItem, let h = handle, let nameStr = name.string else { throw posixError(EINVAL) }
            var err: Int32 = 0
            let ino = nameStr.withCString { lkfs_lookup(h, dir.ino, $0, &err) }
            if ino == 0 { throw posixError(err == 0 ? ENOENT : err) }
            let fsName = FSFileName(string: nameStr)
            return (item(for: ino, parentIno: dir.ino, name: fsName), fsName)
        }
    }

    func reclaimItem(_ item: FSItem) async throws {
        withLock {
            guard let it = item as? lklfuseItem else { return }
            if it.ino != lklfuseItem.rootIno {
                if let h = handle { lkfs_reclaim_item(h, it.ino) }
                items.removeValue(forKey: it.ino)
            }
        }
    }

    func readSymbolicLink(_ item: FSItem) async throws -> FSFileName {
        try withLock {
            guard let it = item as? lklfuseItem, let h = handle else { throw posixError(EINVAL) }
            var buf = [CChar](repeating: 0, count: 4096)
            let rc = buf.withUnsafeMutableBufferPointer { lkfs_readlink(h, it.ino, $0.baseAddress, $0.count) }
            if rc != 0 { throw posixError(-rc) }
            return FSFileName(string: String(cString: buf))
        }
    }

    // MARK: create / remove / rename
    func createItem(named name: FSFileName, type: FSItem.ItemType, inDirectory directory: FSItem,
                    attributes newAttributes: FSItem.SetAttributesRequest) async throws -> (FSItem, FSFileName) {
        try withLock {
            if readOnly { throw posixError(EROFS) }
            guard let dir = directory as? lklfuseItem, let h = handle, let nameStr = name.string else { throw posixError(EINVAL) }
            guard type == .directory || type == .file else { throw posixError(ENOTSUP) }
            let kind = UInt32(type == .directory ? LKFS_TYPE_DIR : LKFS_TYPE_FILE)
            var err: Int32 = 0
            let ino = nameStr.withCString { lkfs_create(h, dir.ino, $0, kind, &err) }
            if ino == 0 { throw posixError(err) }
            do {
                try applyAttributes(newAttributes, ino: ino, handle: h)
            } catch {
                _ = nameStr.withCString { lkfs_remove(h, dir.ino, $0) }
                lkfs_reclaim_item(h, ino)
                throw error
            }
            let fsName = FSFileName(string: nameStr)
            return (item(for: ino, parentIno: dir.ino, name: fsName), fsName)
        }
    }

    func createSymbolicLink(named name: FSFileName, inDirectory directory: FSItem,
                            attributes newAttributes: FSItem.SetAttributesRequest,
                            linkContents contents: FSFileName) async throws -> (FSItem, FSFileName) {
        try withLock {
            if readOnly { throw posixError(EROFS) }
            guard let dir = directory as? lklfuseItem, let h = handle,
                  let nameStr = name.string, let target = contents.string else { throw posixError(EINVAL) }
            var err: Int32 = 0
            let ino = nameStr.withCString { np in target.withCString { tp in lkfs_symlink(h, dir.ino, np, tp, &err) } }
            if ino == 0 { throw posixError(err) }
            let fsName = FSFileName(string: nameStr)
            return (item(for: ino, parentIno: dir.ino, name: fsName), fsName)
        }
    }

    func createLink(to item: FSItem, named name: FSFileName, inDirectory directory: FSItem) async throws -> FSFileName {
        try withLock {
            if readOnly { throw posixError(EROFS) }
            guard let target = item as? lklfuseItem, let dir = directory as? lklfuseItem,
                  let h = handle, let nameStr = name.string else { throw posixError(EINVAL) }
            var err: Int32 = 0
            let rc = nameStr.withCString { lkfs_link(h, target.ino, dir.ino, $0, &err) }
            if rc != 0 { throw posixError(err) }
            return FSFileName(string: nameStr)
        }
    }

    func removeItem(_ item: FSItem, named name: FSFileName, fromDirectory directory: FSItem) async throws {
        try withLock {
            if readOnly { throw posixError(EROFS) }
            guard let dir = directory as? lklfuseItem, let h = handle, let nameStr = name.string else { throw posixError(EINVAL) }
            let rc = nameStr.withCString { lkfs_remove(h, dir.ino, $0) }
            if rc != 0 { throw posixError(-rc) }
        }
    }

    func renameItem(_ item: FSItem, inDirectory sourceDirectory: FSItem, named sourceName: FSFileName,
                    to destinationName: FSFileName, inDirectory destinationDirectory: FSItem,
                    overItem: FSItem?) async throws -> FSFileName {
        try withLock {
            if readOnly { throw posixError(EROFS) }
            guard let sdir = sourceDirectory as? lklfuseItem, let ddir = destinationDirectory as? lklfuseItem,
                  let h = handle, let src = sourceName.string, let dst = destinationName.string else { throw posixError(EINVAL) }
            let rc = src.withCString { sp in dst.withCString { dp in lkfs_rename(h, sdir.ino, sp, ddir.ino, dp) } }
            if rc != 0 { throw posixError(-rc) }
            if let it = item as? lklfuseItem {
                it.parentIno = ddir.ino
                it.name = destinationName
            }
            return FSFileName(string: dst)
        }
    }

    // MARK: enumeration
    func enumerateDirectory(_ directory: FSItem, startingAt cookie: FSDirectoryCookie, verifier: FSDirectoryVerifier,
                            attributes: FSItem.GetAttributesRequest?, packer: FSDirectoryEntryPacker) async throws -> FSDirectoryVerifier {
        try withLock {
            guard let dir = directory as? lklfuseItem, let h = handle else { throw posixError(EINVAL) }
            let wantDots = (attributes == nil)
            let c = cookie.rawValue
            let ctx = EnumContext(volume: self, packer: packer, parentIno: dir.ino, wantAttributes: attributes != nil)

            if wantDots {
                if c < 1 {
                    if !ctx.packRaw(name: ".", ino: dir.ino, type: UInt32(LKFS_TYPE_DIR), nextCookie: 1) {
                        return FSDirectoryVerifier(rawValue: 1)
                    }
                }
                if c < 2 {
                    if !ctx.packRaw(name: "..", ino: dir.ino, type: UInt32(LKFS_TYPE_DIR), nextCookie: 2) {
                        return FSDirectoryVerifier(rawValue: 1)
                    }
                }
            }
            let realSkip: Int64 = c <= 2 ? 0 : Int64(c - 2)
            let ctxPtr = Unmanaged.passUnretained(ctx).toOpaque()
            let rc = lkfs_readdir(h, dir.ino, realSkip, ctxPtr, enumTrampoline)
            if rc != 0 && !ctx.stopped { throw posixError(-rc) }
            return FSDirectoryVerifier(rawValue: 1)
        }
    }

    // MARK: PathConf
    var maximumLinkCount: Int { 65000 }
    var maximumNameLength: Int { 255 }
    var restrictsOwnershipChanges: Bool { false }
    var truncatesLongNames: Bool { false }
    var maximumFileSize: UInt64 { UInt64(Int64.max) }
    var maximumXattrSize: Int { Int(LKFS_XATTR_MAX) }

    // MARK: Extended attributes
    private func withXattrName<T>(_ name: FSFileName, _ body: (UnsafePointer<CChar>) throws -> T) throws -> T {
        guard !name.data.isEmpty, !name.data.contains(0) else { throw posixError(EINVAL) }
        let bytes = name.data.map { CChar(bitPattern: $0) } + [0]
        return try bytes.withUnsafeBufferPointer { try body($0.baseAddress!) }
    }

    func xattr(named name: FSFileName, of item: FSItem) async throws -> Data {
        try withLock {
            guard let it = item as? lklfuseItem, let h = handle else { throw posixError(EINVAL) }
            return try withXattrName(name) { pointer in
                var data = Data(count: Int(LKFS_XATTR_MAX))
                let count = data.withUnsafeMutableBytes { lkfs_get_xattr(h, it.ino, pointer, $0.baseAddress, $0.count) }
                if count < 0 { throw posixError(Int32(-count)) }
                data.count = Int(count)
                return data
            }
        }
    }

    func setXattr(named name: FSFileName, to value: Data?, on item: FSItem, policy: FSVolume.SetXattrPolicy) async throws {
        try withLock {
            if readOnly { throw posixError(EROFS) }
            guard let it = item as? lklfuseItem, let h = handle else { throw posixError(EINVAL) }
            guard policy == .delete || value != nil else { throw posixError(EINVAL) }
            let data = value ?? Data()
            guard data.count <= Int(LKFS_XATTR_MAX) else { throw posixError(E2BIG) }
            let flag: UInt32
            switch policy {
            case .alwaysSet: flag = UInt32(LKFS_XATTR_SET)
            case .mustCreate: flag = UInt32(LKFS_XATTR_CREATE)
            case .mustReplace: flag = UInt32(LKFS_XATTR_REPLACE)
            case .delete: flag = UInt32(LKFS_XATTR_DELETE)
            @unknown default: throw posixError(EINVAL)
            }
            let rc = try withXattrName(name) { pointer in
                data.withUnsafeBytes { lkfs_set_xattr(h, it.ino, pointer, $0.baseAddress, $0.count, flag) }
            }
            if rc != 0 { throw posixError(-rc) }
        }
    }

    func xattrs(of item: FSItem) async throws -> [FSFileName] {
        try withLock {
            guard let it = item as? lklfuseItem, let h = handle else { throw posixError(EINVAL) }
            var data = Data(count: Int(LKFS_XATTR_MAX))
            let count = data.withUnsafeMutableBytes {
                lkfs_list_xattrs(h, it.ino, $0.baseAddress?.assumingMemoryBound(to: CChar.self), $0.count)
            }
            if count < 0 { throw posixError(Int32(-count)) }
            return data.prefix(Int(count)).split(separator: 0).map { FSFileName(data: Data($0)) }
        }
    }

    // MARK: Volume label and allocation
    func setVolumeName(_ name: FSFileName) async throws -> FSFileName {
        try withLock {
            if readOnly { throw posixError(EROFS) }
            guard let h = handle, let value = name.string, !value.utf8.contains(0) else { throw posixError(EINVAL) }
            let rc = value.withCString { lkfs_set_volume_name(h, $0) }
            if rc != 0 { throw posixError(-rc) }
            self.name = name
            rootItem.name = name
            return name
        }
    }

    func preallocateSpace(for item: FSItem, at offset: off_t, length: Int, flags: FSVolume.PreallocateFlags) async throws -> Int {
        try withLock {
            if readOnly { throw posixError(EROFS) }
            guard let it = item as? lklfuseItem, let h = handle else { throw posixError(EINVAL) }
            guard let rawFlags = UInt32(exactly: flags.rawValue) else { throw posixError(EINVAL) }
            let result = lkfs_preallocate(h, it.ino, Int64(offset), Int64(length), rawFlags)
            if result < 0 { throw posixError(Int32(-result)) }
            return Int(result)
        }
    }

    // MARK: ReadWrite
    func read(from item: FSItem, at offset: off_t, length: Int, into buffer: FSMutableFileDataBuffer) async throws -> Int {
        try withLock {
            guard let it = item as? lklfuseItem, let h = handle else { throw posixError(EINVAL) }
            let cap = min(length, buffer.length)
            var err: Int32 = 0
            let n = buffer.withUnsafeMutableBytes { raw in
                lkfs_read(h, it.ino, Int64(offset), raw.baseAddress, Int64(cap), &err)
            }
            if n < 0 { throw posixError(err) }
            return Int(n)
        }
    }

    func write(contents: Data, to item: FSItem, at offset: off_t) async throws -> Int {
        try withLock {
            if readOnly { throw posixError(EROFS) }
            guard let it = item as? lklfuseItem, let h = handle else { throw posixError(EINVAL) }
            var err: Int32 = 0
            let n = contents.withUnsafeBytes { raw in
                lkfs_write(h, it.ino, Int64(offset), raw.baseAddress, Int64(raw.count), &err)
            }
            if n < 0 { throw posixError(err) }
            return Int(n)
        }
    }

    // MARK: OpenClose
    func openItem(_ item: FSItem, modes: FSVolume.OpenModes) async throws {
        try withLock {
            guard let it = item as? lklfuseItem, let h = handle,
                  let raw = UInt32(exactly: modes.rawValue) else { throw posixError(EINVAL) }
            let rc = lkfs_open_item(h, it.ino, raw)
            if rc != 0 { throw posixError(-rc) }
        }
    }

    func closeItem(_ item: FSItem, modes: FSVolume.OpenModes) async throws {
        try withLock {
            guard let it = item as? lklfuseItem, let h = handle,
                  let raw = UInt32(exactly: modes.rawValue) else { throw posixError(EINVAL) }
            let rc = lkfs_close_item(h, it.ino, raw)
            if rc != 0 { throw posixError(-rc) }
        }
    }
}

@available(macOS 27.0, *)
extension lklfuseVolume: FSVolume.SeekRegionHandler {
    func seek(within item: FSItem, from offset: off_t, region: FSVolume.SeekRegion, context: FSContext) async throws -> FSSeekRegionResult {
        try withLock {
            guard let it = item as? lklfuseItem, let h = handle else { throw posixError(EINVAL) }
            let hole: Bool
            switch region {
            case .hole: hole = true
            case .data: hole = false
            @unknown default: throw posixError(EINVAL)
            }
            let result = lkfs_seek_region(h, it.ino, Int64(offset), hole)
            if result < 0 { throw posixError(Int32(-result)) }
            return FSSeekRegionResult(returnedOffset: off_t(result))
        }
    }
}

// MARK: - Directory enumeration trampoline

@available(macOS 15.4, *)
final class EnumContext {
    unowned let volume: lklfuseVolume
    let packer: FSDirectoryEntryPacker
    let parentIno: UInt64
    let wantAttributes: Bool
    var stopped = false

    init(volume: lklfuseVolume, packer: FSDirectoryEntryPacker, parentIno: UInt64, wantAttributes: Bool) {
        self.volume = volume
        self.packer = packer
        self.parentIno = parentIno
        self.wantAttributes = wantAttributes
    }

    func packRaw(name: String, ino: UInt64, type: UInt32, nextCookie: UInt64) -> Bool {
        let itemType = FSItem.ItemType(rawValue: Int(type)) ?? .directory
        let ok = packer.packEntry(name: FSFileName(string: name), itemType: itemType,
                                  itemID: FSItem.Identifier(rawValue: ino) ?? .invalid,
                                  nextCookie: FSDirectoryCookie(rawValue: nextCookie), attributes: nil)
        if !ok { stopped = true }
        return ok
    }

    func pack(name: String, ino: UInt64, type: UInt32, bridgeCookie: Int64) -> Bool {
        let itemType = FSItem.ItemType(rawValue: Int(type)) ?? .file
        var attrs: FSItem.Attributes? = nil
        if wantAttributes {
            attrs = volume.enumAttributesRaw(ino: ino, parentIno: parentIno)
        }
        let next = UInt64(bridgeCookie) + 2
        let ok = packer.packEntry(name: FSFileName(string: name), itemType: itemType,
                                  itemID: FSItem.Identifier(rawValue: ino) ?? .invalid,
                                  nextCookie: FSDirectoryCookie(rawValue: next), attributes: attrs)
        if !ok { stopped = true }
        return ok
    }
}

@available(macOS 15.4, *)
extension lklfuseVolume {
    fileprivate func enumAttributesRaw(ino: UInt64, parentIno: UInt64) -> FSItem.Attributes? {
        guard let h = handle else { return nil }
        var a = lkfs_attr_t()
        if lkfs_getattr(h, ino, &a) != 0 { return nil }
        return makeAttributes(a, parentIno: parentIno, preferContext: true)
    }
}

@available(macOS 15.4, *)
let enumTrampoline: @convention(c)
    (UnsafeMutableRawPointer?, UnsafePointer<CChar>?, UInt64, UInt32, Int64) -> Int32 = {
        ctxPtr, namePtr, ino, type, cookie in
        guard let ctxPtr = ctxPtr, let namePtr = namePtr else { return 1 }
        let ctx = Unmanaged<EnumContext>.fromOpaque(ctxPtr).takeUnretainedValue()
        let cont = ctx.pack(name: String(cString: namePtr), ino: ino, type: type, bridgeCookie: cookie)
        return cont ? 0 : 1
    }

// MARK: - Shared helpers

enum Ext4VolumeSupport {
    /// The volume's identity UUID: the native fs UUID (ext s_uuid / XFS sb_uuid /
    /// Btrfs fsid) when present, else a synthesized fallback. The on-disk UUID is stable
    /// across relabels and distinguishes independently-created same-label/same-size volumes.
    /// (A block-level clone copies the UUID, so clones share identity.)
    static func volumeUUID(raw: [UInt8], label: String, sizeBytes: UInt64) -> UUID {
        if raw.count >= 16, raw.prefix(16).contains(where: { $0 != 0 }) {
            let b = raw
            return UUID(uuid: (b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                               b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]))
        }
        return stableUUID(label: label, sizeBytes: sizeBytes)
    }

    static func stableUUID(label: String, sizeBytes: UInt64) -> UUID {
        var bytes = Array("xlinuxfs:\(label):\(sizeBytes)".utf8)
        var u = [UInt8](repeating: 0, count: 16)
        for (i, b) in bytes.enumerated() { u[i % 16] = u[i % 16] &+ b &+ UInt8(i & 0xff) }
        bytes.removeAll()
        u[6] = (u[6] & 0x0F) | 0x40
        u[8] = (u[8] & 0x3F) | 0x80
        return UUID(uuid: (u[0],u[1],u[2],u[3],u[4],u[5],u[6],u[7],u[8],u[9],u[10],u[11],u[12],u[13],u[14],u[15]))
    }
}
