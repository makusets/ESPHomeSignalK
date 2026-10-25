#ifdef USE_ESP32_FRAMEWORK_ESP_IDF
#pragma once
#include <string>
#include <cstdint>

#include "signalk.h"
#include "esp_websocket_client.h"
#include "esp_http_client.h"

namespace esphome {
namespace signalk {

esp_err_t _http_event_handler(esp_http_client_event_t *evt);

class SignalKEspIdf : public SignalK {
  friend esp_err_t _http_event_handler(esp_http_client_event_t *evt);

 public:
  SignalKEspIdf() {}
  ~SignalKEspIdf() {}

  void disconnect() override;
  bool connect(const std::string &path) override;
  bool discovery() override;
  bool send(const std::string &msg) override;
  bool is_connected() const override {
    return websocket_client != nullptr && esp_websocket_client_is_connected(websocket_client);
  }

  HttpResponse get(const std::string &path) override;
  HttpResponse post(const std::string &path, const std::string &msg) override;

 private:
  esp_websocket_client_handle_t websocket_client{nullptr};
  std::string auth_header_;
};

}  // namespace signalk
}  // namespace esphome

#endif
