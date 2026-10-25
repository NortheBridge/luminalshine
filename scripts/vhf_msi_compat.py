"""Validate the explicit additive VHF MSI migration against the pre-VHF baseline.

Existing rows remain byte-for-byte subject to the compatibility oracle. This
adapter accepts only the named new files, directories and installation actions;
once a Windows-generated baseline includes them, ordinary strict comparison
resumes. No existing component GUID or service identity is normalized here.
"""
import re
from gen_wix_files import FileEntry, make_ids

PACKAGE_FILES = (
    'drivers/vhf-gamepad/install.ps1', 'drivers/vhf-gamepad/manifest.json',
    'drivers/vhf-gamepad/driver/VibeshineVhfGamepad.inf',
    'drivers/vhf-gamepad/driver/VibeshineVhfGamepad.dll',
    'drivers/vhf-gamepad/driver/VibeshineVhfGamepad.cat',
    'drivers/vhf-gamepad/tools/VibeshineVhfGamepadDeviceSetup.exe',
)
LICENSE = 'licenses/libvirtualgamepad-MIT.txt'
CONDITION = 'NOT REMOVE AND INSTALL_VHF_GAMEPAD = "1" AND&CM_C_application=3'
COMMAND = '"[POWERSHELL_PATH]" -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass -File "[INSTALL_ROOT]drivers\\vhf-gamepad\\install.ps1"'
ACTIONS = {
    'SetInstallVhfGamepad': {'Type': '51', 'Source': 'InstallVhfGamepad', 'Target': COMMAND, 'ExtendedType': ''},
    'InstallVhfGamepad': {'Type': '3137', 'Source': 'Wix4UtilCA_X64', 'Target': 'WixQuietExec', 'ExtendedType': ''},
}


def cells(row):
    return dict(part.split('=', 1) for part in row.split('|') if '=' in part)


def file_ids(path):
    *dirs, name = path.split('/')
    return make_ids(FileEntry('application', dirs, name, ''))


def strip_reviewed_vhf_additions(golden, candidate):
    """Return candidate without validated additions, or raise ValueError."""
    if any(cells(r).get('Property') == 'INSTALL_VHF_GAMEPAD' for r in golden.get('Property', [])):
        return candidate  # A real Windows rebaseline owns these rows now.
    bundled = any(cells(r).get('Property') == 'INSTALL_VHF_GAMEPAD' for r in candidate.get('Property', []))
    license_id = file_ids(LICENSE)[0]
    new_license = (any(cells(r).get('Component') == license_id for r in candidate.get('Component', [])) and
                   not any(cells(r).get('Component') == license_id for r in golden.get('Component', [])))
    if not bundled and not new_license:
        return candidate
    result = {name: list(rows) for name, rows in candidate.items()}

    def take(table, key, value):
        matches = [r for r in result.get(table, []) if cells(r).get(key) == value]
        if len(matches) != 1:
            raise ValueError(f'VHF migration requires exactly one {table}.{key}={value}')
        result[table].remove(matches[0])
        return cells(matches[0]), matches[0]

    directories = {}
    paths = list(PACKAGE_FILES) if bundled else []
    if new_license:
        paths.append(LICENSE)
    old_guids = {cells(r).get('ComponentId') for r in golden.get('Component', [])}
    new_guids = set()
    for path in paths:
        component, file_id, directory = file_ids(path)
        row, _ = take('Component', 'Component', component)
        guid = row.pop('ComponentId', '')
        if (not re.fullmatch(r'\{[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}\}', guid) or
                guid.upper() in {g.upper() for g in old_guids if g} or guid.upper() in new_guids):
            raise ValueError(f'Invalid/reused new VHF component GUID for {path}')
        new_guids.add(guid.upper())
        if row != {'Component': component, 'Directory_': directory, 'Attributes': '256', 'Condition': '', 'KeyPath': file_id}:
            raise ValueError(f'Unexpected VHF component attributes: {path}')
        binding, _ = take('FeatureComponents', 'Component_', component)
        if binding != {'Feature_': 'CM_C_application', 'Component_': component}:
            raise ValueError(f'VHF file outside application feature: {path}')
        parts = path.split('/')[:-1]
        for count in range(1, len(parts) + 1):
            name = 'CM_DP_application.' + '.'.join(p.replace('-', '_') for p in parts[:count])
            parent = 'INSTALL_ROOT' if count == 1 else 'CM_DP_application.' + '.'.join(p.replace('-', '_') for p in parts[:count-1])
            directories[name] = (parent, parts[count-1])
    for name, (parent, basename) in directories.items():
        # If a future baseline already owns a shared parent, compare it normally.
        if any(cells(r).get('Directory') == name for r in golden.get('Directory', [])):
            continue
        row, raw = take('Directory', 'Directory', name)
        default_dir = raw.split('DefaultDir=', 1)[-1]
        if (row.get('Directory_Parent') != parent or
                default_dir.split('|')[-1] != basename or
                len(default_dir.split('|')) > 2 or set(row) != {'Directory', 'Directory_Parent', 'DefaultDir'}):
            raise ValueError(f'Unexpected VHF directory placement: {name}')
    if bundled:
        prop, _ = take('Property', 'Property', 'INSTALL_VHF_GAMEPAD')
        if prop != {'Property': 'INSTALL_VHF_GAMEPAD', 'Value': '1'}:
            raise ValueError('Unexpected VHF installation default')
        secure, raw = take('Property', 'Property', 'SecureCustomProperties')
        names = secure.get('Value', '').split(';')
        if names.count('INSTALL_VHF_GAMEPAD') != 1:
            raise ValueError('VHF opt-out property must be Secure exactly once')
        names.remove('INSTALL_VHF_GAMEPAD')
        result['Property'].append('Property=SecureCustomProperties|Value=' + ';'.join(names))
        for name, expected in ACTIONS.items():
            row, _ = take('CustomAction', 'Action', name)
            if row != {'Action': name, **expected}:
                raise ValueError(f'Unexpected VHF custom action: {name}')
        sequence = sorted(candidate.get('InstallExecuteSequence', []), key=lambda r: int(cells(r).get('Sequence') or 0))
        names = [cells(r).get('Action') for r in sequence]
        first = names.index('SetInstallVhfGamepad')
        if not (names.index('RunInstallerMigrations') < first < names.index('InstallVhfGamepad') < names.index('InstallServices')):
            raise ValueError('VHF install must follow migrations and precede service installation')
        for name in ACTIONS:
            row, _ = take('InstallExecuteSequence', 'Action', name)
            if row.get('Condition') != CONDITION or set(row) != {'Action', 'Condition', 'Sequence'}:
                raise ValueError(f'Unexpected VHF action condition: {name}')
    return result
