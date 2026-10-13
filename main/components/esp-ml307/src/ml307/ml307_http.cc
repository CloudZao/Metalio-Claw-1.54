#include "ml307_http.h"
#include <esp_log.h>
#include <cstring>
#include <sstream>
#include <chrono>

static const char *TAG = "Ml307Http";

Ml307Http::Ml307Http(std::shared_ptr<AtUart> at_uart) : at_uart_(at_uart) {
    event_group_handle_ = xEventGroupCreate();

    urc_callback_it_ = at_uart_->RegisterUrcCallback([this](const std::string& command, const std::vector<AtArgumentValue>& arguments) {
        if (command == "MHTTPURC") {
            if (arguments[1].int_value == http_id_) {
                auto& type = arguments[0].string_value;
                if (type == "header") {
                    eof_ = false;
                    body_offset_ = 0;
                    body_.clear();
                    got_content_ = false;
                    last_urc_content_len_ = 0;
                    response_chunked_ = false;
                    status_code_ = arguments[2].int_value;
                    if (arguments.size() >= 5) {
                        ParseResponseHeaders(at_uart_->DecodeHex(arguments[4].string_value));
                    } else {
                        // FIXME: <header> 被分包发送
                        ESP_LOGE(TAG, "Missing header");
                    }
                    xEventGroupSetBits(event_group_handle_, ML307_HTTP_EVENT_HEADERS_RECEIVED);
                } else if (type == "content") {
                    // +MHTTPURC: "content",<httpid>,<content_len>,<sum_len>,<cur_len>,<data>
                    std::string decoded_data;
                    if (arguments.size() >= 6) {
                        at_uart_->DecodeHexAppend(decoded_data, arguments[5].string_value.c_str(), arguments[5].string_value.length());
                    } else {
                        // FIXME: <data> 被分包发送
                        ESP_LOGE(TAG, "Missing content");
                    }

                    std::lock_guard<std::mutex> lock(mutex_);
                    body_.append(decoded_data);
                    got_content_ = true;
                    last_content_time_ = std::chrono::steady_clock::now();
                    last_urc_content_len_ = arguments[2].int_value;

                    // EOF: 零长度收尾 URC（cur_len==0），或已收齐声明长度（sum>=content_len）。
                    // 部分固件对 chunked 响应会省略零长度收尾 URC，仅靠 cur_len==0 会让 ReadAll 空等满超时。
                    if (!eof_) {
                        const int content_len = arguments[2].int_value;
                        const int sum_len = arguments[3].int_value;
                        const int cur_len = arguments[4].int_value;
                        eof_ = (cur_len == 0) || (content_len > 0 && sum_len >= content_len);
                    }
                    
                    body_offset_ += arguments[4].int_value;
                    ESP_LOGI(TAG, "content id=%d cur=%d sum=%d/%d body=%u eof=%d",
                             http_id_,
                             arguments.size() > 4 ? arguments[4].int_value : -1,
                             arguments.size() > 3 ? arguments[3].int_value : -1,
                             arguments.size() > 2 ? arguments[2].int_value : -1,
                             (unsigned)body_.size(), eof_ ? 1 : 0);
                    if (arguments[3].int_value > body_offset_) {
                        ESP_LOGE(TAG, "body_offset_: %u, arguments[3].int_value: %d", body_offset_, arguments[3].int_value);
                        Close();
                        return;
                    }
                    cv_.notify_one();  // 使用条件变量通知
                } else if (type == "err") {
                    error_code_ = arguments[2].int_value;
                    int elapsed_ms = -1;
                    if (request_sent_) {
                        elapsed_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                          std::chrono::steady_clock::now() - request_sent_time_)
                                                          .count());
                    }
                    ESP_LOGE(TAG, "HTTP error id=%d code=%d elapsed_ms=%d",
                             http_id_, error_code_, elapsed_ms);
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        eof_ = true;
                    }
                    cv_.notify_all();
                    xEventGroupSetBits(event_group_handle_, ML307_HTTP_EVENT_ERROR);
                } else if (type == "ind") {
                    xEventGroupSetBits(event_group_handle_, ML307_HTTP_EVENT_IND);
                } else {
                    ESP_LOGE(TAG, "Unknown HTTP event: %s", type.c_str());
                }
            }
        } else if (command == "MHTTPCREATE") {
            http_id_ = arguments[0].int_value;
            instance_active_ = true;
            xEventGroupSetBits(event_group_handle_, ML307_HTTP_EVENT_INITIALIZED);
        } else if (command == "FIFO_OVERFLOW") {
            xEventGroupSetBits(event_group_handle_, ML307_HTTP_EVENT_ERROR);
            Close();
        }
    });
}

