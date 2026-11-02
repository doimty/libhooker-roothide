//
//  LHUserDefaults.swift
//  libhooker configurator
//
//  Created by CoolStar on 1/26/21.
//  Copyright © 2021 coolstar. All rights reserved.
//

import Foundation
#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#endif

enum LHPaths {
    // The engine's domain is stable even when the app's Bundle ID changes.
    static let preferences = "/var/mobile/Library/Preferences/org.coolstar.libhooker.plist"

    static func preferencesURL() -> URL {
        #if targetEnvironment(simulator)
        return FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("org.coolstar.libhooker.plist")
        #else
        return URL(fileURLWithPath: jailbreak(preferences))
        #endif
    }

    static func jailbreak(_ path: String) -> String {
        #if LHC_TESTING
        return path
        #else
        return LHRootPath(path)
        #endif
    }

    static func logical(_ path: String) -> String {
        guard !path.isEmpty else { return "" }
        #if LHC_TESTING
        return normalizeRootFSPath(path)
        #else
        return normalizeRootFSPath(LHRootFSPath(path))
        #endif
    }

    static func normalizeRootFSPath(_ path: String) -> String {
        let normalized = (path as NSString).standardizingPath
        // RootHide rootfs() represents paths outside the bootstrap under /rootfs.
        if normalized == "/rootfs" { return "/" }
        if normalized.hasPrefix("/rootfs/") {
            return String(normalized.dropFirst("/rootfs".count))
        }
        return normalized
    }
}

/// The engine chooses one whole record; fields are never merged with defaults.
enum LHProcessConfiguration {
    static func record(in configs: [String: Any], bundle: String, path: String) -> [String: Any]? {
        if !bundle.isEmpty,
           let bundles = configs["bundles"] as? [String: Any],
           let record = bundles[bundle] as? [String: Any] {
            return record
        }
        let logicalPath = LHPaths.logical(path)
        if !logicalPath.isEmpty,
           let paths = configs["paths"] as? [String: Any],
           let record = paths[logicalPath] as? [String: Any] {
            return record
        }
        return configs["default"] as? [String: Any]
    }

    static func setting(_ record: [String: Any]?, in configs: [String: Any],
                        bundle: String, path: String) -> [String: Any] {
        var result = configs
        if !bundle.isEmpty {
            var bundles = configs["bundles"] as? [String: Any] ?? [:]
            bundles[bundle] = record
            result["bundles"] = bundles
        } else if !path.isEmpty {
            var paths = configs["paths"] as? [String: Any] ?? [:]
            paths[LHPaths.logical(path)] = record
            result["paths"] = paths
        } else {
            result["default"] = record
        }
        return result
    }

    static func isTweakFilename(_ name: String) -> Bool {
        name.hasSuffix(".dylib") && name.count > ".dylib".count &&
            !name.contains("/") && !name.contains("\\") && !name.contains("\0")
    }
}

final class LHUserDefaults {
    // UI loads this store explicitly in the background; opening/registering never writes.
    static let standard = LHUserDefaults(loadImmediately: false)

    private let urlProvider: () -> URL
    private let fileManager: FileManager
    private let reloadQueue: DispatchQueue
    private var persisted: [String: Any] = [:]
    private var registered: [String: Any] = [:]
    private var pending: [String: Any] = [:]
    private var removed = Set<String>()
    private var processChanges: [(record: [String: Any]?, bundle: String, path: String)] = []
    private var generation: UInt = 0
    private var reloadCallbacks: [(Bool) -> Void] = []
    private(set) var isReloading = false
    private(set) var hasLoaded = false
    private(set) var lastError: Error?

    // Mutable state is owned by the calling/UI thread; only immutable snapshots cross queues.
    init(urlProvider: @escaping () -> URL = LHPaths.preferencesURL, fileManager: FileManager = .default,
         loadImmediately: Bool = true,
         reloadQueue: DispatchQueue = DispatchQueue(label: "libhooker.preferences.read", qos: .userInitiated)) {
        self.urlProvider = urlProvider
        self.fileManager = fileManager
        self.reloadQueue = reloadQueue
        if loadImmediately { reload() }
    }

    convenience init(url: URL) {
        self.init(urlProvider: { url })
    }

    // Registration is only a fallback for reads, never a persistent mutation.
    func register(defaults: [String: Any]) {
        registered.merge(defaults) { _, new in new }
    }

    private func object(forKey key: String) -> Any? {
        if let value = pending[key] { return value }
        if removed.contains(key) { return registered[key] }
        return persisted[key] ?? registered[key]
    }

    func bool(forKey key: String) -> Bool {
        object(forKey: key) as? Bool ?? false
    }

