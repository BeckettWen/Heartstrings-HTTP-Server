import Foundation
import Darwin

@_cdecl("heartstrings_swift_prepare_file_response")
public func heartstringsSwiftPrepareFileResponse(
    _ path: UnsafePointer<CChar>?,
    _ isGetRequest: Int32,
    _ descriptorOut: UnsafeMutablePointer<Int32>?,
    _ sizeOut: UnsafeMutablePointer<UInt64>?,
    _ contentTypeOut: UnsafeMutablePointer<CChar>?,
    _ contentTypeCapacity: UInt32
) -> Int32 {
    guard let path, let descriptorOut, let sizeOut, let contentTypeOut else {
        return 5
    }
    descriptorOut.pointee = -1
    sizeOut.pointee = 0
    guard contentTypeCapacity > 0 else {
        return 5
    }
    contentTypeOut[0] = 0

    let descriptor = Darwin.open(path, O_RDONLY | O_CLOEXEC)
    if descriptor == -1 {
        return errno == ENOENT || errno == ENOTDIR ? 1 : 3
    }

    var fileStatus = stat()
    guard Darwin.fstat(descriptor, &fileStatus) == 0 else {
        Darwin.close(descriptor)
        return 4
    }

    guard (fileStatus.st_mode & mode_t(S_IFMT)) == mode_t(S_IFREG),
          fileStatus.st_size >= 0 else {
        Darwin.close(descriptor)
        return 2
    }

    let extensionName = URL(fileURLWithPath: String(cString: path))
        .pathExtension
        .lowercased()
    let contentType = [
        "css": "text/css; charset=utf-8",
        "htm": "text/html; charset=utf-8",
        "html": "text/html; charset=utf-8",
        "js": "text/javascript; charset=utf-8",
        "json": "application/json",
        "jpg": "image/jpeg",
        "jpeg": "image/jpeg",
        "png": "image/png",
        "svg": "image/svg+xml",
        "txt": "text/plain; charset=utf-8"
    ][extensionName] ?? "application/octet-stream"
    let contentTypeBytes = Array(contentType.utf8)
    guard contentTypeBytes.count + 1 <= Int(contentTypeCapacity) else {
        Darwin.close(descriptor)
        return 5
    }

    for (index, byte) in contentTypeBytes.enumerated() {
        contentTypeOut[index] = CChar(bitPattern: byte)
    }
    contentTypeOut[contentTypeBytes.count] = 0

    sizeOut.pointee = UInt64(fileStatus.st_size)
    if isGetRequest != 0 {
        descriptorOut.pointee = Int32(descriptor)
    } else {
        descriptorOut.pointee = -1
        Darwin.close(descriptor)
    }
    return 0
}