int Ml307Http::Read(char* buffer, size_t buffer_size) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (eof_ && body_.empty()) {
        return 0;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);

    while (!eof_ && body_.empty()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            ESP_LOGE(TAG, "Network operation failed");
            return -1;
        }

        auto wait = deadline - now;
        // CDN chunked 且 URC 未声明 content_len：最后一包后短空闲视为结束
        if (response_chunked_ && got_content_ && last_urc_content_len_ <= 0) {
            const auto idle_deadline =
                last_content_time_ +
                std::chrono::milliseconds(kChunkedIdleEofMs);
            if (now >= idle_deadline) {
                eof_ = true;
                ESP_LOGI(TAG,
                         "chunked idle EOF (Read) id=%d body=%u idle=%dms",
                         http_id_, (unsigned)body_offset_, kChunkedIdleEofMs);
                break;
            }
            if (idle_deadline - now < wait) {
                wait = idle_deadline - now;
            }
        }

        cv_.wait_for(lock, wait, [this] { return !body_.empty() || eof_; });
    }

    if (!instance_active_ && body_.empty()) {
        return -1;
    }

    if (eof_ && body_.empty()) {
        return 0;
    }

    size_t bytes_to_read = std::min(body_.size(), buffer_size);
    std::memcpy(buffer, body_.data(), bytes_to_read);
    body_.erase(0, bytes_to_read);

    return static_cast<int>(bytes_to_read);
}

int Ml307Http::Write(const char* buffer, size_t buffer_size) {
    if (buffer_size == 0) { // FIXME: 模组好像不支持发送空数据
        std::string command = "AT+MHTTPCONTENT=" + std::to_string(http_id_) + ",0,2,\"0D0A\"";
        at_uart_->SendCommand(command);
        ESP_LOGI(TAG, "Write end-chunk http_id=%d", http_id_);
        return 0;
    }

    // Write() 用于请求已发出后的续传，每一片都不是最后一包。
    const auto t0 = std::chrono::steady_clock::now();
    size_t buffer_sent = 0;
    unsigned chunk_idx = 0;
    ESP_LOGI(TAG, "Write start http_id=%d total=%u", http_id_,
             static_cast<unsigned>(buffer_size));
    while (buffer_sent < buffer_size) {
        size_t buffer_chunk_size = std::min(buffer_size - buffer_sent, kMaxContentChunk);
        std::string command = "AT+MHTTPCONTENT=" + std::to_string(http_id_) + ",1," + std::to_string(buffer_chunk_size);
        const auto chunk_t0 = std::chrono::steady_clock::now();
        if (!at_uart_->SendCommandWithData(command, 1000, true, buffer + buffer_sent, buffer_chunk_size)) {
            const int chunk_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - chunk_t0)
                    .count());
            const int total_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count());
            ESP_LOGE(TAG, "HTTP write failed cme=%d chunk=%u offset=%u total=%u chunk_ms=%d total_ms=%d",
                     at_uart_->GetCmeErrorCode(), chunk_idx,
                     static_cast<unsigned>(buffer_sent), static_cast<unsigned>(buffer_size),
                     chunk_ms, total_ms);
            return static_cast<int>(buffer_sent);
        }
        ESP_LOGD(TAG,
                 "Write chunk ok chunk_idx=%u chunk=%u offset=%u total=%u",
                 chunk_idx, static_cast<unsigned>(buffer_chunk_size),
                 static_cast<unsigned>(buffer_sent),
                 static_cast<unsigned>(buffer_size));
        buffer_sent += buffer_chunk_size;
        ++chunk_idx;
    }
    const int total_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count());
    ESP_LOGI(TAG, "Write done http_id=%d wrote=%u chunks=%u total_ms=%d",
             http_id_, static_cast<unsigned>(buffer_sent), chunk_idx, total_ms);
    return static_cast<int>(buffer_sent);
}

