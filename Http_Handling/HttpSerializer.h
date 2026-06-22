#pragma once

#include <iostream>
#include <map>
#include "HttpParser.hpp"

struct httpresponse{
    uint statusCode;
    std::string statusReason;
    std::unordered_map<std::string, std::string> headers; 
    std::string body;
};

enum HttpStatusCode: uint {};

class HttpSerializer{
    
};
