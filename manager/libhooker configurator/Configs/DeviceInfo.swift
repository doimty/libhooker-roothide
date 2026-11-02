//
//  DeviceInfo.swift
//  libhooker configurator
//
//  Created by CoolStar on 10/20/20.
//  Copyright © 2020 coolstar. All rights reserved.
//
import Foundation
import UIKit
#if !LHC_ROOTHIDE
import CommonCrypto
#endif

public class DeviceInfo {
    static let shared = DeviceInfo()
    
    func iOSVersion() -> String {
        UIDevice.current.systemVersion
    }
    
    #if !LHC_ROOTHIDE
    var cachedData: [String: String] = [:]
    func getJailbreakVersion(sha1: String) -> String? {
        if cachedData.isEmpty {
            return "..."
        }
        return cachedData[sha1]
    }
    
    func loadRemoteJailbreakData(callback: @escaping () -> Void) {
        if !cachedData.isEmpty {
            return
        }
        guard let url = URL(string: "https://repo.theodyssey.dev/jbd-vers.plist") else {
            return
        }
        URLSession.shared.dataTask(with: url) { data, _, _ in
            if let data = data {
                if let plist = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil) as? [String: String] {
                    self.cachedData = plist
                    callback()
                }
            }
        }.resume()
    }
    
    #endif

    func getJailbreakName() -> String {
        #if targetEnvironment(simulator)
        return "Simulator"
        #elseif LHC_ROOTHIDE
        if let version = packagesList["roothide"]?.version {
            return "Dopamine RootHide \(version)"
        }
        return "Dopamine RootHide"
        #else
        guard !detectOdysseyRa1n() else {
            return "Odysseyra1n"
        }
        let odysseyJbd = "/odyssey/jailbreakd"
        let taurineJbd = "/taurine/jailbreakd"
        let chimeraJbd = "/chimera/jailbreakd"
        
        let jbdSHA1: String
        let jailbreakName: String
        if FileManager.default.fileExists(atPath: odysseyJbd) {
            jailbreakName = "Odyssey"
            jbdSHA1 = sha1File(url: URL(fileURLWithPath: odysseyJbd))
        } else if FileManager.default.fileExists(atPath: taurineJbd) {
            jailbreakName = "Taurine"
            jbdSHA1 = sha1File(url: URL(fileURLWithPath: taurineJbd))
        } else if FileManager.default.fileExists(atPath: chimeraJbd) {
            jailbreakName = "Chimera"
            jbdSHA1 = sha1File(url: URL(fileURLWithPath: chimeraJbd))
        } else {
            jailbreakName = "Unknown"
            jbdSHA1 = ""
        }
        if jailbreakName != "Unknown" {
            if let version = getJailbreakVersion(sha1: jbdSHA1) {
                return "\(jailbreakName) \(version)"
            }
        }
        return jailbreakName
        #endif
    }
    
    // All snapshot state and callbacks belong to the main queue. Getters never start I/O.
    private var packagesList: [String: Package] = [:]
    private var packagesListLoaded = false
    private var packagesListLoading = false
    private var packageCallbacks: [() -> Void] = []
    private(set) var packagesListError: Error?
    private let packagesListQueue = DispatchQueue(label: "libhooker.packagesList", qos: .userInitiated)

    func refreshPackagesList(completion: @escaping () -> Void) {
        precondition(Thread.isMainThread)
        packageCallbacks.append(completion)
        guard !packagesListLoading else { return }
        packagesListLoading = true
        packagesListQueue.async {
            let result = Result { try PackageListManager.shared.loadPackagesList() }
            DispatchQueue.main.async {
                switch result {
                case .success(let fresh):
                    self.packagesList = fresh
                    self.packagesListError = nil
                case .failure(let error):
                    self.packagesListError = error // Keep the last good snapshot on failure.
                }
                self.packagesListLoaded = true
                self.packagesListLoading = false
                let callbacks = self.packageCallbacks
                self.packageCallbacks.removeAll()
                callbacks.forEach { $0() }
            }
        }
    }

    func libhookerVersion() -> String {
        #if targetEnvironment(simulator)
        return "1.4.0"
        #else
        guard packagesListLoaded else { return "…" }
        return packagesList["org.coolstar.libhooker"]?.version ?? "Unknown"
        #endif
    }
    
    #if !LHC_ROOTHIDE
    private func detectOdysseyRa1n() -> Bool {
        isUnionMountPresent()
    }

    private func sha1File(url: URL) -> String {
        let bufferSize = 16 * 1024
        do {
            let file = try FileHandle(forReadingFrom: url)
            defer {
                file.closeFile()
            }
            
            var context = CC_SHA1_CTX()
            CC_SHA1_Init(&context)
            while autoreleasepool(invoking: {
                let data = file.readData(ofLength: bufferSize)
                if !data.isEmpty {
                    data.withUnsafeBytes {
                        _ = CC_SHA1_Update(&context, $0.baseAddress, CC_LONG(data.count))
                    }
                    return true
                } else {
                    return false
                }
            }) {}
            
            var digest = [UInt8](repeating: 0, count: Int(CC_SHA1_DIGEST_LENGTH))
            _ = CC_SHA1_Final(&digest, &context)
            
            return digest.map { String(format: "%02hhx", $0) }.joined()
        } catch {
            return ""
        }
    }
    #endif
}