Ml307Http::~Ml307Http() {
    if (instance_active_) {
        Close();
    }

    at_uart_->UnregisterUrcCallback(urc_callback_it_);
    vEventGroupDelete(event_group_handle_);
}

void Ml307Http::SetHeader(const std::string& key, const std::string& value) {
    headers_[key] = value;
}

void Ml307Http::SetContent(std::string&& content) {
    content_ = std::make_optional(std::move(content));
}

void Ml307Http::SetTimeout(int timeout_ms) {
    timeout_ms_ = timeout_ms;
}

void Ml307Http::SetKeepAlive(bool enable) {
    ESP_LOGW(TAG, "SetKeepAlive is not implemented for ML307");
    // FIXME: Not implemented yet
    keep_alive_ = enable;
}

void Ml307Http::ParseResponseHeaders(const std::string& headers) {
    std::istringstream iss(headers);
    std::string line;
    while (std::getline(iss, line)) {
        std::istringstream line_iss(line);
        std::string key, value;
        std::getline(line_iss, key, ':');
        std::getline(line_iss, value);
    
        // 去除前后空格
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);
        
        response_headers_[key] = value;

        // 检查是否为chunked传输编码
        if (key == "Transfer-Encoding" && value.find("chunked") != std::string::npos) {
            response_chunked_ = true;
            ESP_LOGI(TAG, "Found chunked transfer encoding");
        }
    }
}

bool Ml307Http::SendContent(const char* data, size_t size) {
    size_t offset = 0;
    do {
        const size_t chunk = std::min(kMaxContentChunk, size - offset);
        const bool last = (offset + chunk >= size);
        // eof=1 还有后续片；eof=0 为最后一包（含不超过 4KB 的整包）。
        const std::string command = "AT+MHTTPCONTENT=" + std::to_string(http_id_) + "," +
                                    std::to_string(last ? 0 : 1) + "," + std::to_string(chunk);
        if (!at_uart_->SendCommandWithData(command, 1000, true, data + offset, chunk)) {
            ESP_LOGE(TAG, "MHTTPCONTENT failed cme=%d chunk=%u offset=%u total=%u",
                     at_uart_->GetCmeErrorCode(), static_cast<unsigned>(chunk),
                     static_cast<unsigned>(offset), static_cast<unsigned>(size));
            return false;
        }
        offset += chunk;
    } while (offset < size);

    if (size > kMaxContentChunk) {
        ESP_LOGI(TAG, "MHTTPCONTENT ok len=%u chunks=%u", static_cast<unsigned>(size),
                 static_cast<unsigned>((size + kMaxContentChunk - 1) / kMaxContentChunk));
    } else {
        ESP_LOGI(TAG, "MHTTPCONTENT ok len=%u", static_cast<unsigned>(size));
    }
    return true;
}

bool Ml307Http::SendAtChecked(const std::string& command, const char* step, size_t timeout_ms) {
    const bool ok = at_uart_->SendCommand(command, timeout_ms);
    if (!ok) {
        ESP_LOGE(TAG, "Network operation failed");
        return false;
    }
    return true;
}

