
#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <array>

#include "Lunafilament.hpp"

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
    std::size_t parseBuffer, parseCursor = 0;

    std::queue<ResponseFrame> outboundConnection;

    // from here, the functions are the member functions
    void prepareForNextConnection(){ connection_State = ConnState::READING_HEADERS; }

    void read_client_socket(lunarfilament_connection& input_connection){
        std::array<uint8_t, 1024> temporary_Buffer;
        ssize_t bytes_written = recv(input_connection.fileDescriptor, &temporary_Buffer, temporary_Buffer.size(), 0);

        if(bytes_written < 0){
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            input_connection.state = ConnState::CLOSING;
            return;
        }
        else if(bytes_written == 0){
            input_connection.state = ConnState::CLOSING;
            return;
        }

        temporaryInboundBuffer.insert(temporaryInboundBuffer.end(), temporary_Buffer.begin(), temporary_Buffer.begin() + bytes_written);
        timeoutCounter = std::time(nullptr);

    }

    std::expected<void, std::string> process_pipelinedBuffer(lunarfilament_connection& connection){
        while (connection.state == ConnState::READING_HEADERS){
            // as long as the code state is reading headers, we keep processing the buffer
            std::string buffer_view(connection.temporaryBuffer.begin()+parseCursor, connection.temporaryBuffer.end());
            std::size_t buffer_view_findResult = buffer_view.find("\r\n\r\n");

            if(buffer_view_findResult == std::string::npos){ return std::unexpected<std::string>("Invalid HTTP Request");}

            // now you should continue the parsing process
            std::size_t totalRequestedBytes = parseCursor + buffer_view_findResult + 4;
            parseCursor = totalRequestedBytes;

            if(parseCursor >= temporaryInboundBuffer.size()){
                parseCursor = 0;
                temporaryInboundBuffer.clear();
            }

        }
    // the end of this member function    
    }
};

