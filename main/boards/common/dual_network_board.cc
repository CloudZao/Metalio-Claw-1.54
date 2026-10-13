#include "dual_network_board.h"
#include "application.h"
#include "display.h"
#include "assets/lang_config.h"
#include "settings.h"
#include <driver/gpio.h>
#include <esp_log.h>
#include <at_uart.h>
#include <atomic>

static const char *TAG = "DualNetworkBoard";

DualNetworkBoard::DualNetworkBoard(gpio_num_t ml307_tx_pin, gpio_num_t ml307_rx_pin,
                                   gpio_num_t ml307_dtr_pin, int32_t default_net_type)
    : Board(),
      ml307_tx_pin_(ml307_tx_pin),
      ml307_rx_pin_(ml307_rx_pin),
      ml307_dtr_pin_(ml307_dtr_pin) {
    network_type_ = LoadNetworkTypeFromSettings(default_net_type);
    InitializeCurrentBoard();
}

NetworkType DualNetworkBoard::LoadNetworkTypeFromSettings(int32_t default_net_type) {
    Settings settings("network", true);
    int network_type = settings.GetInt("type", default_net_type);
    return network_type == 1 ? NetworkType::ML307 : NetworkType::WIFI;
}

void DualNetworkBoard::SaveNetworkTypeToSettings(NetworkType type) {
    Settings settings("network", true);
    int network_type = (type == NetworkType::ML307) ? 1 : 0;
    settings.SetInt("type", network_type);
}

void DualNetworkBoard::InitializeCurrentBoard() {
    if (network_type_ == NetworkType::ML307) {
        ESP_LOGI(TAG, "Initialize ML307 board");
        current_board_ = std::make_unique<Ml307Board>(ml307_tx_pin_, ml307_rx_pin_,
                                                      ml307_dtr_pin_);
    } else {
        ESP_LOGI(TAG, "Initialize WiFi board");
        current_board_ = std::make_unique<WifiBoard>();
    }
}

void DualNetworkBoard::SwitchNetworkType() {
    auto display = GetDisplay();
    if (network_type_ == NetworkType::WIFI) {
        SaveNetworkTypeToSettings(NetworkType::ML307);
        display->ShowNotification(Lang::Strings::SWITCH_TO_4G_NETWORK);
    } else {
        SaveNetworkTypeToSettings(NetworkType::WIFI);
        display->ShowNotification(Lang::Strings::SWITCH_TO_WIFI_NETWORK);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    auto& app = Application::GetInstance();
    app.Reboot();
}

std::string DualNetworkBoard::GetBoardType() {
    return current_board_->GetBoardType();
}

void DualNetworkBoard::StartNetwork() {
    auto display = Board::GetInstance().GetDisplay();

    if (network_type_ == NetworkType::WIFI) {
        // ESP32 reset does not guarantee a modem reset. Check for an old broker session.
        auto uart = std::make_shared<AtUart>(ml307_tx_pin_, ml307_rx_pin_, ml307_dtr_pin_);
        uart->Initialize();
        if (uart->SetBaudRate(921600, 2000)) {
            std::atomic<int> state{-1};
            auto callback = uart->RegisterUrcCallback([&state](const std::string& command,
                                                            const std::vector<AtArgumentValue>& args) {
                if (command == "MQTTSTATE" && !args.empty()) state = args[0].int_value;
            });
            const bool queried = uart->SendCommand("AT+MQTTSTATE=0", 1500);
            if (queried && state.load() == 2) {
                const bool closed = uart->SendCommand("AT+MQTTDISC=0", 2000);
                ESP_LOGW(TAG, "WiFi residual modem MQTT disconnected=%d", closed ? 1 : 0);
            }
            uart->UnregisterUrcCallback(callback);
        }
        uart.reset();
        display->SetStatus(Lang::Strings::CONNECTING);
    } else {
        display->SetStatus(Lang::Strings::DETECTING_MODULE);
    }
    current_board_->StartNetwork();
}

NetworkInterface* DualNetworkBoard::GetNetwork() {
    return current_board_->GetNetwork();
}

const char* DualNetworkBoard::GetNetworkStateIcon() {
    return current_board_->GetNetworkStateIcon();
}

void DualNetworkBoard::SetPowerSaveMode(bool enabled) {
    current_board_->SetPowerSaveMode(enabled);
}

std::string DualNetworkBoard::GetBoardJson() {
    return current_board_->GetBoardJson();
}

std::string DualNetworkBoard::GetDeviceStatusJson() {
    return current_board_->GetDeviceStatusJson();
}