bool Ml307Http::Open(const std::string& method, const std::string& url) {
    method_ = method;
    url_ = url;
    request_sent_ = false;
    error_code_ = -1;
    
    // 判断是否为需要发送内容的HTTP方法
    bool method_supports_content = (method_ == "POST" || method_ == "PUT");
    
    // 解析URL
    size_t protocol_end = url.find("://");
    if (protocol_end != std::string::npos) {
        protocol_ = url.substr(0, protocol_end);
        size_t host_start = protocol_end + 3;
        size_t path_start = url.find("/", host_start);
        if (path_start != std::string::npos) {
            host_ = url.substr(host_start, path_start - host_start);
            path_ = url.substr(path_start);
        } else {
            host_ = url.substr(host_start);
            path_ = "/";
        }
    } else {
        // URL格式不正确
        ESP_LOGE(TAG, "Invalid URL format");
        return false;
    }

    // 上次未关干净时先删；并对 0–3 做轻量清理，避免第二次 MHTTPCREATE 挂起
    if (instance_active_) {
        at_uart_->SendCommand("AT+MHTTPDEL=" + std::to_string(http_id_), 500);
        instance_active_ = false;
    }
    for (int id = 0; id < 4; ++id) {
        at_uart_->SendCommand("AT+MHTTPDEL=" + std::to_string(id), 300);
    }
    xEventGroupClearBits(event_group_handle_,
                         ML307_HTTP_EVENT_INITIALIZED | ML307_HTTP_EVENT_ERROR |
                             ML307_HTTP_EVENT_HEADERS_RECEIVED |
                             ML307_HTTP_EVENT_IND);

    // 创建HTTP连接
    std::string command = "AT+MHTTPCREATE=\"" + protocol_ + "://" + host_ + "\"";
    if (!SendAtChecked(command, "MHTTPCREATE")) {
        return false;
    }

    auto bits = xEventGroupWaitBits(event_group_handle_, ML307_HTTP_EVENT_INITIALIZED, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms_));
    if (!(bits & ML307_HTTP_EVENT_INITIALIZED)) {
        ESP_LOGE(TAG, "Timeout waiting for HTTP connection to be created");
        return false;
    }
    request_chunked_ = method_supports_content && !content_.has_value();

    if (protocol_ == "https") {
        // Align with TCP SSL path: disable cert verify before enabling HTTP SSL.
        if (!SendAtChecked("AT+MSSLCFG=\"auth\",0,0", "MSSLCFG auth")) {
            return false;
        }
        command = "AT+MHTTPCFG=\"ssl\"," + std::to_string(http_id_) + ",1,0";
        if (!SendAtChecked(command, "MHTTPCFG ssl")) {
            return false;
        }
    }

    if (request_chunked_) {
        command = "AT+MHTTPCFG=\"chunked\"," + std::to_string(http_id_) + ",1";
        if (!SendAtChecked(command, "MHTTPCFG chunked")) {
            return false;
        }
    }

    // Set HEX encoding OFF
    command = "AT+MHTTPCFG=\"encoding\"," + std::to_string(http_id_) + ",0,0";
    if (!SendAtChecked(command, "MHTTPCFG encoding off")) {
        return false;
    }

    // Set timeout (seconds): connect timeout, response timeout, input timeout
    // sprintf(command, "AT+MHTTPCFG=\"timeout\",%d,%d,%d,%d", http_id_, timeout_ms_ / 1000, timeout_ms_ / 1000, timeout_ms_ / 1000);
    // modem_.Command(command);

    // Set headers
    for (auto it = headers_.begin(); it != headers_.end(); it++) {
        auto line = it->first + ": " + it->second;
        bool is_last = std::next(it) == headers_.end();
        command = "AT+MHTTPHEADER=" + std::to_string(http_id_) + "," + std::to_string(is_last ? 0 : 1) + "," + std::to_string(line.size()) + ",\"" + line + "\"";
        if (!at_uart_->SendCommand(command)) {
            ESP_LOGE(TAG, "MHTTPHEADER failed cme=%d key=%s", at_uart_->GetCmeErrorCode(),
                     it->first.c_str());
            return false;
        }
    }

    if (method_supports_content && content_.has_value()) {
        const std::string& content = content_.value();
        if (!SendContent(content.data(), content.size())) {
            return false;
        }
        content_ = std::nullopt;
    }

    // Set HEX encoding ON
    command = "AT+MHTTPCFG=\"encoding\"," + std::to_string(http_id_) + ",1,1";
    if (!SendAtChecked(command, "MHTTPCFG encoding on")) {
        return false;
    }

    // Send request
    // method to value: 1. GET 2. POST 3. PUT 4. DELETE 5. HEAD
    const char* methods[6] = {"UNKNOWN", "GET", "POST", "PUT", "DELETE", "HEAD"};
    int method_value = 1;
    for (int i = 0; i < 6; i++) {
        if (strcmp(methods[i], method_.c_str()) == 0) {
            method_value = i;
            break;
        }
    }
    command = "AT+MHTTPREQUEST=" + std::to_string(http_id_) + "," + std::to_string(method_value) + ",0,";
    const std::string request_cmd = command + at_uart_->EncodeHex(path_);
    if (!at_uart_->SendCommand(request_cmd)) {
        ESP_LOGE(TAG, "Network operation failed");
        return false;
    }
    request_sent_time_ = std::chrono::steady_clock::now();
    request_sent_ = true;

    if (request_chunked_) {
        auto bits = xEventGroupWaitBits(event_group_handle_, ML307_HTTP_EVENT_IND, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms_));
        if (!(bits & ML307_HTTP_EVENT_IND)) {
            ESP_LOGE(TAG, "Timeout waiting for HTTP IND");
            return false;
        }
    }
    return true;
}

