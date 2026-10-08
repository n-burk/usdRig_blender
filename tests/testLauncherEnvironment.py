"""Viewer helper registers this converter only for source-file translation."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class LauncherEnvironment(unittest.TestCase):
    def test_native_playback_has_no_converter_discovery(self):
        launcher = Path(__file__).resolve().parents[1] / 'bin' / 'usdview.sh'
        with tempfile.TemporaryDirectory(prefix='usdRig-launcher-check-') as folder:
            probe = Path(folder) / 'probe.py'
            probe.write_text('import json, os\nprint(json.dumps({"plugins":os.environ.get("PXR_PLUGINPATH_NAME", "")}))\n')
            env = dict(os.environ, USDBLENDERRIG_USDVIEW=str(probe))
            for source, expected in [('character.usdc', False), ('character.usdz', False),
                                     ('character.blend', True), ('character.blendrig', True)]:
                result = subprocess.run([str(launcher), source], env=env, text=True,
                                        check=True, capture_output=True)
                paths = json.loads(result.stdout)['plugins']
                self.assertEqual('usdBlenderRig/resources' in paths, expected, paths)
                self.assertIn('rigExecSchema/resources', paths)


if __name__ == '__main__':
    unittest.main()
