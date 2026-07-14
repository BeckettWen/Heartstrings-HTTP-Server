
#pragma once

#include <iostream>
#include <string>

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

