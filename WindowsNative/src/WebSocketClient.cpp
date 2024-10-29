#include "AneAwesomeUtilsCsharp.h"
#include "WebSocketClient.h"
#include "log.h"

WebSocketClient::WebSocketClient(char* guid) {
    writeLog("WebSocketClient created");
    m_guidPointer = guid;
    writeLog(m_guidPointer);
}

WebSocketClient::~WebSocketClient() = default;

void WebSocketClient::connect(const char* uri) {
    csharpLibrary_awesomeUtils_connectWebSocket(m_guidPointer, uri);
}

void WebSocketClient::close(uint32_t closeCode) {
    csharpLibrary_awesomeUtils_closeWebSocket(m_guidPointer, static_cast<int>(closeCode));
}

void WebSocketClient::sendMessage(uint8_t* bytes, int lenght) {
    csharpLibrary_awesomeUtils_sendWebSocketMessage(m_guidPointer, bytes, lenght);
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
