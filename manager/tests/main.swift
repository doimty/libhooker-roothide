import Foundation
#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif

struct TestFailure: Error, CustomStringConvertible {
    let description: String
}

func expect(_ condition: @autoclosure () throws -> Bool, _ message: String, line: Int = #line) throws {
    if try !condition() { throw TestFailure(description: "line \(line): \(message)") }
}

func readPlist(_ url: URL) throws -> [String: Any] {
    let data = try Data(contentsOf: url)
    return try PropertyListSerialization.propertyList(from: data, options: [], format: nil) as! [String: Any]
}

func writePlist(_ value: Any, to url: URL) throws {
    let data = try PropertyListSerialization.data(fromPropertyList: value, format: .xml, options: 0)
    try data.write(to: url)
}

final class FailOneModeChange: FileManager, @unchecked Sendable {
    var fail = true
    override func setAttributes(_ attributes: [FileAttributeKey: Any], ofItemAtPath path: String) throws {
        if fail {
            fail = false
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(EACCES), userInfo: nil)
        }
        try super.setAttributes(attributes, ofItemAtPath: path)
    }
}

let fm = FileManager.default
let temporary = fm.temporaryDirectory.appendingPathComponent("lhc-preferences-tests-\(UUID().uuidString)", isDirectory: true)
try fm.createDirectory(at: temporary, withIntermediateDirectories: false)
// Only this newly-created test directory is ever removed.
defer { try? fm.removeItem(at: temporary) }
var passed = 0

func test(_ name: String, _ body: (URL) throws -> Void) throws {
    let directory = temporary.appendingPathComponent(name, isDirectory: true)
    try fm.createDirectory(at: directory, withIntermediateDirectories: false)
    try body(directory)
    passed += 1
    print("PASS: \(name)")
}

