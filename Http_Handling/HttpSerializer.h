#pragma once

#include <iostream>
#include <map>
#include "HttpParser.hpp"

#define universalCode "\r\n"

struct httpresponse{
    uint statusCode;
    std::string statusReason;
    std::unordered_map<std::string, std::string> headers; 
    std::string body;
};

enum HttpStatusCode: uint {};

class httpSerializer{
    public:
    
};
