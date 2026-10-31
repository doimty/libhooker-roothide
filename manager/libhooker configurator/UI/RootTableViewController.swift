//
//  RootTableViewController.swift
//  libhooker configurator
//
//  Created by Andromeda on 30/03/2021.
//  Copyright © 2021 coolstar. All rights reserved.
//

import UIKit

class RootTableViewController: BaseTableViewController {
    private enum GlobalRow { case tweaks, oldABI, web, compatibility, defaults, reset }
    private var globalRows: [GlobalRow] {
        #if LHC_ROOTHIDE
        return [.tweaks, .web, .defaults, .reset]
        #else
        return Self.showOldABI ? [.tweaks, .oldABI, .web, .compatibility, .defaults, .reset]
                               : [.tweaks, .web, .compatibility, .defaults, .reset]
        #endif
    }
    private var reportedLoadError = false

    override func viewDidLoad() {
        super.viewDidLoad()
        title = String(localizationKey: "libhooker")
        navigationItem.largeTitleDisplayMode = .automatic
        navigationItem.rightBarButtonItem = UIBarButtonItem(title: String(localizationKey: "Apply"), style: .done,
                                                            target: self, action: #selector(showAlert(_:)))
        #if !LHC_ROOTHIDE
        DeviceInfo.shared.loadRemoteJailbreakData {
            DispatchQueue.main.async { self.tableView.reloadData() }
        }
        #endif
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        if !reportedLoadError && LHUserDefaults.standard.lastError != nil {
            reportedLoadError = true
            showPreferenceError()
        }
        tableView.reloadData()
    }

    override func traitCollectionDidChange(_ previousTraitCollection: UITraitCollection?) {
        super.traitCollectionDidChange(previousTraitCollection)
        tableView.reloadData()
    }

    @objc private func showAlert(_ sender: Any) {
        #if LHC_ROOTHIDE
        let alert = UIAlertController(title: String(localizationKey: "Apply Changes"),
                                      message: String(localizationKey: "Settings are saved when edited. Restart the target app to apply them. For SpringBoard, respring. For a userspace reboot, use Dopamine. Running processes do not reload these settings live."),
                                      preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: String(localizationKey: "Ok"), style: .default))
        #else
        let alert = UIAlertController(title: String(localizationKey: "Apply Changes"), message: nil, preferredStyle: .actionSheet)
        alert.addAction(UIAlertAction(title: String(localizationKey: "Respring"), style: .default) { _ in respring() })
        alert.addAction(UIAlertAction(title: userspaceRebootSupported() ? String(localizationKey: "Reboot Userspace") : String(localizationKey: "ldRestart"),
                                      style: .destructive) { _ in userspaceReboot() })
        alert.addAction(UIAlertAction(title: String(localizationKey: "Cancel"), style: .cancel))
        alert.popoverPresentationController?.barButtonItem = sender as? UIBarButtonItem
        #endif
        present(alert, animated: true)
    }

