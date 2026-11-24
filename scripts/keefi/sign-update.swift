// Build once on the release Mac. Only the public key and signatures go in releases.
import Foundation
import CryptoKit
import Security

enum SigningError: Error, CustomStringConvertible {
    case message(String)
    var description: String { switch self { case let .message(text): return text } }
}

let service = "xyz.beamgrove.keefi.release-signing"
let account = "ed25519-v1"
let lookup: [String: Any] = [
    kSecClass as String: kSecClassGenericPassword,
    kSecAttrService as String: service,
    kSecAttrAccount as String: account
]

func errorText(_ status: OSStatus) -> SigningError {
    .message("Keychain: " + ((SecCopyErrorMessageString(status, nil) as String?) ?? String(status)))
}

func readKey(create: Bool) throws -> Curve25519.Signing.PrivateKey {
    var query = lookup
    query[kSecReturnData as String] = true
    var result: CFTypeRef?
    let status = SecItemCopyMatching(query as CFDictionary, &result)
    if status == errSecSuccess, let data = result as? Data {
        return try Curve25519.Signing.PrivateKey(rawRepresentation: data)
    }
    guard create && status == errSecItemNotFound else { throw errorText(status) }
    let key = Curve25519.Signing.PrivateKey()
    var item = lookup
    item[kSecValueData as String] = key.rawRepresentation
    item[kSecAttrLabel as String] = "KEEFI release update signing (Ed25519)"
    item[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
    let saved = SecItemAdd(item as CFDictionary, nil)
    guard saved == errSecSuccess else { throw errorText(saved) }
    return key
}

do {
    let args = Array(CommandLine.arguments.dropFirst())
    if args == ["init"] || args == ["public-key"] {
        let key = try readKey(create: args == ["init"])
        print(key.publicKey.rawRepresentation.map { String(format: "%02x", $0) }.joined())
    } else if args.count == 3 && args[0] == "sign-manifest" {
        let input = URL(fileURLWithPath: args[1])
        guard input.lastPathComponent == "keefi-update.json" else { throw SigningError.message("Expected keefi-update.json") }
        let manifest = try Data(contentsOf: input)
        let key = try readKey(create: false)
        let signature = try key.signature(for: manifest)
        guard key.publicKey.isValidSignature(signature, for: manifest) else { throw SigningError.message("Signature verification failed") }
        let encoded = Data((signature.base64EncodedString() + "\n").utf8)
        try encoded.write(to: URL(fileURLWithPath: args[2]), options: .withoutOverwriting)
    } else {
        throw SigningError.message("Usage: keefi-release-sign init | public-key | sign-manifest keefi-update.json keefi-update.json.sig")
    }
} catch {
    FileHandle.standardError.write(Data(("\(error)\n").utf8))
    exit(1)
}
