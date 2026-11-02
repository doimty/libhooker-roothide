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
    private var foregroundObserver: NSObjectProtocol?
    private var isVisible = false
    private var isFetching = false
    private var refreshPending = false

    private struct ScanProblems {
        private var count = 0
        private var details: [String] = []

        mutating func append(_ detail: String) {
            count += 1
            if details.count < 5 { details.append(detail) }
        }

        func footer(title: String) -> String? {
            guard count > 0 else { return nil }
            var text = "\(title) \(count)"
            if count > details.count { text += "\n" + String(localizationKey: "Only the first 5 errors are shown.") }
            return text + "\n\n" + details.joined(separator: "\n\n")
        }
    }

    deinit {
        if let observer = foregroundObserver { NotificationCenter.default.removeObserver(observer) }
    }

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
        refreshControl = UIRefreshControl()
        refreshControl?.addTarget(self, action: #selector(fetch), for: .valueChanged)
        foregroundObserver = NotificationCenter.default.addObserver(forName: UIApplication.didBecomeActiveNotification,
                                                                    object: nil, queue: .main) { [weak self] _ in
            guard let self = self, self.isVisible, self.viewIfLoaded?.window != nil else { return }
            self.fetch()
        }
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        isVisible = true
        fetch()
    }

    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated)
        isVisible = false
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

    private static func appService(_ app: LSApplicationProxy, problems: inout ScanProblems) -> LaunchService? {
        let name = app.localizedName()
        let identifier = app.lhIdentifier()
        let bundleURL = app.bundleURL()
        let application = identifier ?? name ?? bundleURL?.lastPathComponent ?? String(localizationKey: "Unknown application")
        guard let bundleURL = bundleURL else {
            problems.append(application + "\n" + String(localizationKey: "Application bundle URL is unavailable."))
            return nil
        }
        let plistURL = bundleURL.appendingPathComponent("Info.plist")
        let source = application + " (" + LHPaths.logical(plistURL.path) + ")"
        do {
            let data = try Data(contentsOf: plistURL)
            guard let plist = try PropertyListSerialization.propertyList(from: data, options: [], format: nil) as? [String: Any] else {
                problems.append(source + "\n" + String(localizationKey: "Property list is not a dictionary."))
                return nil
            }
            if let tags = plist["SBAppTags"] as? [String], tags.contains("hidden") { return nil }
            if let visible = plist["SBIconVisibilityDefaultVisible"] as? Bool, !visible { return nil }
            guard let name = name, let identifier = identifier else {
                problems.append(source + "\n" + String(localizationKey: "Application name or identifier is unavailable."))
                return nil
            }
            let executable = plist["CFBundleExecutable"] as? String ?? ""
            let path = executable.isEmpty ? "" : LHPaths.logical(bundleURL.appendingPathComponent(executable).path)
            return LaunchService(name: name, path: path, bundle: identifier)
        } catch {
            problems.append(source + "\n" + error.localizedDescription)
            return nil
        }
    }

    #if LHC_ROOTHIDE
    private static func daemonServices() -> (services: [LaunchService], status: String) {
        let directories = ["/System/Library/LaunchDaemons", LHPaths.jailbreak("/Library/LaunchDaemons")]
        var byPath: [String: LaunchService] = [:]
        var problems = ScanProblems()
        for directory in directories {
            let urls: [URL]
            do {
                urls = try FileManager.default.contentsOfDirectory(at: URL(fileURLWithPath: directory),
                                                                   includingPropertiesForKeys: nil, options: [.skipsHiddenFiles])
                    .sorted {
                        let left = LHPaths.logical($0.path), right = LHPaths.logical($1.path)
                        return left == right ? $0.path < $1.path : left < right
                    }
            } catch {
                problems.append(String(localizationKey: "Could not read daemon directory:") + " "
                                + LHPaths.logical(directory) + "\n" + error.localizedDescription)
                continue
            }
            for url in urls where url.pathExtension == "plist" {
                do {
                    let data = try Data(contentsOf: url)
                    guard let plist = try PropertyListSerialization.propertyList(from: data, options: [], format: nil) as? [String: Any] else {
                        problems.append(LHPaths.logical(url.path) + "\n" + String(localizationKey: "Property list is not a dictionary."))
                        continue
                    }
                    guard let program = (plist["Program"] as? String) ?? (plist["ProgramArguments"] as? [String])?.first,
                          program.hasPrefix("/") else {
                        problems.append(LHPaths.logical(url.path) + "\n" + String(localizationKey: "Daemon plist does not specify an absolute executable path."))
                        continue
                    }
                    let path = LHPaths.logical(program)
                    let name = plist["Label"] as? String ?? url.deletingPathExtension().lastPathComponent
                    if byPath[path] == nil {
                        byPath[path] = LaunchService(name: name, path: path, bundle: "")
                    }
                } catch { problems.append(LHPaths.logical(url.path) + "\n" + error.localizedDescription) }
            }
        }
        var status = String(localizationKey: "Lists installed launch daemon plists, including services that are not currently running. Services sharing an executable share its configuration.")
        if byPath.isEmpty { status += "\n\n" + String(localizationKey: "No configurable daemons were found.") }
        if let errors = problems.footer(title: String(localizationKey: "Some daemon directories or plists were skipped:")) {
            status += "\n\n" + errors
        }
        return (Array(byPath.values), status)
    }
    #endif

    @objc private func fetch() {
        guard isVisible else { refreshControl?.endRefreshing(); return }
        guard !isFetching else { refreshPending = true; return }
        isFetching = true
        let filter = serviceFilter
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            var services: [LaunchService]
            var status: String?
            if filter == .apps {
                var problems = ScanProblems()
                services = LSApplicationWorkspace.default().allInstalledApplications().compactMap {
                    LaunchServiceListView.appService($0, problems: &problems)
                }
                if services.isEmpty { status = String(localizationKey: "No visible applications were found. The application list may be unavailable.") }
                if let errors = problems.footer(title: String(localizationKey: "Some applications were skipped:")) {
                    status = (status.map { $0 + "\n\n" } ?? "") + errors
                }
            } else {
                #if LHC_ROOTHIDE
                let result = LaunchServiceListView.daemonServices()
                services = result.services
                status = result.status
                #else
                services = launchdList().map { LaunchService(name: $0[0], path: $0[1], bundle: "") }
                if services.isEmpty { status = String(localizationKey: "No configurable daemons were found.") }
                #endif
            }
            services.sort {
                let comparison = $0.name.localizedCaseInsensitiveCompare($1.name)
                if comparison != .orderedSame { return comparison == .orderedAscending }
                return $0.path == $1.path ? $0.bundle < $1.bundle : $0.path < $1.path
            }
            DispatchQueue.main.async { [weak self] in
                guard let self = self else { return }
                self.isFetching = false
                guard self.isVisible else {
                    self.refreshPending = false
                    self.refreshControl?.endRefreshing()
                    return
                }
                if self.refreshPending {
                    self.refreshPending = false
                    self.fetch()
                    return
                }
                self.services = services
                self.listStatus = status
                self.updateSearchResults(for: self.searchController)
                self.refreshControl?.endRefreshing()
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
