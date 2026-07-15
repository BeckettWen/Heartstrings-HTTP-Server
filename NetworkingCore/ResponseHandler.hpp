
#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <array>

#include "Lunafilament.hpp"

enum class ConnState {
    READING_HEADERS,
    READING_BODY,
    PROCESSING,
    WRITING_RESPONSES,
    KEEP_ALIVE_IDLE,
    CLOSING
};

struct ResponseFrame{
    std::string headers; // pre-serialized headers
    std::string body; // target payload data
    std::size_t bytesWritten = 0;
};

struct connection_Response{
    int fileDescriptor;
    ConnState connection_State;
    std::time_t timeoutCounter;
    bool if_keepAlive;

    std::vector<uint8_t> temporaryInboundBuffer;
    std::size_t parseBuffer;

    std::queue<ResponseFrame> outboundConnection;

    // from here, the functions are the member functions
    void prepareForNextConnection(){ connection_State = ConnState::READING_HEADERS; }

    void read_client_socket(lunarfilament_connection& input_connection){
        std::array<uint8_t, 1024> temporary_Buffer;
        ssize_t bytes_written = recv(input_connection.fileDescriptor, &temporary_Buffer, temporary_Buffer.size(), 0);

        
    }
};