    func dictionary(forKey key: String) -> [String: Any]? {
        object(forKey: key) as? [String: Any]
    }

    func set(_ value: Any?, forKey key: String) {
        if key == "tweakconfigs" { processChanges.removeAll() }
        if let value = value {
            pending[key] = value
            removed.remove(key)
        } else {
            pending.removeValue(forKey: key)
            removed.insert(key)
        }
    }

    // Apply a single process edit to the freshly-read tree, preserving other processes.
    func setProcessConfiguration(_ record: [String: Any]?, bundle: String, path: String) {
        processChanges.append((record, bundle, path))
    }

    private static func read(at url: URL) throws -> [String: Any] {
        let data: Data
        do {
            data = try Data(contentsOf: url)
        } catch let error as NSError {
            if error.domain == NSCocoaErrorDomain && error.code == NSFileReadNoSuchFileError {
                return [:]
            }
            throw error
        }
        let value = try PropertyListSerialization.propertyList(from: data, options: [], format: nil)
        guard let dictionary = value as? [String: Any] else {
            throw NSError(domain: NSCocoaErrorDomain, code: NSPropertyListReadCorruptError,
                          userInfo: [NSLocalizedDescriptionKey: "The libhooker preferences must contain a dictionary."])
        }
        return dictionary
    }

    // Coalesce overlapping reads. A read started before a save must not overwrite that save.
    func reloadAsync(completion: @escaping (Bool) -> Void) {
        precondition(Thread.isMainThread)
        reloadCallbacks.append(completion)
        guard !isReloading else { return }
        isReloading = true
        let requestedGeneration = generation
        let url = urlProvider()
        reloadQueue.async {
            let result = Result { try Self.read(at: url) }
            DispatchQueue.main.async {
                if requestedGeneration == self.generation {
                    switch result {
                    case .success(let snapshot):
                        self.persisted = snapshot
                        self.lastError = nil
                    case .failure(let error):
                        self.lastError = error
                    }
                    self.hasLoaded = true
                }
                self.isReloading = false
                let callbacks = self.reloadCallbacks
                self.reloadCallbacks.removeAll()
                callbacks.forEach { $0(self.lastError == nil && self.hasLoaded) }
            }
        }
    }

    @discardableResult func reload() -> Bool {
        generation &+= 1
        hasLoaded = true
        do {
            persisted = try Self.read(at: urlProvider())
            lastError = nil
            return true
        } catch {
            lastError = error
            return false
        }
    }

    /// A no-op until an explicit edit. Failed saves discard the edit so the UI can revert.
    @discardableResult func synchronize() -> Bool {
        guard !pending.isEmpty || !removed.isEmpty || !processChanges.isEmpty else { return lastError == nil }
        generation &+= 1
        hasLoaded = true
        defer {
            pending.removeAll()
            removed.removeAll()
            processChanges.removeAll()
        }
        do {
            let url = urlProvider() // Resolve the current bootstrap through the API on each save.
            var latest = try Self.read(at: url)
            persisted = latest
            for key in removed { latest.removeValue(forKey: key) }
            latest.merge(pending) { _, new in new }
            for change in processChanges {
                let configs = latest["tweakconfigs"] as? [String: Any] ?? [:]
                latest["tweakconfigs"] = LHProcessConfiguration.setting(change.record, in: configs,
                                                                       bundle: change.bundle, path: change.path)
            }
            let data = try PropertyListSerialization.data(fromPropertyList: latest, format: .binary, options: 0)
            try atomicWrite(data, to: url)
            persisted = latest
            lastError = nil
            return true
        } catch {
            lastError = error
            return false
        }
    }

    private func atomicWrite(_ data: Data, to url: URL) throws {
        let directory = url.deletingLastPathComponent()
        try fileManager.createDirectory(at: directory, withIntermediateDirectories: true,
                                        attributes: [.posixPermissions: 0o755])
        let temporary = directory.appendingPathComponent(".libhooker-\(UUID().uuidString).tmp")
        defer { try? fileManager.removeItem(at: temporary) }
        // The temporary file is on the same filesystem. Set mode before the atomic rename.
        try data.write(to: temporary, options: .withoutOverwriting)
        try fileManager.setAttributes([.posixPermissions: 0o644], ofItemAtPath: temporary.path)
        let handle = try FileHandle(forWritingTo: temporary)
        defer { handle.closeFile() }
        guard fsync(handle.fileDescriptor) == 0 else {
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno), userInfo: nil)
        }
        guard rename(temporary.path, url.path) == 0 else {
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno), userInfo: nil)
        }
    }
}
