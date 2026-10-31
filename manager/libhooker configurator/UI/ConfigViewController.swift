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
    private var tweakListError: String?

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

    @objc private func enablePressed(sender: UISwitch) {
        enableTweaks = sender.isOn
        save()
    }

    @objc private func overrideConfigPressed(sender: UISwitch) {
        customConfig = sender.isOn
        save()
    }

    @objc private func useInheritedConfiguration() {
        guard let service = launchService else { return }
        let alert = UIAlertController(title: String(localizationKey: "Use Inherited Configuration"),
                                      message: String(localizationKey: "Remove this process override and use the next matching path or default record."),
                                      preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: String(localizationKey: "Yes"), style: .destructive) { _ in
            let defaults = LHUserDefaults.standard
            let configs = defaults.dictionary(forKey: "tweakconfigs") ?? [:]
            defaults.set(LHProcessConfiguration.setting(nil, in: configs, bundle: service.bundle, path: service.path),
                         forKey: "tweakconfigs")
            if !defaults.synchronize() { self.showPreferenceError() }
            self.fetch()
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
            cell.segment.selectedSegmentIndex = allowDeny
            cell.backgroundColor = ThemeManager.backgroundColour
            cell.saveFunc = { index in
                self.allowDeny = index
                self.save()
            }
            return cell
        }
        let cell = ConfigSwitch()
        cell.textLabel?.text = tweaksList[indexPath.row].name
        cell.textLabel?.textColor = ThemeManager.labelColour
        cell.textLabel?.adjustsFontSizeToFitWidth = true
        cell.control.isOn = tweaksList[indexPath.row].state
        cell.backgroundColor = ThemeManager.backgroundColour
        cell.saveFunc = { state in
            self.tweaksList[indexPath.row].state = state
            self.save()
        }
        return cell
    }

    override func tableView(_ tableView: UITableView, titleForFooterInSection section: Int) -> String? {
        switch section {
        case 0:
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

    private func fetch() {
        enableTweaks = true
        customConfig = false
        allowDeny = 1
        currentRecord = [:]
        tweakListError = nil
        if let service = launchService,
           let configs = LHUserDefaults.standard.dictionary(forKey: "tweakconfigs"),
           let record = LHProcessConfiguration.record(in: configs, bundle: service.bundle, path: service.path) {
            currentRecord = record
            enableTweaks = record["enableTweaks"] as? Bool ?? true
            customConfig = record["customConfig"] as? Bool ?? false
            allowDeny = (record["allowDeny"] as? Int) == 0 ? 0 : 1
        }
        var tweaks = (currentRecord["tweakConfigs"] as? [String: Bool] ?? [:])
            .filter { LHProcessConfiguration.isTweakFilename($0.key) }
        // Keep saved keys for temporarily uninstalled tweaks, even when directory reads fail.
        #if targetEnvironment(simulator)
        let filenames = ["Test1.dylib", "Test2.dylib"]
        #else
        var filenames: [String] = []
        do {
            filenames = try FileManager.default.contentsOfDirectory(atPath: LHPaths.jailbreak("/usr/lib/TweakInject"))
        } catch {
            tweakListError = String(localizationKey: "Could not read the installed tweak list:") + " " + error.localizedDescription
        }
        #endif
        for filename in filenames where LHProcessConfiguration.isTweakFilename(filename) {
            if tweaks[filename] == nil { tweaks[filename] = false }
        }
        tweaksList = tweaks.map { TweakConfig(name: $0.key, state: $0.value) }
            .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    private func save() {
        guard let service = launchService else { return }
        var record = currentRecord
        record["enableTweaks"] = enableTweaks
        record["customConfig"] = customConfig
        record["allowDeny"] = allowDeny // 0 = Allow selected only; 1 = Deny selected.
        record["tweakConfigs"] = Dictionary(uniqueKeysWithValues: tweaksList.map { ($0.name, $0.state) })
        let defaults = LHUserDefaults.standard
        let configs = defaults.dictionary(forKey: "tweakconfigs") ?? [:]
        defaults.set(LHProcessConfiguration.setting(record, in: configs, bundle: service.bundle, path: service.path),
                     forKey: "tweakconfigs")
        if !defaults.synchronize() {
            showPreferenceError()
            fetch()
        } else {
            currentRecord = record
        }
        tableView.reloadData()
    }
}
