

#include "esphome/core/preferences.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include <regex>
#include <cstdio>
// #include "esphome/components/wifi/wifi_component.h"

#include "signalk.h"
#include "utils.h"

namespace esphome {
namespace signalk {

static const char *const TAG = "signalk";

void SignalK::setup() {
  ESP_LOGD(TAG, "Setup");
#ifdef USE_SIGNALK_CONNECTION_SETTINGS
  setup_connection_settings_();
#endif
#ifdef USE_BINARY_SENSOR
  if (connected_sensor_ != nullptr)
    connected_sensor_->publish_state(false);
#endif

  if (!this->user_name_.empty() && !this->user_password_.empty()) {
    prefered_access_method_ = SignalKPreferedAccessMethod::LOGIN;
    ESP_LOGD(TAG, "Using LOGIN as prefered access method");
    return;
  }

  load_token();
  ESP_LOGD(TAG, "Using REQUEST_ACCESS as prefered access method");
  prefered_access_method_ = SignalKPreferedAccessMethod::REQUEST_ACCESS;

  if (!token_.empty()) {
    request_access_state_ = SignalKRequestAccessState::HASTOKEN;
  }
}

void SignalK::loop() {
#ifdef USE_SIGNALK_CONNECTION_SETTINGS
  if (apply_settings_requested_)
    apply_connection_settings_();
#endif
#ifdef USE_BINARY_SENSOR
  const bool connected = is_connected();
  if (connected_sensor_ != nullptr && connection_sensor_state_ != connected) {
    connection_sensor_state_ = connected;
    connected_sensor_->publish_state(connected);
  }
#endif
  if (is_connected())
    return;
  const uint32_t now = sk_millis();
  if (connection_attempted_ && now - last_connection_attempt_ < 30000)
    return;
  connection_attempted_ = true;
  last_connection_attempt_ = now;

  if (host_.empty()) {
    ESP_LOGI(TAG, "Searching for a SignalK server");
    if (!discovery() || host_.empty()) {
      ESP_LOGW(TAG, "No SignalK server found; retrying in 30 seconds");
      return;
    }
    ESP_LOGI(TAG, "SignalK server found at %s:%u", host_.c_str(), port_);
  }

  if (prefered_access_method_ == SignalKPreferedAccessMethod::LOGIN) {
    if (login_state_ != SignalKLoginState::COMPLETED)
      login();
    if (login_state_ != SignalKLoginState::COMPLETED) {
      ESP_LOGW(TAG, "SignalK login unsuccessful; retrying in 30 seconds");
      return;
    }
  } else {
    if (request_access_state_ == SignalKRequestAccessState::DENIED)
      request_access_state_ = SignalKRequestAccessState::UNKNOWN;
    if (request_access_state_ == SignalKRequestAccessState::HASTOKEN)
      validate_token();
    if (request_access_state_ == SignalKRequestAccessState::UNKNOWN)
      send_access_request();
    else if (request_access_state_ == SignalKRequestAccessState::PENDING)
      poll_access_request();
    if (request_access_state_ != SignalKRequestAccessState::COMPLETED) {
      ESP_LOGI(TAG, "SignalK access not ready; retrying in 30 seconds");
      return;
    }
  }
  ESP_LOGI(TAG, "Connecting to SignalK at %s:%u", host_.c_str(), port_);
  if (!connect("/signalk/v1/stream?subscribe=none")) {
    ESP_LOGW(TAG, "SignalK connection failed; retrying in 30 seconds");
    login_state_ = SignalKLoginState::UNKNOWN;
    request_access_state_ = token_.empty() ? SignalKRequestAccessState::UNKNOWN : SignalKRequestAccessState::HASTOKEN;
  }
}

void SignalK::update() {
  if (is_connected()) {
    for (auto it = sensors_.begin(); it != sensors_.end(); ++it) {
      (*it)->update();
      // if (it->second != NULL) {
      //   it->second->update();
      // }
    }
  }
}

void SignalK::dump_config() {}

void SignalK::on_connected() {
  ESP_LOGI(TAG, "Connected to SignalK at %s:%u", host_.c_str(), port_);
  std::set<std::string> topics;
  JsonDocument doc;
  doc["context"] = "vessels.self";
  JsonArray subscribe = doc["subscribe"].to<JsonArray>();
  for (auto it = sensors_.begin(); it != sensors_.end(); ++it) {
    auto path = (*it)->get_path();
    if (topics.find(path) != topics.end())
      continue;
    JsonObject subscription = subscribe.add<JsonObject>();
    subscription["path"] = path;
    subscription["period"] = (*it)->get_period();
    subscription["format"] = (*it)->get_format();
    subscription["policy"] = (*it)->get_policy();
    subscription["minPeriod"] = (*it)->get_min_period();
    ESP_LOGI(TAG, "Subscribing to '%s' (period=%d ms, format=%s, policy=%s)", path.c_str(), (*it)->get_period(),
             (*it)->get_format().c_str(), (*it)->get_policy().c_str());
    topics.insert(path);
  }
  std::string output;
  serializeJsonPretty(doc, output);
  ESP_LOGD(TAG, "Subscription message: %s", output.c_str());
  if (!send(output))
    ESP_LOGE(TAG, "Failed to send SignalK subscriptions; sensor values will not arrive");
  else
    ESP_LOGI(TAG, "Sent subscriptions for %u SignalK paths; waiting for values", static_cast<unsigned>(topics.size()));
}

void SignalK::on_disconnected() {
  ESP_LOGW(TAG, "Disconnected from SignalK server; retrying in 30 seconds");
  connection_attempted_ = true;
  last_connection_attempt_ = sk_millis();
  login_state_ = SignalKLoginState::UNKNOWN;
  if (!token_.empty())
    request_access_state_ = SignalKRequestAccessState::HASTOKEN;
  else
    request_access_state_ = SignalKRequestAccessState::UNKNOWN;
  for (auto it = sensors_.begin(); it != sensors_.end(); ++it) {
    // it->second->disconnected();
    (*it)->disconnected();
  }
}

void SignalK::login() {
  if (login_state_ == SignalKLoginState::COMPLETED) {
    return;
  }

  JsonDocument doc;
  doc["username"] = this->user_name_;
  doc["password"] = this->user_password_;
  std::string msg;
  serializeJson(doc, msg);
  ESP_LOGI(TAG, "Logging in to SignalK at %s:%u", host_.c_str(), port_);
  auto response = post("/signalk/v1/auth/login", msg);
  if (response.status_code <= 0) {
    ESP_LOGW(TAG, "SignalK login request failed at %s:%u: no usable HTTP response (status %d)", host_.c_str(), port_,
             response.status_code);
    return;
  }
  ESP_LOGI(TAG, "SignalK server responded at %s:%u (HTTP %d)", host_.c_str(), port_, response.status_code);
  if (response.status_code == 401 || response.status_code == 403) {
    login_state_ = SignalKLoginState::DENIED;
    ESP_LOGE(TAG, "SignalK login denied: check username and password (HTTP %d)", response.status_code);
    return;
  }
  if (response.status_code < 200 || response.status_code >= 300) {
    ESP_LOGW(TAG, "SignalK login failed: HTTP %d", response.status_code);
    return;
  }

  JsonDocument resp_doc;
  DeserializationError error = deserializeJson(resp_doc, response.body);
  if (error) {
    ESP_LOGE(TAG, "Invalid SignalK login response: %s", error.c_str());
    return;
  }

  token_ = resp_doc["token"] | "";
  if (!token_.empty()) {
    save_token();
    login_state_ = SignalKLoginState::COMPLETED;
    ESP_LOGI(TAG, "Login request successful");
  } else if (resp_doc["result"] == 401 || resp_doc["result"] == 403) {
    login_state_ = SignalKLoginState::DENIED;
    ESP_LOGE(TAG, "SignalK login denied: check username and password");
  } else {
    ESP_LOGW(TAG, "SignalK login response contains no token");
  }
}

void SignalK::send_access_request() {
  if (!token_.empty()) {
    request_access_state_ = SignalKRequestAccessState::HASTOKEN;
    return;
  }
  JsonDocument doc;
  doc["clientId"] = get_uuid();
  doc["description"] = "ESPHome device: " + App.get_name();
  std::string msg;
  serializeJson(doc, msg);

  ESP_LOGD(TAG, "Trying to request access: %s", msg.c_str());
  auto response = post("/signalk/v1/access/requests", msg);
  ESP_LOGD(TAG, "Access request response: HTTP %d", response.status_code);
  if (response.status_code < 200 || response.status_code >= 300) {
    ESP_LOGW(TAG, "SignalK access request failed at %s:%u (HTTP status %d)", host_.c_str(), port_,
             response.status_code);
    return;
  }

  JsonDocument resp_doc;
  DeserializationError error = deserializeJson(resp_doc, response.body);
  if (error) {
    ESP_LOGE(TAG, "Failed to parse access request response: %s", error.c_str());
    return;
  } else if (resp_doc["state"] == "PENDING") {
    request_access_state_ = SignalKRequestAccessState::PENDING;
    request_access_href_ = resp_doc["href"] | "";
    ESP_LOGW(TAG, "Access request is pending approval");
  } else if (resp_doc["state"] == "COMPLETED") {
    if (resp_doc["statusCode"] == 400) {
      request_access_href_ = resp_doc["href"] | "";
      if (!request_access_href_.empty()) {
        request_access_state_ = SignalKRequestAccessState::PENDING;
        return;
      }
      request_access_state_ = SignalKRequestAccessState::DENIED;
      ESP_LOGE(TAG, "Access request denied");
      return;
    }
  }
}

void SignalK::poll_access_request() {
  auto response = get(request_access_href_);
  ESP_LOGD(TAG, "Access request status response: HTTP %d", response.status_code);
  if (response.status_code < 200 || response.status_code >= 300) {
    ESP_LOGW(TAG, "SignalK access request failed at %s:%u (HTTP status %d)", host_.c_str(), port_,
             response.status_code);
    return;
  }

  JsonDocument resp_doc;
  DeserializationError error = deserializeJson(resp_doc, response.body);
  if (error) {
    ESP_LOGE(TAG, "Failed to parse access request status response: %s", error.c_str());
    return;
  }
  std::string state = resp_doc["state"] | "UNKNOWN";
  if (state == "COMPLETED") {
    if (resp_doc["accessRequest"]["permission"] == "DENIED") {
      request_access_state_ = SignalKRequestAccessState::DENIED;
      ESP_LOGE(TAG, "Access request denied");
      return;
    }

    if (resp_doc["accessRequest"]["permission"] == "APPROVED") {
      request_access_state_ = SignalKRequestAccessState::COMPLETED;
      token_ = resp_doc["accessRequest"]["token"] | "";
      if (!token_.empty()) {
        save_token();
      }
      if (token_.empty()) {
        request_access_state_ = SignalKRequestAccessState::UNKNOWN;
        ESP_LOGW(TAG, "SignalK approval response contains no token");
        return;
      }
      ESP_LOGI(TAG, "SignalK access approved");
    }
  }
}

void SignalK::validate_token() {
  ESP_LOGD(TAG, "Validating token");
  auto response = get("/signalk/v1/stream");
  ESP_LOGD(TAG, "Token validation response: HTTP %d", response.status_code);
  if (response.status_code == 426) {
    request_access_state_ = SignalKRequestAccessState::COMPLETED;
    ESP_LOGI(TAG, "Token validation successful");
  } else if (response.status_code == 401 || response.status_code == 403) {
    token_.clear();
    save_token();
    request_access_state_ = SignalKRequestAccessState::UNKNOWN;
    ESP_LOGE(TAG, "Token validation failed: HTTP %d", response.status_code);
  } else {
    ESP_LOGW(TAG, "Unexpected response during token validation: HTTP %d", response.status_code);
  }
}

uint32_t SignalK::token_storage_key_() const {
  char port[6];
  snprintf(port, sizeof(port), "%u", static_cast<unsigned>(port_));
  return fnv1_hash("signalk-token:" + host_ + ":" + port + ":" + user_name_);
}

void SignalK::save_token() {
  constexpr size_t TOKEN_MAX_LENGTH = 256;
  char token_buffer[TOKEN_MAX_LENGTH] = {0};
  uint32_t hash = token_storage_key_();
  auto pref = global_preferences->make_preference<char[TOKEN_MAX_LENGTH]>(hash, true);
  memcpy(token_buffer, token_.c_str(), std::min(token_.size(), sizeof(token_buffer) - 1));
  if (pref.save(&token_buffer)) {
    ESP_LOGI(TAG, "Saved SignalK token to preferences");
  } else {
    ESP_LOGE(TAG, "Failed to save token");
  }
}

void SignalK::load_token() {
  constexpr size_t TOKEN_MAX_LENGTH = 256;
  char token_buffer[TOKEN_MAX_LENGTH] = {0};
  uint32_t hash = token_storage_key_();
  auto pref = global_preferences->make_preference<char[TOKEN_MAX_LENGTH]>(hash, true);
  if (pref.load(&token_buffer)) {
    token_ = std::string(token_buffer);
    if (!token_.empty()) {
      request_access_state_ = SignalKRequestAccessState::HASTOKEN;
      ESP_LOGI(TAG, "Loaded stored SignalK token");
      return;
    }
  } else {
    ESP_LOGW(TAG, "No token stored in flash");
  }
}

void SignalK::on_message(const std::string &msg) {
  JsonDocument doc;
  auto error = deserializeJson(doc, msg);
  if (error) {
    ESP_LOGW(TAG, "Cannot parse SignalK WebSocket message (%u bytes): %s", static_cast<unsigned>(msg.size()),
             error.c_str());
    return;
  }
  if (doc["statusCode"].is<int>()) {
    int status = doc["statusCode"].as<int>();
    if (status >= 400)
      ESP_LOGW(TAG, "SignalK server reported stream/request error: statusCode=%d", status);
  }
  JsonArray updates = doc["updates"].as<JsonArray>();
  ESP_LOGV(TAG, "Received SignalK message with %u updates", static_cast<unsigned>(updates.size()));
  for (JsonObject update : updates) {
    JsonArray values = update["values"].as<JsonArray>();
    if (!values.isNull())
      on_receive_delta(values);
  }
  JsonArray put = doc["put"].as<JsonArray>();
  if (!put.isNull())
    on_receive_delta(put);
}

void SignalK::on_receive_delta(JsonArray &arr) {
  for (JsonVariant delta : arr) {
    if (!delta["path"].is<std::string>()) {
      ESP_LOGV(TAG, "Ignoring SignalK value without a string path");
      continue;
    }
    const auto path = delta["path"].as<std::string>();
    bool matched = false;
    for (auto *subscriber : sensors_) {
      if (match_path(path, subscriber->get_path())) {
        matched = true;
        ESP_LOGD(TAG, "Received SignalK path '%s' for subscription '%s'", path.c_str(), subscriber->get_path().c_str());
        set_sensor_value(subscriber, delta["value"]);
      }
    }
    if (!matched)
      ESP_LOGV(TAG, "No subscriber matches received SignalK path '%s'", path.c_str());
  }
}

void SignalK::set_sensor_value(SignalkSubscriber *sensor, JsonVariant value) {
  if (sensor == NULL) {
    return;
  }
  sensor->set_value(value);
}

void SignalK::put_request(const std::string &path, const std::variant<double, std::string, bool> &value) {
  if (!is_connected()) {
    return;
  }
  JsonDocument doc;
  doc["context"] = "vessels.self";
  doc["requestId"] = generate_requestid();
  JsonObject put = doc["put"].to<JsonObject>();
  put["path"] = path;

  if (std::holds_alternative<double>(value)) {
    put["value"] = std::get<double>(value);
  } else if (std::holds_alternative<bool>(value)) {
    put["value"] = std::get<bool>(value);
  } else {
    put["value"] = std::get<std::string>(value);
  }

  std::string output;
  serializeJson(doc, output);
  ESP_LOGD(TAG, "Put request: %s", output.c_str());
  send(output);
}

void SignalK::publish_delta(const std::string &path, const std::variant<double, std::string, bool> &value) {
  if (!is_connected()) {
    return;
  }
  JsonDocument doc;
  JsonArray updates = doc["updates"].to<JsonArray>();

  JsonObject update = updates.add<JsonObject>();

  // JsonObject source = update["source"].to<JsonObject>();
  // source["src"] = get_uuid();

  // auto timestamp = iso_timestamp();
  // update["timestamp"] = timestamp;

  JsonArray values = update["values"].to<JsonArray>();
  JsonObject val = values.add<JsonObject>();
  val["path"] = path;
  if (std::holds_alternative<double>(value)) {
    val["value"] = std::get<double>(value);
  } else if (std::holds_alternative<bool>(value)) {
    val["value"] = std::get<bool>(value);
  } else {
    val["value"] = std::get<std::string>(value);
  }

  std::string output;
  serializeJson(doc, output);
  ESP_LOGD(TAG, "Publishing delta: %s", output.c_str());
  send(output);
}

void SignalK::publish_meta_delta(SignalkSubscriber *subscriber) {
  if (!is_connected()) {
    return;
  }
  JsonDocument doc;
  JsonArray updates = doc["updates"].to<JsonArray>();

  JsonObject update = updates.add<JsonObject>();

  JsonArray metas = update["meta"].to<JsonArray>();
  JsonObject meta = metas.add<JsonObject>();
  meta["path"] = subscriber->get_path();
  JsonObject value = meta["value"].to<JsonObject>();
  subscriber->to_json(value);

  std::string output;
  serializeJson(doc, output);
  ESP_LOGD(TAG, "Publishing meta delta: %s", output.c_str());
  send(output);
}

}  // namespace signalk
}  // namespace esphome
