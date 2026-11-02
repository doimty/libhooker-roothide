//
//  ConfigViewController.swift
//  libhooker configurator
//
//  Created by Andromeda on 30/03/2021.
//  Copyright © 2021 coolstar. All rights reserved.
//

import UIKit

struct TweakConfig {
    let name: String // Exact filename, including .dylib, is the engine key.
    var state: Bool
}

class ConfigViewController: UITableViewController {
    private var enableTweaks = true
    private var customConfig = false
    private var allowDeny = 1
    public var launchService: LaunchService?
    private var tweaksList: [TweakConfig] = []
    private var currentRecord: [String: Any] = [:]
    private var filenames: [String] = []
    private var tweakListError: String?
    private var preferencesReady = false
    private var loading = false
    private var refreshPending = false
    private var foregroundObserver: NSObjectProtocol?
    private let scanQueue = DispatchQueue(label: "libhooker.tweak-list", qos: .userInitiated)

    deinit {
        if let observer = foregroundObserver { NotificationCenter.default.removeObserver(observer) }
    }

    override func traitCollectionDidChange(_ previousTraitCollection: UITraitCollection?) {
        super.traitCollectionDidChange(previousTraitCollection)
        tableView.reloadData()
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        title = launchService?.name
        navigationItem.largeTitleDisplayMode = .never
        if let service = launchService, !service.bundle.isEmpty || !service.path.isEmpty {
            navigationItem.rightBarButtonItem = UIBarButtonItem(title: String(localizationKey: "Inherit"),
                                                               style: .plain, target: self,
                                                               action: #selector(useInheritedConfiguration))
        }
        refreshControl = UIRefreshControl()
        refreshControl?.addTarget(self, action: #selector(fetch), for: .valueChanged)
        foregroundObserver = NotificationCenter.default.addObserver(forName: UIApplication.didBecomeActiveNotification,
                                                                    object: nil, queue: .main) { [weak self] _ in
            guard let self = self, self.isViewLoaded, self.view.window != nil,
                  self.navigationController?.topViewController === self else { return }
            self.fetch()
        }
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        fetch()
    }

    override func numberOfSections(in tableView: UITableView) -> Int {
        if !enableTweaks { return 1 }
        return customConfig ? 3 : 2
    }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        switch section {
        case 0: return 1
        case 1: return customConfig ? 2 : 1
        case 2: return tweaksList.count
        default: return 0
        }
    }

    override func tableView(_ tableView: UITableView, willDisplay cell: UITableViewCell, forRowAt indexPath: IndexPath) {
        cell.isUserInteractionEnabled = preferencesReady && !loading
        cell.contentView.alpha = preferencesReady && !loading ? 1 : 0.5
    }

    @objc private func enablePressed(sender: UISwitch) {
        guard preferencesReady && !loading else { return }
        enableTweaks = sender.isOn
        save()
    }

    @objc private func overrideConfigPressed(sender: UISwitch) {
        guard preferencesReady && !loading else { return }
        customConfig = sender.isOn
        save()
    }

    @objc private func useInheritedConfiguration() {
        guard preferencesReady && !loading, let service = launchService else { return }
        let alert = UIAlertController(title: String(localizationKey: "Use Inherited Configuration"),
                                      message: String(localizationKey: "Remove this process override and use the next matching path or default record."),
                                      preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: String(localizationKey: "Yes"), style: .destructive) { [weak self] _ in
            guard let self = self else { return }
            let defaults = LHUserDefaults.standard
            defaults.setProcessConfiguration(nil, bundle: service.bundle, path: service.path)
            if !defaults.synchronize() { self.showPreferenceError() }
            self.applySnapshot()
            self.tableView.reloadData()
        })
        alert.addAction(UIAlertAction(title: String(localizationKey: "Cancel"), style: .cancel))
        present(alert, animated: true)
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        if indexPath.section == 0 {
            let cell = SettingsSwitchTableViewCell()
            cell.textLabel?.text = String(localizationKey: "Enable Tweaks")
            cell.textLabel?.textColor = ThemeManager.labelColour
            cell.backgroundColor = ThemeManager.backgroundColour
            cell.control.isOn = enableTweaks
            cell.control.addTarget(self, action: #selector(enablePressed(sender:)), for: .valueChanged)
            return cell
        }
        if indexPath.section == 1 {
            if indexPath.row == 0 {
                let cell = SettingsSwitchTableViewCell()
                cell.textLabel?.text = String(localizationKey: "Custom Tweak Selection")
                cell.textLabel?.textColor = ThemeManager.labelColour
                cell.backgroundColor = ThemeManager.backgroundColour
                cell.control.isOn = customConfig
                cell.control.addTarget(self, action: #selector(overrideConfigPressed(sender:)), for: .valueChanged)
                return cell
            }
            let cell = SegmentedCell()
            cell.keys = [String(localizationKey: "Allow (0)"), String(localizationKey: "Deny (1)")]
            cell.segment.selectedSegmentIndex = min(max(allowDeny, 0), 1)
            cell.backgroundColor = ThemeManager.backgroundColour
            cell.saveFunc = { [weak self] index in
                guard let self = self, self.preferencesReady && !self.loading, (0...1).contains(index) else { return }
                self.allowDeny = index
                self.save()
            }
            return cell
        }
        let cell = ConfigSwitch()
        let tweak = tweaksList[indexPath.row]
        cell.textLabel?.text = tweak.name
        cell.textLabel?.textColor = ThemeManager.labelColour
        cell.textLabel?.adjustsFontSizeToFitWidth = true
        cell.control.isOn = tweak.state
        cell.backgroundColor = ThemeManager.backgroundColour
        cell.saveFunc = { [weak self] state in
            guard let self = self, self.preferencesReady && !self.loading,
                  let index = self.tweaksList.firstIndex(where: { $0.name == tweak.name }) else { return }
            self.tweaksList[index].state = state
            self.save()
        }
        return cell
    }

    override func tableView(_ tableView: UITableView, titleForFooterInSection section: Int) -> String? {
        switch section {
        case 0:
            if loading { return String(localizationKey: "Loading settings…") }
            if let error = LHUserDefaults.standard.lastError {
                return String(localizationKey: "Pull to refresh and retry reading settings.") + "\n" + error.localizedDescription
            }
            return String(localizationKey: "Disabling tweaks skips all tweaks in this process. A process override replaces the whole record: bundle, then executable path, then default, then built-in settings. Restart the target app or respring SpringBoard to apply changes.")
        case 1:
            var help = String(localizationKey: "With custom selection off, only the tweak's existing Filter applies. With it on, Allow (0) permits only selected tweaks; Deny (1) blocks selected tweaks. Filters still apply in both modes.")
            if let error = tweakListError { help += "\n\n" + error }
            return help
        case 2:
            return tweaksList.isEmpty ? String(localizationKey: "No tweak dylibs were found. An empty Allow list permits no tweaks.") : nil
        default: return nil
        }
    }

    // Both disk reads run off-main. Publish one complete snapshot and keep edits disabled until ready.
    @objc private func fetch() {
        guard !loading else { refreshPending = true; return }
        loading = true
        preferencesReady = false
        navigationItem.prompt = String(localizationKey: "Loading settings…")
        navigationItem.rightBarButtonItem?.isEnabled = false
        tableView.reloadData()
        var remaining = 2
        let finished: () -> Void = { [weak self] in
            remaining -= 1
            guard remaining == 0, let self = self else { return }
            self.loading = false
            if self.refreshPending {
                self.refreshPending = false
                self.fetch()
                return
            }
            self.navigationItem.prompt = nil
            self.navigationItem.rightBarButtonItem?.isEnabled = self.preferencesReady
            self.refreshControl?.endRefreshing()
            self.applySnapshot()
            self.tableView.reloadData()
        }
        LHUserDefaults.standard.reloadAsync { [weak self] success in
            self?.preferencesReady = success
            finished()
        }
        let path = LHPaths.jailbreak("/usr/lib/TweakInject")
        scanQueue.async {
            #if targetEnvironment(simulator)
            let result: Result<[String], Error> = .success(["Test1.dylib", "Test2.dylib"])
            #else
            let result = Result { try FileManager.default.contentsOfDirectory(atPath: path) }
            #endif
            DispatchQueue.main.async { [weak self] in
                if let self = self {
                    switch result {
                    case .success(let names):
                        self.filenames = names.filter(LHProcessConfiguration.isTweakFilename)
                        self.tweakListError = nil
                    case .failure(let error):
                        self.tweakListError = String(localizationKey: "Could not read the installed tweak list:") + " " + error.localizedDescription
                    }
                }
                finished()
            }
        }
    }

    private func applySnapshot() {
        let configs = LHUserDefaults.standard.dictionary(forKey: "tweakconfigs") ?? [:]
        if let service = launchService {
            currentRecord = LHProcessConfiguration.record(in: configs, bundle: service.bundle, path: service.path) ?? [:]
        } else { currentRecord = [:] }
        enableTweaks = currentRecord["enableTweaks"] as? Bool ?? true
        customConfig = currentRecord["customConfig"] as? Bool ?? false
        allowDeny = (currentRecord["allowDeny"] as? Int) == 0 ? 0 : 1
        // Keep saved entries for temporarily uninstalled tweaks, including on scan failures.
        var tweaks = (currentRecord["tweakConfigs"] as? [String: Bool] ?? [:])
            .filter { LHProcessConfiguration.isTweakFilename($0.key) }
        for filename in filenames where tweaks[filename] == nil { tweaks[filename] = false }
        tweaksList = tweaks.map { TweakConfig(name: $0.key, state: $0.value) }
            .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    private func save() {
        guard preferencesReady && !loading, let service = launchService else { return }
        var record = currentRecord
        record["enableTweaks"] = enableTweaks
        record["customConfig"] = customConfig
        record["allowDeny"] = allowDeny // 0 = Allow selected only; 1 = Deny selected.
        record["tweakConfigs"] = Dictionary(uniqueKeysWithValues: tweaksList.map { ($0.name, $0.state) })
        let defaults = LHUserDefaults.standard
        defaults.setProcessConfiguration(record, bundle: service.bundle, path: service.path)
        if !defaults.synchronize() { showPreferenceError() }
        applySnapshot() // No directory re-scan or second disk read after saving/resetting.
        tableView.reloadData()
    }
}
