#include "ml307_tcp.h"
#include <esp_log.h>
#include <algorithm>
#include <cstring>

#define TAG "Ml307Tcp"

Ml307Tcp::Ml307Tcp(std::shared_ptr<AtUart> at_uart, int tcp_id) : at_uart_(at_uart), tcp_id_(tcp_id) {
    event_group_handle_ = xEventGroupCreate();

    urc_callback_it_ = at_uart_->RegisterUrcCallback([this](const std::string& command, const std::vector<AtArgumentValue>& arguments) {
        if (command == "MIPOPEN" && arguments.size() == 2) {
            if (arguments[0].int_value == tcp_id_) {
                const int result = arguments[1].int_value;
                ESP_LOGI(TAG, "MIPOPEN URC id=%d result=%d", tcp_id_, result);
                connected_ = result == 0;
                if (connected_) {
                    instance_active_ = true;
                    xEventGroupClearBits(event_group_handle_, ML307_TCP_DISCONNECTED | ML307_TCP_ERROR);
                    xEventGroupSetBits(event_group_handle_, ML307_TCP_CONNECTED);
                } else {
                    last_error_ = result;  // Store error code from MIPOPEN response
                    xEventGroupSetBits(event_group_handle_, ML307_TCP_ERROR);
                }
            }
        } else if (command == "MIPCLOSE" && arguments.size() == 1) {
            if (arguments[0].int_value == tcp_id_) {
                instance_active_ = false;
                xEventGroupSetBits(event_group_handle_, ML307_TCP_DISCONNECTED);
            }
        } else if (command == "MIPSEND" && arguments.size() == 2) {
            if (arguments[0].int_value == tcp_id_) {
                xEventGroupSetBits(event_group_handle_, ML307_TCP_SEND_COMPLETE);
            }
        } else if (command == "MIPURC" && arguments.size() >= 3) {
            if (arguments[1].int_value == tcp_id_) {
                if (arguments[0].string_value == "rtcp") {
                    if (connected_ && stream_callback_) {
                        stream_callback_(at_uart_->DecodeHex(arguments[3].string_value));
                    }
                } else if (arguments[0].string_value == "disconn") {
                    if (connected_) {
                        connected_ = false;
                        if (disconnect_callback_) {
                            disconnect_callback_();
                        }
                    }
                    instance_active_ = false;
                    xEventGroupSetBits(event_group_handle_, ML307_TCP_DISCONNECTED);
                } else {
                    ESP_LOGE(TAG, "Unknown MIPURC command: %s", arguments[0].string_value.c_str());
                }
            }
        } else if (command == "MIPSTATE") {
            // 空闲态常为 +MIPSTATE: id,,,,"INITIAL"；空字段可能被丢掉变成 [id,"INITIAL"]。
            // size>=1 且 id 匹配即置 INITIALIZED（对齐 UDP）。
            if (arguments.size() >= 1 && arguments[0].int_value == tcp_id_) {
                const char* state = "";
                if (arguments.size() >= 5) {
                    state = arguments[4].string_value.c_str();
                    connected_ = arguments[4].string_value == "CONNECTED";
                    instance_active_ = arguments[4].string_value != "INITIAL";
                } else if (arguments.size() >= 2) {
                    state = arguments.back().string_value.c_str();
                    connected_ = arguments.back().string_value == "CONNECTED";
                    instance_active_ = arguments.back().string_value != "INITIAL";
                } else {
                    connected_ = false;
                    instance_active_ = false;
                }
                ESP_LOGI(TAG, "MIPSTATE id=%d argc=%u state=%s connected=%d active=%d",
                         tcp_id_, (unsigned)arguments.size(), state,
                         connected_ ? 1 : 0, instance_active_ ? 1 : 0);
                xEventGroupSetBits(event_group_handle_, ML307_TCP_INITIALIZED);
            }
        } else if (command == "MDNSGIP" || command == "CDNSGIP" || command == "MDNSCFG") {
            resolved_ip_.clear();
            for (const auto& arg : arguments) {
                if (IsIpv4(arg.string_value)) {
                    resolved_ip_ = arg.string_value;
                    break;
                }
            }
            xEventGroupSetBits(event_group_handle_, ML307_TCP_DNS_DONE);
        } else if (command == "FIFO_OVERFLOW") {
            xEventGroupSetBits(event_group_handle_, ML307_TCP_ERROR);
            Disconnect();
        }
    });
}

Ml307Tcp::~Ml307Tcp() {
    Disconnect();
    at_uart_->UnregisterUrcCallback(urc_callback_it_);
    if (event_group_handle_) {
        vEventGroupDelete(event_group_handle_);
    }
}

