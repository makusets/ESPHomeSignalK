"""Exercise the production connection loop with a fake clock and transports.

Run with: python3 -m unittest discover -s tests/components/signalk -p 'test_*.py'
A native C++ compiler is required; no ESP32 hardware or server is needed.
"""

from pathlib import Path
import subprocess
import tempfile
import unittest


class ConnectionRetryTest(unittest.TestCase):
    def test_connection_state_machine(self):
        root = Path(__file__).resolve().parents[3]
        source = (root / "components/signalk/signalk.cpp").read_text()
        # Compile the actual loop in isolation from ESPHome and platform SDKs.
        loop = source[source.index("void SignalK::loop() {") : source.index("void SignalK::update() {")]
        disconnected = source[
            source.index("void SignalK::on_disconnected() {") : source.index("void SignalK::login() {")
        ]
        header = (root / "components/signalk/signalk.h").read_text()
        enums = header[header.index("enum class SignalKPreferedAccessMethod") : header.index("struct HttpResponse")]
        harness = (
            r"""
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#define USE_BINARY_SENSOR
struct BinarySensor { bool state{false}; void publish_state(bool value) { state = value; } };
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
uint32_t now = 0;
uint32_t sk_millis() { return now; }
ENUMS
struct Subscriber { void disconnected() {} };
struct SignalK {
  uint32_t last_connection_attempt_{0};
  bool connection_attempted_{false};
  BinarySensor *connected_sensor_{nullptr};
  bool connection_sensor_state_{false};
  bool connected{false}, found{false}, login_ok{false}, connect_ok{false};
  unsigned port_{3000};
  std::string host_, token_;
  std::vector<Subscriber *> sensors_;
  SignalKPreferedAccessMethod prefered_access_method_{SignalKPreferedAccessMethod::LOGIN};
  SignalKLoginState login_state_{SignalKLoginState::UNKNOWN};
  SignalKRequestAccessState request_access_state_{SignalKRequestAccessState::UNKNOWN};
  int discoveries{0}, logins{0}, connections{0}, requests{0}, polls{0}, validations{0};
  bool is_connected() { return connected; }
  bool discovery() { ++discoveries; if (found) host_ = "server.local"; return found; }
  void login() { ++logins; login_state_ = login_ok ? SignalKLoginState::COMPLETED : SignalKLoginState::DENIED; }
  bool connect(const std::string &) { ++connections; return connect_ok; }
  void send_access_request() { ++requests; request_access_state_ = SignalKRequestAccessState::PENDING; }
  void poll_access_request() { ++polls; }
  void validate_token() { ++validations; request_access_state_ = SignalKRequestAccessState::COMPLETED; }
  void loop();
  void on_disconnected();
};
LOOP
DISCONNECTED
int main() {
  SignalK a;
  BinarySensor sensor;
  a.connected_sensor_ = &sensor;
  a.loop();
  assert(!sensor.state); assert(a.discoveries == 1); // Immediate initial attempt, even at uptime zero.
  now = 29999; a.loop(); assert(a.discoveries == 1);
  now = 30000; a.loop(); assert(a.discoveries == 2);
  a.found = true;
  now = 60000; a.loop(); assert(a.logins == 1 && a.connections == 0);
  now = 89999; a.loop(); assert(a.logins == 1);
  now = 90000; a.loop(); assert(a.logins == 2); // Denial is retried.
  a.login_ok = true;
  now = 120000; a.loop(); assert(a.logins == 3 && a.connections == 1);
  now = 149999; a.loop(); assert(a.connections == 1);
  now = 150000; a.loop(); assert(a.connections == 2); // Failed WebSocket retry.
  a.connected = true;
  now = 180000; a.loop(); assert(a.connections == 2); // No attempts while connected.

  assert(sensor.state);
  a.connected = false;
  a.on_disconnected();
  now += 29999; a.loop(); assert(a.connections == 2);
  assert(!sensor.state);
  now += 1; a.loop(); assert(a.connections == 3); // Wait 30 seconds after disconnect.

  SignalK b;
  b.host_ = "configured";
  b.loop(); assert(b.logins == 1); // Timers are per instance.
  assert(b.discoveries == 0);

  SignalK c;
  c.host_ = "configured";
  c.prefered_access_method_ = SignalKPreferedAccessMethod::REQUEST_ACCESS;
  c.loop(); assert(c.requests == 1);
  now += 29999; c.loop(); assert(c.polls == 0);
  now += 1; c.loop(); assert(c.polls == 1 && c.requests == 1);
  c.request_access_state_ = SignalKRequestAccessState::DENIED;
  now += 30000; c.loop(); assert(c.requests == 2);
  c.request_access_state_ = SignalKRequestAccessState::HASTOKEN;
  now += 30000; c.loop(); assert(c.validations == 1 && c.connections == 1);

  SignalK d;
  now = UINT32_MAX - 10000;
  d.loop();
  now += 29999; d.loop(); assert(d.discoveries == 1);
  now += 1; d.loop(); assert(d.discoveries == 2); // Clock rollover.
}
""".replace("ENUMS", enums)
            .replace("LOOP", loop)
            .replace("DISCONNECTED", disconnected)
        )
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / "retry.cpp"
            binary = Path(tmp) / "retry"
            cpp.write_text(harness)
            subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(binary)], check=True
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
