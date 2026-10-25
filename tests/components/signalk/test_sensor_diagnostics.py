"""Exercise the production numeric sensor methods without an ESP32 SDK."""

from pathlib import Path
import subprocess
import tempfile
import unittest


class SensorDiagnosticsTest(unittest.TestCase):
    def test_validation_publication_and_missing_values(self):
        root = Path(__file__).resolve().parents[3]
        source = (root / "components/signalk/sensor/signalk_sensor.cpp").read_text()
        methods = source[source.index("namespace esphome {") :]
        harness = r"""
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
uint32_t clock_ms = 0;
uint32_t sk_millis() { return clock_ms; }
std::vector<std::string> warnings;
template<class... Args> void log_warning(const char *, const char *format, Args...) { warnings.emplace_back(format); }
template<class... Args> void log_other(Args...) {}
#define ESP_LOGD(...) log_other(__VA_ARGS__)
#define ESP_LOGCONFIG(...) log_other(__VA_ARGS__)
#define ESP_LOGW(...) log_warning(__VA_ARGS__)
struct JsonVariant {
  enum Type { NUMBER, NIL, STRING, BOOLEAN, OBJECT } type;
  double number{0};
  bool isNull() const { return type == NIL; }
  template<class T> bool is() const {
    if constexpr (std::is_same_v<T, double>) return type == NUMBER;
    if constexpr (std::is_same_v<T, const char *>) return type == STRING;
    if constexpr (std::is_same_v<T, bool>) return type == BOOLEAN;
    return false;
  }
  template<class T> T as() const { return static_cast<T>(number); }
};
float convert_from_base(double value, int) { return value; }
namespace esphome::signalk {
struct SignalK { bool connected{true}; bool is_connected() const { return connected; } };
struct SignalkSubscriber { void disconnected() {} };
struct SignalkSensor : SignalkSubscriber {
  SignalK *parent_;
  std::string path_{"electrical.solar.victronMPPT.dc.voltage"};
  std::string get_name() const { return "Battery voltage"; }
  double value_{};
  int unit_{0};
  bool requires_update_{false}, received_value_{false}, was_connected_{false};
  uint32_t last_value_ms_{0}, last_diagnostic_ms_{0};
  std::vector<float> published;
  void publish_state(float value) { published.push_back(value); }
  void setup(); void dump_config(); void loop(); void update(); void disconnected(); void set_value(JsonVariant &);
};
}
METHODS
int main() {
  using namespace esphome::signalk;
  SignalK parent;
  SignalkSensor sensor; sensor.parent_ = &parent; sensor.setup();
  sensor.loop(); assert(sensor.published.empty());
  clock_ms = 29999; sensor.loop(); assert(warnings.empty());
  clock_ms = 30000; sensor.loop(); assert(warnings.size() == 1);
  for (auto kind : {JsonVariant::NIL, JsonVariant::STRING, JsonVariant::BOOLEAN, JsonVariant::OBJECT}) {
    JsonVariant bad{kind}; sensor.set_value(bad); sensor.loop();
    assert(sensor.published.empty());
  }
  JsonVariant nan{JsonVariant::NUMBER, std::numeric_limits<double>::quiet_NaN()};
  sensor.set_value(nan); sensor.loop(); assert(sensor.published.empty());
  JsonVariant valid{JsonVariant::NUMBER, 12.7};
  sensor.set_value(valid); assert(sensor.published.empty());
  sensor.loop(); assert(sensor.published.size() == 1);
  assert(std::fabs(sensor.published.back() - 12.7) < 0.001);
  sensor.loop(); sensor.update(); assert(sensor.published.size() == 1);
  auto warning_count = warnings.size();
  clock_ms = 60000; sensor.loop(); assert(warnings.size() == warning_count);
  sensor.disconnected(); parent.connected = false;
  clock_ms = 90000; sensor.loop(); assert(sensor.published.size() == 1);
  parent.connected = true; sensor.loop();
  clock_ms += 30000; sensor.loop(); assert(warnings.size() == warning_count + 1);
  JsonVariant too_large{JsonVariant::NUMBER, std::numeric_limits<double>::max()};
  sensor.set_value(too_large); sensor.loop(); assert(sensor.published.size() == 1);
}
""".replace("METHODS", methods)
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "test"
            subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-x", "c++", "-", "-o", str(binary)],
                input=harness,
                text=True,
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
