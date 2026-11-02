#!/usr/bin/env python3
"""Verify the optional GUI package, independently from the engine package."""
from pathlib import Path
import io
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

if len(sys.argv)!=2:
    raise SystemExit('Usage: verify-configurator-package.py package.deb')
package=Path(sys.argv[1]).resolve()
dpkg=shutil.which('dpkg-deb')
assert dpkg, 'dpkg-deb missing'
metadata=subprocess.check_output([dpkg,'-f',str(package)],text=True)
fields=dict(line.split(': ',1) for line in metadata.splitlines() if ': ' in line and not line.startswith(' '))
assert fields['Package']=='org.coolstar.libhooker-configurator'
assert fields['Architecture']=='iphoneos-arm64e'
assert 'org.coolstar.libhooker (>= 1.6.9-23+roothide)' in fields['Depends']
assert 'roothide' in fields['Depends'].split(', ')
assert 'firmware (>= 15.0)' in fields['Depends']
assert 'firmware (<< 18.0)' in fields['Depends']
assert not fields.get('Conflicts') and not fields.get('Replaces') and not fields.get('Provides')

payload=subprocess.check_output([dpkg,'--fsys-tarfile',str(package)])
with tarfile.open(fileobj=io.BytesIO(payload),mode='r:') as archive:
    for member in archive:
        name=Path(member.name).as_posix().removeprefix('./')
        assert not name.startswith('/') and '..' not in Path(name).parts, name
        assert not member.mode & 0o6000, ('privileged mode',name)
        assert 'giveMeRoot' not in name, name
        assert not name.startswith(('var/','Library/LaunchDaemons/','usr/lib/')),name
        assert '.jbroot-' not in name,name

Path('worker').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='verify-configurator-',dir='worker') as temp:
    root=Path(temp).resolve()
    subprocess.run([dpkg,'-x',str(package),str(root)],check=True)
    subprocess.run([dpkg,'-e',str(package),str(root/'DEBIAN')],check=True)
    app=root/'Applications/libhooker.app'
    assert app.is_dir()
    with (app/'Info.plist').open('rb') as file:
        info=plistlib.load(file)
    assert info['CFBundleExecutable']=='libhooker'
    assert info['CFBundleIdentifier']=='org.coolstar.libhooker-configurator'
    assert info['MinimumOSVersion']=='15.0'
    assert info['UIRequiredDeviceCapabilities']==['arm64']
    assert (app/'zh-Hans.lproj/Localizable.strings').is_file()
    binary=app/info['CFBundleExecutable']
    architectures=subprocess.check_output(['xcrun','lipo','-archs',str(binary)],text=True).split()
    assert architectures==['arm64'],architectures
    commands=subprocess.check_output(['xcrun','dyld_info','-load_commands',str(binary)],text=True)
    assert re.findall(r'min-OS:\s+(\S+)',commands)==['15.0']
    dependencies=subprocess.check_output(['xcrun','dyld_info','-linked_dylibs',str(binary)],text=True)
    assert '@loader_path/.jbroot/usr/lib/libroothide.dylib' in dependencies
    assert 'libjoker.dylib' not in dependencies and 'libellekit.dylib' not in dependencies
    assert not (app/'giveMeRoot').exists()
    entitlements=subprocess.check_output(['ldid','-e',str(binary)])
    entitlement_data=plistlib.loads(entitlements)
    assert entitlement_data.get('com.apple.private.security.no-container') is True
    assert not entitlement_data.get('com.apple.private.reboot')
    license_file=root/'usr/share/doc/org.coolstar.libhooker-configurator/copyright'
    assert license_file.read_bytes()==Path('manager/LICENSE').read_bytes()
    assert not any((root/'DEBIAN'/name).exists() for name in ('postinst','preinst','postrm','prerm'))
    assert not (root/'var/mobile/Library/Preferences').exists()
print('PASS optional Configurator package: metadata, iOS15 app, dependency, localization, license and no privileged helper')