bool Ml307Http::FetchHeaders() {
    // Wait for headers
    auto bits = xEventGroupWaitBits(event_group_handle_, ML307_HTTP_EVENT_HEADERS_RECEIVED | ML307_HTTP_EVENT_ERROR, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms_));
    if (bits & ML307_HTTP_EVENT_ERROR) {
        int elapsed_ms = -1;
        if (request_sent_) {
            elapsed_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - request_sent_time_)
                                              .count());
        }
        ESP_LOGE(TAG, "HTTP request error: %s (code=%d elapsed_ms=%d)",
                 ErrorCodeToString(error_code_).c_str(), error_code_, elapsed_ms);
        return false;
    }
    if (!(bits & ML307_HTTP_EVENT_HEADERS_RECEIVED)) {
        ESP_LOGE(TAG, "Timeout waiting for HTTP headers to be received");
        return false;
    }

    auto it = response_headers_.find("Content-Length");
    if (it != response_headers_.end()) {
        content_length_ = std::stoul(it->second);
    }

    ESP_LOGI(TAG, "HTTP request successful, status code: %d", status_code_);
    return true;
}

int Ml307Http::GetStatusCode() {
    if (status_code_ == -1) {
        if (!FetchHeaders()) {
            return -1;
        }
    }
    return status_code_;
}

size_t Ml307Http::GetBodyLength() {
    if (status_code_ == -1) {
        if (!FetchHeaders()) {
            return 0;
        }
    }
    return content_length_;
}

std::string Ml307Http::ReadAll() {
    std::unique_lock<std::mutex> lock(mutex_);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);

    while (!eof_) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            ESP_LOGE(TAG, "Network operation failed");
            return body_;
        }

        auto wait = deadline - now;
        if (response_chunked_ && got_content_ && last_urc_content_len_ <= 0 &&
            !body_.empty()) {
            const auto idle_deadline =
                last_content_time_ +
                std::chrono::milliseconds(kChunkedIdleEofMs);
            if (now >= idle_deadline) {
                eof_ = true;
                ESP_LOGI(TAG,
                         "chunked idle EOF (ReadAll) id=%d body=%u idle=%dms",
                         http_id_, (unsigned)body_.size(), kChunkedIdleEofMs);
                break;
            }
            if (idle_deadline - now < wait) {
                wait = idle_deadline - now;
            }
        }

        cv_.wait_for(lock, wait, [this] { return eof_; });
    }

    return body_;
}

int Ml307Http::GetLastError() {
    return error_code_;
}

void Ml307Http::Close() {
    // 即使 instance_active_ 为假也尝试删除，避免槽位残留导致下次 CREATE 挂起
    if (instance_active_ || http_id_ >= 0) {
        std::string command = "AT+MHTTPDEL=" + std::to_string(http_id_ >= 0 ? http_id_ : 0);
        at_uart_->SendCommand(command, 500);
    }

    instance_active_ = false;
    eof_ = true;
    cv_.notify_all();
    ESP_LOGI(TAG, "HTTP connection closed, ID: %d", http_id_);
}

std::string Ml307Http::ErrorCodeToString(int error_code) {
    switch (error_code) {
        case 1: return "Domain name resolution failed";
        case 2: return "Connection to server failed";
        case 3: return "Connection to server timeout";
        case 4: return "SSL handshake failed";
        case 5: return "Connection abnormal disconnection";
        case 6: return "Request response timeout";
        case 7: return "Data reception parsing failed";
        case 8: return "Cache space insufficient";
        case 9: return "Data packet loss";
        case 10: return "File write failed";
        case 255: return "Unknown error";
        default: return "Undefined error";
    }
}

std::string Ml307Http::GetResponseHeader(const std::string& key) const {
    auto it = response_headers_.find(key);
    if (it != response_headers_.end()) {
        return it->second;
    }
    return "";
}