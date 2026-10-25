#pragma once
#include "esphome/core/defines.h"

#ifdef USE_SIGNALK_CONNECTION_SETTINGS
#include "esphome/core/component.h"
#include "esphome/components/text/text.h"
#include "esphome/components/number/number.h"
#include "esphome/components/button/button.h"

namespace esphome {
namespace signalk {

class SignalK;
enum class ConnectionSettingField { HOST, USERNAME, PASSWORD };

struct ConnectionSettings {
  std::string host;
  uint16_t port{3000};
  std::string username;
  std::string password;
};

class SignalKConnectionText : public text::Text, public Component {
 public:
  void set_parent(SignalK *parent) { parent_ = parent; }
  void set_field(ConnectionSettingField field) { field_ = field; }
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1; }

 protected:
  void control(const std::string &value) override;
  SignalK *parent_;
  ConnectionSettingField field_;
};

class SignalKConnectionPort : public number::Number, public Component {
 public:
  void set_parent(SignalK *parent) { parent_ = parent; }
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1; }

 protected:
  void control(float value) override;
  SignalK *parent_;
};

class SignalKApplySettings : public button::Button {
 public:
  void set_parent(SignalK *parent) { parent_ = parent; }

 protected:
  void press_action() override;
  SignalK *parent_;
};

}  // namespace signalk
}  // namespace esphome
#endif
