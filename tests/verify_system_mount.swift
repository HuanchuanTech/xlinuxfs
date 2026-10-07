import Foundation

let data = try Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1]))
let info = try PropertyListSerialization.propertyList(from: data, format: nil) as! [String: Any]
guard let path = info["MountPoint"] as? String, !path.isEmpty else {
    fatalError("The system did not report a mount point")
}
var filesystem = statfs()
precondition(statfs(path, &filesystem) == 0)
let type = withUnsafeBytes(of: filesystem.f_fstypename) {
    String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self)
}
precondition(type == "xlinuxfs", "Wrong filesystem driver: \(type)")
let readOnly = filesystem.f_flags & UInt32(MNT_RDONLY) != 0
precondition(readOnly, "Expected a read-only mount")
_ = try FileManager.default.contentsOfDirectory(atPath: path)
print("PASS xlinuxfs, read-only, readable directory")
