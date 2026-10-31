//
//  LaunchServiceListView.swift
//  libhooker configurator
//
//  Created by Andromeda on 30/03/2021.
//  Copyright © 2021 coolstar. All rights reserved.
//

import UIKit

enum LaunchServiceFilter { case apps, daemons }

struct LaunchService: Hashable {
    let name: String
    let path: String
    let bundle: String

    static let SpringBoard = LaunchService(name: String(localizationKey: "SpringBoard"),
                                           path: "/System/Library/CoreServices/SpringBoard.app/SpringBoard",
                                           bundle: "com.apple.springboard")
    static let empty = LaunchService(name: String(localizationKey: "Default Configuration"), path: "", bundle: "")
}

class LaunchServiceListView: BaseTableViewController {
    public var serviceFilter: LaunchServiceFilter?
    private var services: [LaunchService] = []
    private var visibleServices: [LaunchService] = []
    private var listStatus: String?
    private let searchController = UISearchController()

    override func traitCollectionDidChange(_ previousTraitCollection: UITraitCollection?) {
        super.traitCollectionDidChange(previousTraitCollection)
        tableView.reloadData()
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        title = String(localizationKey: serviceFilter == .apps ? "Applications" : "Daemons")
        navigationItem.largeTitleDisplayMode = .never
        searchController.loadViewIfNeeded()
        searchController.searchResultsUpdater = self
        searchController.obscuresBackgroundDuringPresentation = false
        searchController.searchBar.enablesReturnKeyAutomatically = false
        searchController.searchBar.returnKeyType = .done
        searchController.searchBar.placeholder = String(localizationKey: "Search")
        searchController.searchBar.delegate = self
        definesPresentationContext = true
        navigationItem.searchController = searchController
        navigationItem.hidesSearchBarWhenScrolling = false
        listStatus = String(localizationKey: "Loading process list…")
        fetch()
    }

    override func numberOfSections(in tableView: UITableView) -> Int { 1 }
    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int { visibleServices.count }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = reusableCell(withStyle: .subtitle, reuseIdentifier: "ProcessCell")
        let service = visibleServices[indexPath.row]
        cell.textLabel?.text = service.name
        cell.detailTextLabel?.text = service.bundle.isEmpty ? service.path : service.bundle
        cell.textLabel?.textColor = ThemeManager.labelColour
        cell.detailTextLabel?.textColor = ThemeManager.labelColour
        cell.accessoryType = .disclosureIndicator
        return cell
    }

    override func tableView(_ tableView: UITableView, titleForFooterInSection section: Int) -> String? { listStatus }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        let controller: ConfigViewController
        if #available(iOS 13.0, *) { controller = ConfigViewController(style: .insetGrouped) }
        else { controller = ConfigViewController(style: .grouped) }
        controller.launchService = visibleServices[indexPath.row]
        navigationController?.pushViewController(controller, animated: true)
    }

    private func appService(_ app: LSApplicationProxy) -> LaunchService? {
        guard let name = app.localizedName(), let identifier = app.lhIdentifier(),
              let bundleURL = app.bundleURL(),
              let data = try? Data(contentsOf: bundleURL.appendingPathComponent("Info.plist")),
              let plist = (try? PropertyListSerialization.propertyList(from: data, options: [], format: nil)) as? [String: Any]
        else { return nil }
        if let tags = plist["SBAppTags"] as? [String], tags.contains("hidden") { return nil }
        if let visible = plist["SBIconVisibilityDefaultVisible"] as? Bool, !visible { return nil }
        let executable = plist["CFBundleExecutable"] as? String ?? ""
        let path = executable.isEmpty ? "" : LHPaths.logical(bundleURL.appendingPathComponent(executable).path)
        return LaunchService(name: name, path: path, bundle: identifier)
    }

    #if LHC_ROOTHIDE
    private func daemonServices() -> (services: [LaunchService], status: String) {
        let directories = ["/System/Library/LaunchDaemons", LHPaths.jailbreak("/Library/LaunchDaemons")]
        var byPath: [String: LaunchService] = [:]
        var problems: [String] = []
        var skipped = 0
        for directory in directories {
            let urls: [URL]
            do {
                urls = try FileManager.default.contentsOfDirectory(at: URL(fileURLWithPath: directory),
                                                                   includingPropertiesForKeys: nil, options: [.skipsHiddenFiles])
            } catch {
                problems.append(String(localizationKey: "Could not read daemon directory:") + " "
                                + LHPaths.logical(directory) + "\n" + error.localizedDescription)
                continue
            }
            for url in urls where url.pathExtension == "plist" {
                do {
                    let data = try Data(contentsOf: url)
                    guard let plist = try PropertyListSerialization.propertyList(from: data, options: [], format: nil) as? [String: Any],
                          let program = (plist["Program"] as? String) ?? (plist["ProgramArguments"] as? [String])?.first,
                          program.hasPrefix("/") else { skipped += 1; continue }
                    let path = LHPaths.logical(program)
                    let name = plist["Label"] as? String ?? url.deletingPathExtension().lastPathComponent
                    if byPath[path] == nil {
                        byPath[path] = LaunchService(name: name, path: path, bundle: "")
                    }
                } catch { skipped += 1 }
            }
        }
        var status = String(localizationKey: "Lists installed launch daemon plists, including services that are not currently running. Services sharing an executable share its configuration.")
        if byPath.isEmpty { status += "\n\n" + String(localizationKey: "No configurable daemons were found.") }
        if skipped > 0 {
            status += "\n\n" + String(localizationKey: "Some daemon plists could not be read or did not specify an absolute executable path:") + " \(skipped)"
        }
        if !problems.isEmpty { status += "\n\n" + problems.joined(separator: "\n\n") }
        return (Array(byPath.values), status)
    }
    #endif

    private func fetch() {
        let filter = serviceFilter
        DispatchQueue.global(qos: .userInitiated).async {
            var services: [LaunchService]
            var status: String?
            if filter == .apps {
                services = LSApplicationWorkspace.default().allInstalledApplications().compactMap { self.appService($0) }
                if services.isEmpty { status = String(localizationKey: "No visible applications were found. The application list may be unavailable.") }
            } else {
                #if LHC_ROOTHIDE
                let result = self.daemonServices()
                services = result.services
                status = result.status
                #else
                services = launchdList().map { LaunchService(name: $0[0], path: $0[1], bundle: "") }
                if services.isEmpty { status = String(localizationKey: "No configurable daemons were found.") }
                #endif
            }
            services.sort { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
            DispatchQueue.main.async {
                self.services = services
                self.listStatus = status
                self.updateSearchResults(for: self.searchController)
            }
        }
    }
}

extension LaunchServiceListView: UISearchBarDelegate, UISearchResultsUpdating {
    func updateSearchResults(for searchController: UISearchController) {
        let text = searchController.searchBar.text ?? ""
        visibleServices = text.isEmpty ? services : services.filter {
            $0.name.localizedCaseInsensitiveContains(text) || $0.bundle.localizedCaseInsensitiveContains(text)
                || $0.path.localizedCaseInsensitiveContains(text)
        }
        tableView.reloadData()
    }
}