    #if !LHC_ROOTHIDE
    private static var showOldABI: Bool = {
        if #available(iOS 14.5, *) {
            var subtype: cpu_subtype_t = 0
            var length = MemoryLayout<cpu_subtype_t>.size
            sysctlbyname("hw.cpusubtype", &subtype, &length, nil, 0)
            return subtype == CPU_SUBTYPE_ARM64E
        }
        return false
    }()
    #endif

    override func numberOfSections(in tableView: UITableView) -> Int { 3 }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        switch section {
        case 0: return 3
        case 1: return globalRows.count
        case 2: return 3
        default: return 0
        }
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        if indexPath.section == 0 {
            let cell = reusableCell(withStyle: .value1, reuseIdentifier: "ValueCell")
            cell.selectionStyle = .none
            cell.textLabel?.textColor = ThemeManager.labelColour
            cell.detailTextLabel?.textColor = ThemeManager.labelColour
            switch indexPath.row {
            case 0:
                cell.textLabel?.text = String(localizationKey: "Version")
                cell.detailTextLabel?.text = DeviceInfo.shared.libhookerVersion()
            case 1:
                cell.textLabel?.text = String(localizationKey: "Jailbreak")
                cell.detailTextLabel?.text = DeviceInfo.shared.getJailbreakName()
            default:
                cell.textLabel?.text = String(localizationKey: "iOS")
                cell.detailTextLabel?.text = DeviceInfo.shared.iOSVersion()
            }
            return cell
        }
        if indexPath.section == 1 {
            switch globalRows[indexPath.row] {
            case .tweaks:
                #if LHC_ROOTHIDE
                let cell = SettingsSwitchTableViewCell()
                cell.defaultKey = "tweaksEnabled"
                #else
                let cell = TweaksEnabledSwitch()
                #endif
                cell.textLabel?.text = String(localizationKey: "Tweaks")
                cell.presentVC = self
                cell.textLabel?.textColor = ThemeManager.labelColour
                cell.backgroundColor = ThemeManager.backgroundColour
                return cell
            case .web:
                let cell = SettingsSwitchTableViewCell()
                cell.textLabel?.text = String(localizationKey: "Allow tweaks in webpages")
                cell.defaultKey = "webProcessTweaks"
                cell.presentVC = self
                cell.textLabel?.textColor = ThemeManager.labelColour
                cell.backgroundColor = ThemeManager.backgroundColour
                return cell
            case .oldABI:
                #if !LHC_ROOTHIDE
                let cell = OldABIEnabledSwitch()
                cell.textLabel?.text = String(localizationKey: "Old ABI Patch")
                cell.presentVC = self
                cell.textLabel?.textColor = ThemeManager.labelColour
                cell.backgroundColor = ThemeManager.backgroundColour
                return cell
                #else
                return UITableViewCell()
                #endif
            case .compatibility, .defaults, .reset:
                let cell = reusableCell(withStyle: .default, reuseIdentifier: "DefaultCell")
                let row = globalRows[indexPath.row]
                let key: String
                switch row {
                case .compatibility: key = "Tweak Compatibility"
                case .defaults: key = "Default Configuration"
                default: key = "Reset Configuration"
                }
                cell.textLabel?.text = String(localizationKey: key)
                cell.textLabel?.textColor = row == .reset ? .systemRed : ThemeManager.labelColour
                cell.accessoryType = row == .reset ? .none : .disclosureIndicator
                return cell
            }
        }
        let cell = reusableCell(withStyle: .default, reuseIdentifier: "DefaultCell")
        cell.textLabel?.text = String(localizationKey: ["SpringBoard", "Applications", "Daemons"][indexPath.row])
        cell.textLabel?.textColor = ThemeManager.labelColour
        cell.accessoryType = .disclosureIndicator
        return cell
    }

    override func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        switch section {
        case 1: return String(localizationKey: "Global Configuration")
        case 2: return String(localizationKey: "Process Configuration")
        default: return nil
        }
    }

    override func tableView(_ tableView: UITableView, titleForFooterInSection section: Int) -> String? {
        #if LHC_ROOTHIDE
        if section == 1 {
            return String(localizationKey: "The global Tweaks switch disables all tweak injection. Webpage tweaks also require this switch. Changes take effect when each process restarts.")
        }
        #endif
        return nil
    }

    private func showConfiguration(for service: LaunchService) {
        let controller: ConfigViewController
        if #available(iOS 13.0, *) { controller = ConfigViewController(style: .insetGrouped) }
        else { controller = ConfigViewController(style: .grouped) }
        controller.launchService = service
        navigationController?.pushViewController(controller, animated: true)
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        if indexPath.section == 0 { return }
        if indexPath.section == 1 {
            switch globalRows[indexPath.row] {
            case .compatibility:
                #if !LHC_ROOTHIDE
                let controller: TweakCompatibilityList
                if #available(iOS 13.0, *) { controller = TweakCompatibilityList(style: .insetGrouped) }
                else { controller = TweakCompatibilityList(style: .grouped) }
                navigationController?.pushViewController(controller, animated: true)
                #endif
            case .defaults: showConfiguration(for: .empty)
            case .reset:
                let alert = UIAlertController(title: String(localizationKey: "Reset Configuration"),
                                              message: String(localizationKey: "Tweak configurations for all processes will be reset"),
                                              preferredStyle: .alert)
                alert.addAction(UIAlertAction(title: String(localizationKey: "Yes"), style: .destructive) { _ in
                    LHUserDefaults.standard.set(nil, forKey: "tweakconfigs")
                    #if !LHC_ROOTHIDE
                    LHUserDefaults.standard.set(nil, forKey: "memPrefs")
                    #endif
                    if !LHUserDefaults.standard.synchronize() { self.showPreferenceError() }
                })
                alert.addAction(UIAlertAction(title: String(localizationKey: "No"), style: .cancel))
                present(alert, animated: true)
            default: break
            }
            return
        }
        if indexPath.row == 0 { showConfiguration(for: .SpringBoard); return }
        let controller: LaunchServiceListView
        if #available(iOS 13.0, *) { controller = LaunchServiceListView(style: .insetGrouped) }
        else { controller = LaunchServiceListView(style: .grouped) }
        controller.serviceFilter = indexPath.row == 1 ? .apps : .daemons
        navigationController?.pushViewController(controller, animated: true)
    }
}

extension UIViewController {
    func showPreferenceError() {
        let detail = LHUserDefaults.standard.lastError?.localizedDescription ?? String(localizationKey: "Unknown error")
        let alert = UIAlertController(title: String(localizationKey: "Could Not Save or Read Settings"),
                                      message: String(localizationKey: "Settings could not be read or saved. Check the preference file's permissions or contents, then try again. Failed edits are reverted.") + "\n\n" + detail,
                                      preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: String(localizationKey: "Ok"), style: .default))
        present(alert, animated: true)
    }
}

class BaseTableViewController: UITableViewController {
    func reusableCell(withStyle style: UITableViewCell.CellStyle, reuseIdentifier: String) -> UITableViewCell {
        reusableCell(withStyle: style, reuseIdentifier: reuseIdentifier, cellClass: UITableViewCell.self)
    }

    func reusableCell(withStyle style: UITableViewCell.CellStyle, reuseIdentifier: String, cellClass: AnyClass) -> UITableViewCell {
        var cell = tableView.dequeueReusableCell(withIdentifier: reuseIdentifier)
        if cell == nil {
            let type = cellClass as? UITableViewCell.Type ?? UITableViewCell.self
            cell = type.init(style: style, reuseIdentifier: reuseIdentifier)
            cell?.selectionStyle = .gray
        }
        cell?.backgroundColor = ThemeManager.backgroundColour
        return cell ?? UITableViewCell()
    }
}