bool Ml307Tcp::IsIpv4(const std::string& value) {
    if (value.empty() || value.find('.') == std::string::npos) {
        return false;
    }
    int dots = 0;
    for (char c : value) {
        if (c == '.') {
            dots++;
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    return dots == 3;
}

bool Ml307Tcp::ResolveHost(const std::string& host, std::string& ip) {
    if (IsIpv4(host)) {
        ip = host;
        return true;
    }

    resolved_ip_.clear();
    xEventGroupClearBits(event_group_handle_, ML307_TCP_DNS_DONE);
    if (!at_uart_->SendCommand("AT+MDNSGIP=\"" + host + "\"", TCP_DNS_TIMEOUT_MS)) {
        if (!at_uart_->SendCommand("AT+CDNSGIP=\"" + host + "\"", TCP_DNS_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "Network operation failed");
            return false;
        }
    }

    auto bits = xEventGroupWaitBits(event_group_handle_, ML307_TCP_DNS_DONE, pdTRUE, pdFALSE,
                                    pdMS_TO_TICKS(TCP_DNS_TIMEOUT_MS));
    if (!(bits & ML307_TCP_DNS_DONE) || resolved_ip_.empty()) {
        ESP_LOGW(TAG, "Network operation failed");
        return false;
    }
    ip = resolved_ip_;
    return true;
}

bool Ml307Tcp::TryOpen(const std::string& cmd, int id) {
    tcp_id_ = id;
    last_error_ = 0;
    connected_ = false;
    xEventGroupClearBits(event_group_handle_,
                         ML307_TCP_CONNECTED | ML307_TCP_ERROR | ML307_TCP_DISCONNECTED);
    if (!at_uart_->SendCommand(cmd, TCP_CONNECT_TIMEOUT_MS)) {
        last_error_ = at_uart_->GetCmeErrorCode();
        ESP_LOGW(TAG, "Network operation failed");
        at_uart_->SendCommand("AT+MIPCLOSE=" + std::to_string(id), 1000);
        return false;
    }

    auto bits = xEventGroupWaitBits(
        event_group_handle_, ML307_TCP_CONNECTED | ML307_TCP_ERROR, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(TCP_CONNECT_TIMEOUT_MS));
    if (bits & ML307_TCP_CONNECTED) {
        instance_active_ = true;
        return true;
    }
    if (!(bits & ML307_TCP_ERROR)) {
        // 超时：与 ERROR 区分，便于上层日志
        last_error_ = -1;
        ESP_LOGW(TAG, "MIPOPEN timeout id=%d", id);
    }
    at_uart_->SendCommand("AT+MIPCLOSE=" + std::to_string(id), 1000);
    return false;
}

bool Ml307Tcp::Connect(const std::string& host, int port) {
    const int requested_id = tcp_id_;

    xEventGroupClearBits(event_group_handle_,
                         ML307_TCP_CONNECTED | ML307_TCP_DISCONNECTED |
                             ML307_TCP_ERROR | ML307_TCP_INITIALIZED |
                             ML307_TCP_DNS_DONE);
    last_error_ = 0;
    connected_ = false;
    instance_active_ = false;

    std::string peer;
    if (!ResolveHost(host, peer)) {
        peer = host;
    }

    // MQTT 占用 id=0；预关闭 1/2/3，避免残留占位（对齐 UDP）
    for (int id : {1, 2, 3}) {
        at_uart_->SendCommand("AT+MIPCLOSE=" + std::to_string(id), 500);
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    int ids[4];
    int id_n = 0;
    auto push_id = [&](int id) {
        if (id <= 0) {
            return;
        }
        for (int i = 0; i < id_n; i++) {
            if (ids[i] == id) {
                return;
            }
        }
        if (id_n < 4) {
            ids[id_n++] = id;
        }
    };
    // 候选：requested → 1 → 2 → 3
    push_id(requested_id);
    push_id(1);
    push_id(2);
    push_id(3);

    const std::string port_s = std::to_string(port);

    for (int ii = 0; ii < id_n; ii++) {
        const int id = ids[ii];
        tcp_id_ = id;

        // 查询并清理该 id
        xEventGroupClearBits(event_group_handle_,
                             ML307_TCP_INITIALIZED | ML307_TCP_ERROR |
                                 ML307_TCP_DISCONNECTED | ML307_TCP_CONNECTED);
        connected_ = false;
        instance_active_ = false;

        std::string command = "AT+MIPSTATE=" + std::to_string(id);
        if (!at_uart_->SendCommand(command, 3000)) {
            ESP_LOGW(TAG, "MIPSTATE query failed id=%d, retry once", id);
            at_uart_->SendCommand("AT", 1000);
            at_uart_->SendCommand(command, 3000);
        }
        auto bits = xEventGroupWaitBits(
            event_group_handle_, ML307_TCP_INITIALIZED | ML307_TCP_ERROR, pdTRUE,
            pdFALSE, pdMS_TO_TICKS(3000));
        if (bits & ML307_TCP_ERROR) {
            ESP_LOGW(TAG, "TCP connect aborted during MIPSTATE id=%d", id);
            tcp_id_ = requested_id;
            return false;
        }
        if (instance_active_ || connected_) {
            at_uart_->SendCommand("AT+MIPCLOSE=" + std::to_string(id), 2000);
            xEventGroupWaitBits(event_group_handle_,
                                ML307_TCP_DISCONNECTED | ML307_TCP_ERROR, pdTRUE,
                                pdFALSE, pdMS_TO_TICKS(2000));
            connected_ = false;
            instance_active_ = false;
        }

        if (!ConfigureSsl(port)) {
            ESP_LOGW(TAG, "SSL cfg failed id=%d, try next id", id);
            continue;
        }
        command = "AT+MIPCFG=\"encoding\"," + std::to_string(id) + ",1,1";
        if (!at_uart_->SendCommand(command)) {
            ESP_LOGW(TAG, "encoding cfg failed id=%d, try next id", id);
            continue;
        }

        const std::string id_s = std::to_string(id);
        const std::string cmds[] = {
            "AT+MIPOPEN=" + id_s + ",\"TCP\",\"" + peer + "\"," + port_s + ",,0",
            "AT+MIPOPEN=" + id_s + ",\"TCP\",\"" + peer + "\"," + port_s,
            "AT+MIPOPEN=" + id_s + ",\"TCP\",\"" + peer + "\"," + port_s + ",0",
        };

        bool id_unusable = false;
        for (const auto& cmd : cmds) {
            if (TryOpen(cmd, id)) {
                return true;
            }
            // 800=该 socket 不可用；50=参数格式不支持
            if (last_error_ == 800) {
                ESP_LOGW(TAG, "TCP id=%d unavailable (800), try next id", id);
                id_unusable = true;
                break;
            }
            if (last_error_ == 50) {
                ESP_LOGW(TAG, "TCP MIPOPEN format unsupported (50), try next fmt");
                continue;
            }
            // 其它错误：继续试下一格式，穷尽后再换 id
            ESP_LOGW(TAG, "TCP MIPOPEN failed id=%d mipopen_err=%d, try next fmt/id",
                     id, last_error_);
        }
        if (id_unusable) {
            continue;
        }
    }

    ESP_LOGE(TAG, "Network operation failed");
    tcp_id_ = requested_id;
    return false;
}

void Ml307Tcp::Disconnect() {
    // 始终唤醒 Connect 中的 WaitBits，便于上层 Close 取消建连
    xEventGroupSetBits(event_group_handle_, ML307_TCP_ERROR | ML307_TCP_DISCONNECTED);

    if (instance_active_) {
        std::string command = "AT+MIPCLOSE=" + std::to_string(tcp_id_);
        if (at_uart_->SendCommand(command)) {
            xEventGroupWaitBits(event_group_handle_, ML307_TCP_DISCONNECTED, pdTRUE,
                                pdFALSE, pdMS_TO_TICKS(TCP_CONNECT_TIMEOUT_MS));
        }
        instance_active_ = false;
    }

    if (connected_) {
        connected_ = false;
        if (disconnect_callback_) {
            disconnect_callback_();
        }
    }
}

bool Ml307Tcp::ConfigureSsl(int port) {
    std::string command = "AT+MIPCFG=\"ssl\"," + std::to_string(tcp_id_) + ",0,0";
    if (!at_uart_->SendCommand(command)) {
        ESP_LOGE(TAG, "Failed to set SSL configuration");
        return false;
    }
    return true;
}

int Ml307Tcp::Send(const std::string& data) {
    const size_t MAX_PACKET_SIZE = 1460 / 2;
    size_t total_sent = 0;

    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    // 在循环外预先分配command
    std::string command;
    command.reserve(32 + MAX_PACKET_SIZE * 2);  // 预分配最大可能需要的空间

    while (total_sent < data.size()) {
        size_t chunk_size = std::min(data.size() - total_sent, MAX_PACKET_SIZE);

        // 重置command并构建新的命令，利用预分配的容量
        command.clear();
        command += "AT+MIPSEND=";
        command += std::to_string(tcp_id_);
        command += ",";
        command += std::to_string(chunk_size);
        command += ",";

        // 直接在command字符串上进行十六进制编码
        at_uart_->EncodeHexAppend(command, data.data() + total_sent, chunk_size);
        command += "\r\n";

        // 根据波特率和命令长度动态计算超时：传输时间(10位/字节) + 处理余量
        int baud = at_uart_->GetBaudRate();
        if (baud <= 0) baud = 115200;
        size_t bytes_to_tx = command.size();
        // 发送位数≈字节*10（1起始+8数据+1停止），转毫秒
        uint32_t tx_time_ms = static_cast<uint32_t>((bytes_to_tx * 10ULL * 1000ULL) / static_cast<uint32_t>(baud));
        uint32_t timeout_ms = tx_time_ms + 100; // 余量

        if (!at_uart_->SendCommand(command, timeout_ms, false)) {
            ESP_LOGE(TAG, "Failed to send data chunk");
            Disconnect();
            return -1;
        }

        auto bits = xEventGroupWaitBits(event_group_handle_, ML307_TCP_SEND_COMPLETE, pdTRUE, pdFALSE, pdMS_TO_TICKS(TCP_CONNECT_TIMEOUT_MS));
        if (!(bits & ML307_TCP_SEND_COMPLETE)) {
            ESP_LOGE(TAG, "No send confirmation received");
            return -1;
        }

        total_sent += chunk_size;
    }
    return data.size();
}

int Ml307Tcp::GetLastError() {
    return last_error_;
}
