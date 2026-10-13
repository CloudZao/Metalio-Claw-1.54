#ifndef DUAL_NETWORK_BOARD_H
#define DUAL_NETWORK_BOARD_H

#include "board.h"
#include "wifi_board.h"
#include "ml307_board.h"
#include <memory>

enum class NetworkType {
    WIFI,
    ML307
};

// 双网络板卡：WiFi 与 ML307 之间切换（星智 1.54）
class DualNetworkBoard : public Board {
private:
    std::unique_ptr<Board> current_board_;
    NetworkType network_type_ = NetworkType::ML307;

    gpio_num_t ml307_tx_pin_;
    gpio_num_t ml307_rx_pin_;
    gpio_num_t ml307_dtr_pin_;

    void SaveNetworkTypeToSettings(NetworkType type);
    void InitializeCurrentBoard();

public:
    DualNetworkBoard(gpio_num_t ml307_tx_pin, gpio_num_t ml307_rx_pin,
                     gpio_num_t ml307_dtr_pin = GPIO_NUM_NC,
                     int32_t default_net_type = 1);

    virtual ~DualNetworkBoard() = default;

    static NetworkType LoadNetworkTypeFromSettings(int32_t default_net_type);

    void SwitchNetworkType();

    NetworkType GetNetworkType() const { return network_type_; }

    Board& GetCurrentBoard() const { return *current_board_; }

    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual NetworkInterface* GetNetwork() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual std::string GetBoardJson() override;
    virtual std::string GetDeviceStatusJson() override;
};

#endif // DUAL_NETWORK_BOARD_H
