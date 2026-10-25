"""Compile the enum expressions emitted by ESPHome for connection settings."""

import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(importlib.util.find_spec("esphome"), "ESPHome is required for code generation")
class ConnectionCodegenTest(unittest.TestCase):
    def test_generated_connection_fields_compile(self):
        root = Path(__file__).resolve().parents[3]
        fixture = root / "tests/components/signalk/test.web-settings.yaml"
        # Older ESPHome releases resolve local component paths relative to the
        # top-level config, so keep that config beside the fixture.
        with (
            tempfile.TemporaryDirectory() as tmp,
            tempfile.NamedTemporaryFile(
                mode="w", suffix=".yaml", prefix=".codegen-", dir=fixture.parent
            ) as config_file,
        ):
            directory = Path(tmp)
            config = Path(config_file.name)
            build = directory / "build"
            config.write_text(
                f"packages:\n  base: !include {json.dumps(str(fixture))}\n"
                f"esphome:\n  build_path: {json.dumps(str(build))}\n"
            )
            generated = subprocess.run(
                [sys.executable, "-m", "esphome", "compile", str(config), "--only-generate"],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(generated.returncode, 0, generated.stdout + generated.stderr)
            main = (build / "src/main.cpp").read_text()
            fields = re.findall(r"->set_field\(([^)]+)\)", main)
            self.assertEqual(len(fields), 3)
            header = (root / "components/signalk/connection_settings.h").read_text()
            enum = re.search(r"enum class ConnectionSettingField[^;]+;", header).group()
            code = "namespace esphome { namespace signalk { " + enum + " } }\n"
            code += "using namespace esphome;\nint main() {\n"
            code += "".join(f"  (void) {field};\n" for field in fields)
            code += "}\n"
            compiled = subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-x", "c++", "-fsyntax-only", "-"],
                input=code,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)


if __name__ == "__main__":
    unittest.main()
