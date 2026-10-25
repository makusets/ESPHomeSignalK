"""Check HTTP status handling when ESP-IDF rejects an authentication challenge."""

from pathlib import Path
import subprocess
import tempfile
import unittest


class HttpResponseStatusTest(unittest.TestCase):
    def test_received_http_errors_survive_transport_errors(self):
        root = Path(__file__).resolve().parents[3]
        source = (root / "components/signalk/signalk_esp_idf.cpp").read_text()
        start = source.index("static int http_response_status(")
        end = source.index("\n}", start) + 2
        code = "#include <cassert>\nusing esp_err_t = int;\nconstexpr int ESP_OK = 0;\n"
        code += source[start:end]
        code += r"""
int main() {
  assert(http_response_status(0, 200) == 200);
  assert(http_response_status(0, 426) == 426);
  assert(http_response_status(1, 401) == 401);
  assert(http_response_status(1, 403) == 403);
  assert(http_response_status(1, 500) == 500);
  assert(http_response_status(1, 0) == -1);
  assert(http_response_status(1, 200) == -1);
  assert(http_response_status(1, 302) == -1);
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "test"
            subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-x", "c++", "-", "-o", str(binary)],
                input=code,
                text=True,
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
