#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"

#include "esphome/components/signalk/signalk_subscriber.h"
#include "esphome/components/signalk/unit_conversions.h"

namespace esphome {
namespace signalk {

class SignalkSensor : public SignalKSubscriberTemplated<double>, public sensor::Sensor, public Component {
 public:
  void setup() override;
  void dump_config() override;

  void set_unit(Unit unit) { this->unit_ = unit; }
  void update() override;
  void loop() override;
  void set_value(JsonVariant &value) override;
  void disconnected() override;

 private:
  Unit unit_;
  bool received_value_{false};
  bool was_connected_{false};
  uint32_t last_value_ms_{0};
  uint32_t last_diagnostic_ms_{0};
};

}  // namespace signalk
}  // namespace esphome
