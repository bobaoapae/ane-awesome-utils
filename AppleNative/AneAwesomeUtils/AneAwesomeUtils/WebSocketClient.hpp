//
//  WebSocketClient.hpp
//  WebSocketANE
//
//  Created by João Vitor Borges on 23/08/24.
//

#ifndef WebSocketClient_hpp
#define WebSocketClient_hpp

#include <queue>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <thread>

class WebSocketClient {
public:
    WebSocketClient(char* guid);

    ~WebSocketClient();

    void connect(const char* uri);
    void close(uint32_t closeCode);
    void sendMessage(uint8_t* bytes, int lenght);
    std::optional<std::vector<uint8_t>> getNextMessage();
    void enqueueMessage(const std::vector<uint8_t>& message);

private:
    std::mutex m_lock_receive_queue;
    std::queue<std::vector<uint8_t>> m_received_message_queue;
    char* m_guidPointer;
};

#endif /* WebSocketClient_hpp */
