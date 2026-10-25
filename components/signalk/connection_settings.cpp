#include "connection_settings.h"

#ifdef USE_SIGNALK_CONNECTION_SETTINGS
#include "signalk.h"
#include "esphome/core/preferences.h"
#include "esphome/core/log.h"
#include <cmath>
#include <cstring>

namespace esphome {
namespace signalk {

static const char *const TAG = "signalk";

// Keep the on-flash layout stable. A separate key leaves existing token storage intact.
struct StoredConnectionSettings {
  char host[256];
  char username[64];
  char password[128];
  uint16_t port;
};

static constexpr uint32_t SETTINGS_KEY = 0x534B4301;

void SignalK::setup_connection_settings_() {
  StoredConnectionSettings stored{};
  auto pref = global_preferences->make_preference<StoredConnectionSettings>(SETTINGS_KEY, true);
  if (pref.load(&stored) && stored.port != 0 && stored.host[255] == '\0' && stored.username[63] == '\0' &&
      stored.password[127] == '\0') {
    host_ = stored.host;
    port_ = stored.port;
    user_name_ = stored.username;
    user_password_ = stored.password;
    ESP_LOGI(TAG, "Loaded saved SignalK connection settings");
  }
  pending_settings_ = {host_, port_, user_name_, user_password_};
  active_settings_ = pending_settings_;
}

void SignalK::apply_connection_settings_() {
  apply_settings_requested_ = false;
  const auto &settings = pending_settings_;
  if (settings.host.size() > 255 || settings.username.size() > 63 || settings.password.size() > 127 ||
      settings.port == 0 || settings.host.find_first_of("/ \t\r\n") != std::string::npos ||
      (settings.username.empty() != settings.password.empty())) {
    ESP_LOGE(TAG,
             "Invalid SignalK settings: use a hostname/IP, a valid port, and both username and password or neither");
    return;
  }
  StoredConnectionSettings stored{};
  memcpy(stored.host, settings.host.data(), settings.host.size());
  memcpy(stored.username, settings.username.data(), settings.username.size());
  memcpy(stored.password, settings.password.data(), settings.password.size());
  stored.port = settings.port;
  auto pref = global_preferences->make_preference<StoredConnectionSettings>(SETTINGS_KEY, true);
  if (!pref.save(&stored) || !global_preferences->sync()) {
    ESP_LOGE(TAG, "Failed to save SignalK connection settings; connection unchanged");
    return;
  }

  const bool changed = settings.host != active_settings_.host || settings.port != active_settings_.port ||
                       settings.username != active_settings_.username || settings.password != active_settings_.password;
  disconnect();
  on_disconnected();
  host_ = settings.host;
  port_ = settings.port;
  user_name_ = settings.username;
  user_password_ = settings.password;
  if (changed) {
    token_.clear();
    save_token();
    global_preferences->sync();
  }
  active_settings_ = settings;
  request_access_href_.clear();
  login_state_ = SignalKLoginState::UNKNOWN;
  request_access_state_ = token_.empty() ? SignalKRequestAccessState::UNKNOWN : SignalKRequestAccessState::HASTOKEN;
  prefered_access_method_ =
      user_name_.empty() ? SignalKPreferedAccessMethod::REQUEST_ACCESS : SignalKPreferedAccessMethod::LOGIN;
  connection_attempted_ = false;
  ESP_LOGI(TAG, "Applied SignalK connection settings; reconnecting");
}

void SignalKConnectionText::setup() {
  const auto &settings = parent_->pending_settings();
  if (field_ == ConnectionSettingField::HOST)
    publish_state(settings.host);
  else if (field_ == ConnectionSettingField::USERNAME)
    publish_state(settings.username);
  else
    publish_state("");  // The password is write-only, including after restart.
}

void SignalKConnectionText::control(const std::string &value) {
  auto &settings = parent_->pending_settings();
  if (field_ == ConnectionSettingField::HOST)
    settings.host = value;
  else if (field_ == ConnectionSettingField::USERNAME)
    settings.username = value;
  else {
    settings.password = value;
    publish_state("");
    return;
  }
  publish_state(value);
}

void SignalKConnectionPort::setup() { publish_state(parent_->pending_settings().port); }

void SignalKConnectionPort::control(float value) {
  if (!std::isfinite(value) || value < 1 || value > 65535 || std::floor(value) != value)
    return;
  parent_->pending_settings().port = static_cast<uint16_t>(value);
  publish_state(value);
}

void SignalKApplySettings::press_action() { parent_->request_apply_settings(); }

}  // namespace signalk
}  // namespace esphome
#endif
