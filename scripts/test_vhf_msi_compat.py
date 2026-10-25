#!/usr/bin/env python3
"""Checks the additive migration without constructing or fabricating a golden MSI."""
from pathlib import Path
import unittest
import uuid
from diff_msi_tables import parse_dump, diff_dumps
from vhf_msi_compat import PACKAGE_FILES, LICENSE, ACTIONS, CONDITION, file_ids

GOLDEN = (Path(__file__).resolve().parent.parent / 'tests/fixtures/msi_golden/msi_baseline.txt').read_text()


def candidate_dump():
    tables = parse_dump(GOLDEN)
    tables['Property'].append('Property=INSTALL_VHF_GAMEPAD|Value=1')
    for i, row in enumerate(tables['Property']):
        if row.startswith('Property=SecureCustomProperties|'):
            tables['Property'][i] = row.replace(';INSTALL_ROOT;', ';INSTALL_ROOT;INSTALL_VHF_GAMEPAD;')
    directories = {}
    for path in (*PACKAGE_FILES, LICENSE):
        component, file_id, directory = file_ids(path)
        guid = '{' + str(uuid.uuid5(uuid.NAMESPACE_URL, path)).upper() + '}'
        tables['Component'].append(f'Component={component}|ComponentId={guid}|Directory_={directory}|Attributes=256|Condition=|KeyPath={file_id}')
        tables['FeatureComponents'].append(f'Feature_=CM_C_application|Component_={component}')
        dirs = path.split('/')[:-1]
        for i in range(1, len(dirs) + 1):
            name = 'CM_DP_application.' + '.'.join(p.replace('-', '_') for p in dirs[:i])
            parent = 'INSTALL_ROOT' if i == 1 else 'CM_DP_application.' + '.'.join(p.replace('-', '_') for p in dirs[:i-1])
            directories[name] = (parent, dirs[i-1])
    for directory, (parent, name) in directories.items():
        tables['Directory'].append(f'Directory={directory}|Directory_Parent={parent}|DefaultDir={name}')
    for i, (action, values) in enumerate(ACTIONS.items(), 4009):
        tables['CustomAction'].append('|'.join(f'{k}={v}' for k, v in {'Action': action, **values}.items()))
        tables['InstallExecuteSequence'].append(f'Action={action}|Condition={CONDITION}|Sequence={i}')
    return '\n'.join('## TABLE: ' + name + '\n' + '\n'.join(rows) for name, rows in tables.items())


class VhfMigrationContract(unittest.TestCase):
    def test_reviewed_additions_pass(self):
        self.assertEqual(diff_dumps(GOLDEN, candidate_dump()), [])

    def test_missing_payload_fails(self):
        cid = file_ids(PACKAGE_FILES[2])[0]
        candidate = '\n'.join(r for r in candidate_dump().splitlines() if not r.startswith('Component=' + cid + '|'))
        self.assertTrue(diff_dumps(GOLDEN, candidate))

    def test_install_on_uninstall_fails(self):
        self.assertTrue(diff_dumps(GOLDEN, candidate_dump().replace(CONDITION, 'REMOVE="ALL"')))

    def test_wrong_command_fails(self):
        self.assertTrue(diff_dumps(GOLDEN, candidate_dump().replace('drivers\\vhf-gamepad\\install.ps1', 'scripts\\other.ps1')))

    def test_wrong_directory_fails(self):
        self.assertTrue(diff_dumps(GOLDEN, candidate_dump().replace('DefaultDir=vhf-gamepad', 'DefaultDir=elsewhere')))

    def test_unrelated_change_still_fails(self):
        self.assertTrue(diff_dumps(GOLDEN, candidate_dump().replace('Name=LuminalShineService|', 'Name=OtherService|')))

    def test_existing_order_still_fails(self):
        self.assertTrue(diff_dumps(GOLDEN, candidate_dump().replace('Action=CreateFolders|Condition=|Sequence=3700', 'Action=CreateFolders|Condition=|Sequence=4001')))

    def test_later_baseline_is_strict(self):
        baseline = candidate_dump()
        altered = baseline.replace('Attributes=256|Condition=|KeyPath=' + file_ids(PACKAGE_FILES[0])[1], 'Attributes=0|Condition=|KeyPath=' + file_ids(PACKAGE_FILES[0])[1])
        self.assertTrue(diff_dumps(baseline, altered))


if __name__ == '__main__':
    unittest.main()
