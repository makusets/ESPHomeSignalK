"""Native tests of the production settings implementation using fake ESPHome storage/entities."""

from pathlib import Path
import subprocess
import tempfile
import unittest


class ConnectionSettingsTest(unittest.TestCase):
    def test_settings_lifecycle(self):
        root = Path(__file__).resolve().parents[3]
        component = root / "components/signalk"
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            # Compile the unchanged implementation with minimal ESPHome substitutes.
            for filename in ["connection_settings.h", "connection_settings.cpp"]:
                (directory / filename).write_text((component / filename).read_text())
            for filename in [
                "core/defines.h",
                "core/component.h",
                "core/preferences.h",
                "core/log.h",
                "components/text/text.h",
                "components/number/number.h",
                "components/button/button.h",
            ]:
                path = directory / "esphome" / filename
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#include "fake_esphome.h"\n')
            (directory / "fake_esphome.h").write_text(r"""
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <cstring>
#define USE_SIGNALK_CONNECTION_SETTINGS
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
namespace esphome {
namespace setup_priority { constexpr float DATA = 600; }
struct Component {
  virtual ~Component() = default;
  virtual void setup() {}
  virtual float get_setup_priority() const { return 600; }
};
namespace text {
struct Text {
  std::string state;
  void publish_state(const std::string &s) { state = s; }
  virtual void control(const std::string &) = 0;
};
}
namespace number {
struct Number {
  float state{};
  void publish_state(float s) { state = s; }
  virtual void control(float) = 0;
};
}
namespace button { struct Button { virtual void press_action() = 0; }; }
inline std::vector<uint8_t> storage;
inline bool save_ok = true, sync_ok = true;
struct Preference {
  template<class T> bool load(T *value) {
    if (storage.size() != sizeof(T)) return false;
    std::memcpy(value, storage.data(), sizeof(T)); return true;
  }
  template<class T> bool save(const T *value) {
    if (!save_ok) return false;
    const auto *p = reinterpret_cast<const uint8_t *>(value);
    storage.assign(p, p + sizeof(T)); return true;
  }
};
struct Preferences {
  template<class T> Preference make_preference(uint32_t, bool) { return {}; }
  bool sync() { return sync_ok; }
};
inline Preferences preferences;
inline Preferences *global_preferences = &preferences;
}
""")
            (directory / "signalk.h").write_text(r"""
#pragma once
#include "connection_settings.h"
namespace esphome::signalk {
enum class SignalKLoginState { UNKNOWN, COMPLETED };
enum class SignalKRequestAccessState { UNKNOWN, COMPLETED, HASTOKEN };
enum class SignalKPreferedAccessMethod { LOGIN, REQUEST_ACCESS };
struct SignalK {
  ConnectionSettings pending_settings_;
  ConnectionSettings active_settings_;
  bool apply_settings_requested_{false}, connection_attempted_{true};
  std::string host_{"yaml-server"}, user_name_{"yaml-user"}, user_password_{"yaml-password"};
  std::string token_{"old-token"}, request_access_href_{"old-request"};
  uint16_t port_{3000};
  SignalKLoginState login_state_{SignalKLoginState::COMPLETED};
  SignalKRequestAccessState request_access_state_{SignalKRequestAccessState::COMPLETED};
  SignalKPreferedAccessMethod prefered_access_method_{SignalKPreferedAccessMethod::LOGIN};
  int disconnects{0}, notifications{0}, token_saves{0};
  ConnectionSettings &pending_settings() { return pending_settings_; }
  void request_apply_settings() { apply_settings_requested_ = true; }
  void setup_connection_settings_();
  void apply_connection_settings_();
  void disconnect() { ++disconnects; }
  void on_disconnected() { ++notifications; }
  void save_token() { ++token_saves; }
};
}
""")
            (directory / "test.cpp").write_text(r"""
#include "signalk.h"
#include <cassert>
#include <limits>
using namespace esphome;
using namespace esphome::signalk;
struct Input : SignalKConnectionText { using SignalKConnectionText::control; };
struct Port : SignalKConnectionPort { using SignalKConnectionPort::control; };
struct Apply : SignalKApplySettings { using SignalKApplySettings::press_action; };
int main() {
  SignalK client;
  client.setup_connection_settings_();
  assert(client.pending_settings().host == "yaml-server");
  Input address, password, username;
  address.set_parent(&client); address.set_field(ConnectionSettingField::HOST); address.setup();
  username.set_parent(&client); username.set_field(ConnectionSettingField::USERNAME); username.setup();
  password.set_parent(&client); password.set_field(ConnectionSettingField::PASSWORD); password.setup();
  assert(password.state.empty());
  assert(client.pending_settings().password == "yaml-password");
  address.control("new-server.local");
  username.control("new-user");
  password.control("new-secret");
  assert(password.state.empty());
  assert(client.host_ == "yaml-server" && client.disconnects == 0); // Edits are staged.
  Port port; port.set_parent(&client); port.setup();
  port.control(4000); assert(port.state == 4000);
  port.control(0); port.control(65536); port.control(1.5);
  port.control(std::numeric_limits<float>::quiet_NaN());
  assert(client.pending_settings().port == 4000);
  Apply apply; apply.set_parent(&client); apply.press_action();
  assert(client.apply_settings_requested_ && client.disconnects == 0);
  client.apply_connection_settings_();
  assert(client.host_ == "new-server.local" && client.port_ == 4000);
  assert(client.user_name_ == "new-user" && client.user_password_ == "new-secret");
  assert(client.disconnects == 1 && client.notifications == 1);
  assert(client.token_.empty() && client.token_saves == 1);
  assert(client.request_access_href_.empty() && !client.connection_attempted_);
  assert(client.login_state_ == SignalKLoginState::UNKNOWN);
  assert(client.request_access_state_ == SignalKRequestAccessState::UNKNOWN);
  assert(!client.apply_settings_requested_);

  client.token_ = "current-token";
  client.apply_connection_settings_(); // Apply unchanged settings reconnects without discarding access.
  assert(client.token_ == "current-token");
  assert(client.request_access_state_ == SignalKRequestAccessState::HASTOKEN);

  SignalK restored;
  restored.setup_connection_settings_();
  assert(restored.host_ == "new-server.local" && restored.port_ == 4000);
  assert(restored.user_name_ == "new-user" && restored.user_password_ == "new-secret");
  password.set_parent(&restored); password.setup(); assert(password.state.empty());
  restored.pending_settings().host = "another-server";
  restored.apply_connection_settings_(); // Untouched password is retained.
  assert(restored.user_password_ == "new-secret");

  const int disconnects = restored.disconnects;
  for (const auto &host : {"http://server", "server/path", "server name"}) {
    restored.pending_settings().host = host;
    restored.apply_connection_settings_();
    assert(restored.disconnects == disconnects);
  }
  restored.pending_settings().host = "";
  password.control("");
  restored.apply_connection_settings_(); // A partial credential pair is rejected.
  assert(restored.disconnects == disconnects);
  restored.pending_settings().username = "";
  restored.apply_connection_settings_();
  assert(restored.host_.empty() && restored.user_password_.empty());
  assert(restored.prefered_access_method_ == SignalKPreferedAccessMethod::REQUEST_ACCESS);

  restored.pending_settings().host = "unsaved";
  save_ok = false;
  restored.apply_connection_settings_();
  assert(restored.host_.empty());
  save_ok = true; sync_ok = false;
  restored.apply_connection_settings_();
  assert(restored.host_.empty());
}
""")
            binary = directory / "test"
            subprocess.run(
                [
                    "c++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(directory),
                    str(directory / "connection_settings.cpp"),
                    str(directory / "test.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
