#pragma once

#include <iostream>
#include <map>
#include <expected>
#include "HttpParser.hpp"

#define universalCode "\r\n"

struct httpresponse{
    int statusCode;
    std::string statusReason;
    std::unordered_map<std::string, std::string> headers; 
    std::string body;
};

std::unordered_map<std::string, int> statusCodeSet{
    {"OK", 200}
};

//MARK: HTTP Serializer
class httpSerializer{
    protected:
    std::vector<std::uint8_t> serializedHTTPRequest;
    HTTPRequest request;

    public:
    httpSerializer(): serializedHTTPRequest({}){}
    ~httpSerializer(){}

    std::expected<void, std::string> Serialize(HTTPParser& universalParser){
        //call the parser to stretch the parsed header and body for the serializer
        auto middleconvertion = universalParser.parse();
        if(middleconvertion.has_value()){
            request = middleconvertion.value();
        }
        else{ return std::unexpected("Parse failed, with null request");}

        //the emplace of the version in the head of the serializer
        serializedHTTPRequest.emplace_back(request.version.begin(), request.version.end());
        serializedHTTPRequest.emplace_back(static_cast<uint8_t>(' '));

        //the emplace of the status code
    }
};