do {
    try test("opening-and-registering-never-writes") { directory in
        let url = directory.appendingPathComponent("not-created/org.coolstar.libhooker.plist")
        let store = LHUserDefaults(url: url)
        store.register(defaults: ["tweaksEnabled": true, "webProcessTweaks": true])
        try expect(store.bool(forKey: "tweaksEnabled"), "registered global default")
        try expect(store.bool(forKey: "webProcessTweaks"), "registered web default")
        try expect(store.synchronize(), "no-op synchronize succeeds")
        try expect(!fm.fileExists(atPath: url.deletingLastPathComponent().path), "opening must not create even a directory")
    }

    try test("existing-file-is-not-rewritten-on-open") { directory in
        let url = directory.appendingPathComponent("prefs.plist")
        try writePlist(["webProcessTweaks": false, "sentinel": "unchanged"], to: url)
        try fm.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
        let before = try Data(contentsOf: url)
        let beforeAttributes = try fm.attributesOfItem(atPath: url.path)
        let store = LHUserDefaults(url: url)
        store.register(defaults: ["webProcessTweaks": true, "tweaksEnabled": true])
        try expect(!store.bool(forKey: "webProcessTweaks"), "persisted false overrides registration")
        try expect(store.synchronize(), "no-op synchronize")
        let after = try Data(contentsOf: url)
        let afterAttributes = try fm.attributesOfItem(atPath: url.path)
        try expect(before == after, "original bytes preserved")
        try expect(beforeAttributes[.modificationDate] as? Date == afterAttributes[.modificationDate] as? Date, "mtime preserved")
        try expect((afterAttributes[.posixPermissions] as? NSNumber)?.intValue == 0o600, "no chmod before an edit")
    }

    try test("first-edit-is-atomic-0644-and-excludes-registration") { directory in
        let url = directory.appendingPathComponent("prefs.plist")
        let store = LHUserDefaults(url: url)
        store.register(defaults: ["tweaksEnabled": true, "webProcessTweaks": true])
        store.set(false, forKey: "tweaksEnabled")
        try expect(store.synchronize(), "first save")
        let result = try readPlist(url)
        let attributes = try fm.attributesOfItem(atPath: url.path)
        try expect(result["tweaksEnabled"] as? Bool == false, "global gate saved")
        try expect(result["webProcessTweaks"] == nil, "registration must never be persisted incidentally")
        try expect((attributes[.posixPermissions] as? NSNumber)?.intValue == 0o644, "engine-readable mode 0644")
        try expect(try fm.contentsOfDirectory(atPath: directory.path) == ["prefs.plist"], "no temporary file left")
        let reopened = LHUserDefaults(url: url)
        try expect(!reopened.bool(forKey: "tweaksEnabled"), "round-trip false")
    }

    try test("save-preserves-latest-unrelated-keys-and-deletions") { directory in
        let url = directory.appendingPathComponent("prefs.plist")
        try writePlist(["external": 1, "tweakconfigs": ["default": ["enableTweaks": false]]], to: url)
        let store = LHUserDefaults(url: url)
        try writePlist(["external": 2, "newExternalKey": "keep", "tweakconfigs": ["default": ["enableTweaks": false]]], to: url)
        store.set(true, forKey: "webProcessTweaks")
        store.set(nil, forKey: "tweakconfigs")
        try expect(store.synchronize(), "save after independent writer")
        let result = try readPlist(url)
        try expect(result["external"] as? Int == 2 && result["newExternalKey"] as? String == "keep", "latest other keys preserved")
        try expect(result["tweakconfigs"] == nil, "explicit deletion persisted")
    }

    try test("bad-plist-is-visible-and-never-clobbered") { directory in
        let url = directory.appendingPathComponent("prefs.plist")
        let broken = Data("not a plist".utf8)
        try broken.write(to: url)
        let store = LHUserDefaults(url: url)
        try expect(store.lastError != nil, "read failure exposed")
        store.register(defaults: ["tweaksEnabled": true])
        store.set(false, forKey: "tweaksEnabled")
        try expect(!store.synchronize(), "bad file blocks save")
        try expect(store.lastError != nil && store.bool(forKey: "tweaksEnabled"), "failed edit reverted")
        try expect(try Data(contentsOf: url) == broken, "bad file not replaced")
    }

    try test("non-dictionary-plist-is-visible") { directory in
        let url = directory.appendingPathComponent("prefs.plist")
        try writePlist(["array", "instead of dictionary"], to: url)
        let before = try Data(contentsOf: url)
        let store = LHUserDefaults(url: url)
        store.set(true, forKey: "tweaksEnabled")
        try expect(!store.synchronize() && store.lastError != nil, "top-level array rejected")
        try expect(try Data(contentsOf: url) == before, "non-dictionary file preserved")
    }

    try test("serialization-failure-keeps-the-old-file") { directory in
        let url = directory.appendingPathComponent("prefs.plist")
        try writePlist(["sentinel": "old"], to: url)
        let before = try Data(contentsOf: url)
        let store = LHUserDefaults(url: url)
        store.set(URL(fileURLWithPath: "/not-a-property-list-value"), forKey: "bad")
        try expect(!store.synchronize() && store.lastError != nil, "invalid property list value rejected")
        try expect(try Data(contentsOf: url) == before, "failed serialization preserves old bytes")
    }

    try test("atomic-mode-failure-preserves-old-bytes-and-reverts-edit") { directory in
        let url = directory.appendingPathComponent("prefs.plist")
        try writePlist(["tweaksEnabled": true], to: url)
        let before = try Data(contentsOf: url)
        let manager = FailOneModeChange()
        let store = LHUserDefaults(urlProvider: { url }, fileManager: manager)
        store.set(false, forKey: "tweaksEnabled")
        try expect(!store.synchronize() && store.lastError != nil, "chmod failure surfaced")
        try expect(store.bool(forKey: "tweaksEnabled"), "UI can read the old value after failure")
        try expect(try Data(contentsOf: url) == before, "failure before rename preserves old bytes")
        try expect(try fm.contentsOfDirectory(atPath: directory.path) == ["prefs.plist"], "failed temporary file removed")
        store.set(false, forKey: "tweaksEnabled")
        try expect(store.synchronize() && store.lastError == nil, "retry succeeds and clears error")
    }

    try test("current-preference-location-is-resolved-at-save") { directory in
        let first = directory.appendingPathComponent("first.plist")
        let second = directory.appendingPathComponent("second.plist")
        try writePlist(["root": "first"], to: first)
        try writePlist(["root": "second"], to: second)
        let original = try Data(contentsOf: first)
        var current = first
        let store = LHUserDefaults(urlProvider: { current })
        current = second
        store.set(false, forKey: "webProcessTweaks")
        try expect(store.synchronize(), "write through current resolver")
        try expect(try Data(contentsOf: first) == original, "old bootstrap left intact")
        let result = try readPlist(second)
        try expect(result["root"] as? String == "second", "new root's unrelated keys kept")
        try expect(result["webProcessTweaks"] as? Bool == false, "new root received edit")
    }

    try test("whole-record-precedence-and-built-in-fallback") { _ in
        let path = "/usr/sbin/exampled"
        let configs: [String: Any] = [
            "default": ["enableTweaks": false, "customConfig": true],
            "paths": [path: ["customConfig": false]],
            "bundles": ["example.app": [:] as [String: Any]]
        ]
        let bundle = LHProcessConfiguration.record(in: configs, bundle: "example.app", path: path)!
        try expect(bundle.isEmpty, "even empty bundle record wins as a whole")
        try expect((bundle["enableTweaks"] as? Bool ?? true), "missing field uses built-in true, not default false")
        let executable = LHProcessConfiguration.record(in: configs, bundle: "missing.bundle", path: path)!
        try expect(executable["customConfig"] as? Bool == false, "nonmatching bundle falls through to path")
        try expect(executable["enableTweaks"] == nil, "path fields not merged with default")
        let fallback = LHProcessConfiguration.record(in: configs, bundle: "missing.bundle", path: "/missing")!
        try expect(fallback["enableTweaks"] as? Bool == false, "default record fallback")
        try expect(LHProcessConfiguration.record(in: [:], bundle: "", path: "") == nil, "no record means built-ins")
    }

    try test("logical-path-normalization-and-inheritance-deletion") { _ in
        try expect(LHPaths.preferences == "/var/mobile/Library/Preferences/org.coolstar.libhooker.plist", "fixed domain independent of app identity")
        try expect(LHPaths.normalizeRootFSPath("/rootfs/System/Library/../Library/test") == "/System/Library/test", "system rootfs prefix normalized")
        try expect(LHPaths.normalizeRootFSPath("/usr/sbin/exampled") == "/usr/sbin/exampled", "bootstrap API logical result unchanged")
        try expect(LHPaths.normalizeRootFSPath("/rootfs-other/test") == "/rootfs-other/test", "prefix boundary respected")
        try expect(LHPaths.normalizeRootFSPath("/rootfs") == "/", "root normalized")
        var configs = LHProcessConfiguration.setting(["enableTweaks": false], in: [:], bundle: "", path: "/rootfs/usr/sbin/exampled")
        let paths = configs["paths"] as! [String: Any]
        try expect(paths["/usr/sbin/exampled"] != nil && paths["/rootfs/usr/sbin/exampled"] == nil, "only logical key persisted")
        configs["default"] = ["customConfig": false]
        configs = LHProcessConfiguration.setting(nil, in: configs, bundle: "", path: "/usr/sbin/exampled")
        let inherited = LHProcessConfiguration.record(in: configs, bundle: "", path: "/usr/sbin/exampled")!
        try expect(inherited["customConfig"] as? Bool == false, "removing path record restores inheritance")
        configs = LHProcessConfiguration.setting([:], in: configs, bundle: "example.app", path: "/usr/sbin/exampled")
        configs = LHProcessConfiguration.setting(nil, in: configs, bundle: "example.app", path: "/usr/sbin/exampled")
        try expect((configs["bundles"] as! [String: Any])["example.app"] == nil, "bundle override removed")
        try expect(configs["default"] != nil, "inherit action preserves default")
    }

    try test("exact-dylib-keys-and-allow-deny-round-trip") { directory in
        for valid in ["Tweak.dylib", "A.B.dylib", "Case Sensitive.dylib"] {
            try expect(LHProcessConfiguration.isTweakFilename(valid), "exact filename accepted: \(valid)")
        }
        for invalid in [".dylib", "Tweak.plist", "Tweak.dylib.bak", "/usr/lib/Tweak.dylib", "folder/Tweak.dylib"] {
            try expect(!LHProcessConfiguration.isTweakFilename(invalid), "non-filename rejected: \(invalid)")
        }
        let url = directory.appendingPathComponent("prefs.plist")
        let store = LHUserDefaults(url: url)
        for mode in [0, 1] {
            let record: [String: Any] = ["enableTweaks": true, "customConfig": true, "allowDeny": mode,
                                         "tweakConfigs": ["A.B.dylib": true, "Uninstalled.dylib": false]]
            store.set(LHProcessConfiguration.setting(record, in: [:], bundle: "example.app", path: ""), forKey: "tweakconfigs")
            try expect(store.synchronize(), "save mode \(mode)")
            let configs = try readPlist(url)["tweakconfigs"] as! [String: Any]
            let result = LHProcessConfiguration.record(in: configs, bundle: "example.app", path: "")!
            try expect(result["allowDeny"] as? Int == mode, "mode not reversed")
            let tweaks = result["tweakConfigs"] as! [String: Bool]
            try expect(tweaks["A.B.dylib"] == true && tweaks["Uninstalled.dylib"] == false && tweaks["A"] == nil, "full and orphan filename keys preserved")
        }
    }

    print("\(passed) Foundation preference/path contract tests passed")
} catch {
    fputs("FAIL: \(error)\n", stderr)
    exit(1)
}
