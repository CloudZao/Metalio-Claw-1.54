#ifndef MQTT_PROTOCOL_H
#define MQTT_PROTOCOL_H


#include "protocol.h"
#include <mqtt.h>
#include <udp.h>
#include <cJSON.h>
#include <mbedtls/aes.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

#include <functional>
#include <string>
#include <map>
#include <mutex>

#define MQTT_PING_INTERVAL_SECONDS 90
// Idle 后台重连间隔；活跃会话断线走立即重连，不等此定时器。
#define MQTT_RECONNECT_INTERVAL_MS 5000

#define MQTT_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class MqttProtocol : public Protocol {
public:
    MqttProtocol();
    ~MqttProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;

private:
    EventGroupHandle_t event_group_handle_;

    std::string publish_topic_;

    std::mutex channel_mutex_;
    std::unique_ptr<Mqtt> mqtt_;
    std::unique_ptr<Udp> udp_;
    mbedtls_aes_context aes_ctx_;
    std::string aes_nonce_;
    std::string udp_server_;
    int udp_port_;
    uint32_t local_sequence_;
    uint32_t remote_sequence_;
    esp_timer_handle_t reconnect_timer_;
    // StartMqttClient 主动销毁旧客户端时抑制 OnDisconnected 重连/省电逻辑。
    bool closing_mqtt_ = false;
    bool reconnect_in_progress_ = false;

    bool StartMqttClient(bool report_error=false);
    void HandleMqttDisconnected();
    void ScheduleMqttReconnect(bool immediate);
    void UpdateWifiPowerSaveAfterMqttEvent(bool mqtt_connected);
    void ParseServerHello(const cJSON* root);
    void ProcessIncomingAudioPacket(const std::string& data);
    std::string DecodeHexString(const std::string& hex_string);

    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
};


#endif // MQTT_PROTOCOL_H
