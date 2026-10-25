#include "signalk_sensor.h"
#include "../signalk.h"
#include "esphome/core/log.h"
#include <cmath>

namespace esphome {
namespace signalk {
static const char *const TAG = "signalk.sensor";

void SignalkSensor::setup() { this->requires_update_ = false; }

void SignalkSensor::dump_config() {
  ESP_LOGCONFIG(TAG, "SignalK numeric sensor '%s': path='%s'", this->get_name().c_str(), this->path_.c_str());
}

void SignalkSensor::set_value(JsonVariant &value) {
  if (value.isNull()) {
    ESP_LOGW(TAG, "Path '%s': received null; waiting for a numeric value", path_.c_str());
    return;
  }
  if (!value.is<double>()) {
    ESP_LOGW(TAG, "Path '%s': expected a number, received %s", path_.c_str(),
             value.is<const char *>() ? "a string"
             : value.is<bool>()       ? "a boolean"
                                      : "an object or array");
    return;
  }
  const double numeric = value.as<double>();
  if (!std::isfinite(numeric)) {
    ESP_LOGW(TAG, "Path '%s': received a non-finite value", path_.c_str());
    return;
  }
  this->value_ = numeric;
  this->requires_update_ = true;
  received_value_ = true;
  last_value_ms_ = sk_millis();
  ESP_LOGD(TAG, "Path '%s': received numeric value %.6f; queued for publication", path_.c_str(), numeric);
}

void SignalkSensor::loop() {
  // Publish on the ESPHome loop, independently of the parent's polling interval.
  if (!parent_->is_connected()) {
    was_connected_ = false;
    return;
  }
  update();
  const uint32_t now = sk_millis();
  if (!was_connected_) {
    was_connected_ = true;
    last_diagnostic_ms_ = now;
    ESP_LOGD(TAG, "Path '%s': connected; monitoring stream values", path_.c_str());
  }
  if (now - last_diagnostic_ms_ >= 30000) {
    last_diagnostic_ms_ = now;
    if (!received_value_)
      ESP_LOGW(
          TAG,
          "Path '%s': connected but no valid numeric value received; check path, access permissions and source updates",
          path_.c_str());
    else
      ESP_LOGD(TAG, "Path '%s': last valid value received %u ms ago", path_.c_str(),
               static_cast<unsigned>(now - last_value_ms_));
  }
}

void SignalkSensor::disconnected() {
  SignalkSubscriber::disconnected();
  this->requires_update_ = false;
  received_value_ = false;
  was_connected_ = false;
}

void SignalkSensor::update() {
  if (this->requires_update_ && received_value_) {
    this->requires_update_ = false;
    float converted_value = convert_from_base(this->value_, this->unit_);
    if (!std::isfinite(converted_value)) {
      ESP_LOGW(TAG, "Path '%s': unit conversion produced a non-finite value", path_.c_str());
      return;
    }
    ESP_LOGD(TAG, "Path '%s': publishing %.6f (raw %.6f)", path_.c_str(), converted_value, this->value_);
    this->publish_state(converted_value);
  }
}

}  // namespace signalk
}  // namespace esphome
