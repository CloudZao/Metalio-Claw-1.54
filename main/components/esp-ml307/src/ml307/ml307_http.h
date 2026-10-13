#ifndef ML307_HTTP_TRANSPORT_H
#define ML307_HTTP_TRANSPORT_H

#include "at_uart.h"
#include "http.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <map>
#include <string>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <chrono>

#define ML307_HTTP_EVENT_INITIALIZED (1 << 0)
#define ML307_HTTP_EVENT_ERROR (1 << 2)
#define ML307_HTTP_EVENT_HEADERS_RECEIVED (1 << 3)
#define ML307_HTTP_EVENT_IND (1 << 4)

class Ml307Http : public Http {
public:
    Ml307Http(std::shared_ptr<AtUart> at_uart);
    ~Ml307Http();

    void SetTimeout(int timeout_ms) override;
    void SetHeader(const std::string& key, const std::string& value) override;
    void SetContent(std::string&& content) override;
    void SetKeepAlive(bool enable) override;
    bool Open(const std::string& method, const std::string& url) override;
    void Close() override;
    int Read(char* buffer, size_t buffer_size) override;
    int Write(const char* buffer, size_t buffer_size) override;

    int GetStatusCode() override;
    std::string GetResponseHeader(const std::string& key) const override;
    size_t GetBodyLength() override;
    std::string ReadAll() override;
    int GetLastError() override;

private:
    std::shared_ptr<AtUart> at_uart_;
    EventGroupHandle_t event_group_handle_;
    std::mutex mutex_;
    std::condition_variable cv_;

    int http_id_ = -1;
    int status_code_ = -1;
    int error_code_ = -1;
    int timeout_ms_ = 30000;
    std::string rx_buffer_;
    std::list<UrcCallback>::iterator urc_callback_it_;
    std::map<std::string, std::string> headers_;
    std::string url_;
    std::string method_;
    std::string protocol_;
    std::string host_;
    std::string path_;
    std::optional<std::string> content_ = std::nullopt;
    std::map<std::string, std::string> response_headers_;
    std::string body_;
    size_t body_offset_ = 0;
    size_t content_length_ = 0;
    bool eof_ = false;
    bool instance_active_ = false;
    bool request_chunked_ = false;
    bool response_chunked_ = false;
    bool keep_alive_ = false;
    // chunked 且 URC content_len==0 时：最后一包后短空闲视为 EOF（CDN 常省略零长度收尾）
    bool got_content_ = false;
    int last_urc_content_len_ = 0;
    std::chrono::steady_clock::time_point last_content_time_{};
    std::chrono::steady_clock::time_point request_sent_time_{};
    bool request_sent_ = false;
    static constexpr int kChunkedIdleEofMs = 2000;

    bool FetchHeaders();
    void ParseResponseHeaders(const std::string& headers);
    std::string ErrorCodeToString(int error_code);
    bool SendAtChecked(const std::string& command, const char* step, size_t timeout_ms = 1000);
    // ML307 单次 AT+MHTTPCONTENT 不超过 4KB，超长正文需分片。
    bool SendContent(const char* data, size_t size);

    // eof：1 表示后续还有数据，0 表示最后一包。
    static constexpr size_t kMaxContentChunk = 4096;
};

#endif