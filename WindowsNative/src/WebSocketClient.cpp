#include "AneAwesomeUtilsCsharp.h"
#include "WebSocketClient.h"
#include "log.h"

WebSocketClient::WebSocketClient(const char *guid) {
    writeLog("WebSocketClient created");
    m_guid = std::string(guid); // Use std::string instead of char*
    writeLog(m_guid.c_str());
}

WebSocketClient::~WebSocketClient() = default;

void WebSocketClient::connect(const char *uri) const {
    csharpLibrary_awesomeUtils_connectWebSocket(m_guid.c_str(), uri);
}

void WebSocketClient::close(uint32_t closeCode) const {
    csharpLibrary_awesomeUtils_closeWebSocket(m_guid.c_str(), static_cast<int>(closeCode));
}

void WebSocketClient::sendMessage(uint8_t *bytes, int length) const {
    csharpLibrary_awesomeUtils_sendWebSocketMessage(m_guid.c_str(), bytes, length);
}

std::optional<std::vector<uint8_t> > WebSocketClient::getNextMessage() {
    std::lock_guard guard(m_lock_receive_queue);
    if (m_received_message_queue.empty()) {
        return std::nullopt;
    }

    // Pega a próxima mensagem da fila
    std::vector<uint8_t> message = std::move(m_received_message_queue.front());
    m_received_message_queue.pop();

    return message;
}

void WebSocketClient::enqueueMessage(const std::vector<uint8_t> &message) {
    std::lock_guard guard(m_lock_receive_queue);
    m_received_message_queue.push(message);
}
