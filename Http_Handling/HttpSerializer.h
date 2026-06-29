#pragma once

#include <filesystem>
#include <iostream>
#include <map>
#include <expected>
#include <string>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>
#include <sys/socket.h>

#include "HttpParser.hpp"

#define universalCode "\r\n"

struct httpresponse{
    int statusCode;
    std::string statusReason;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

inline std::unordered_map<HTTPParserError, int> statusCodeSet{
    {HTTPParserError::InvalidContentLength, 400}
};

inline std::unordered_map<int, std::string> statusCodeToText{
    {400, "Bad Request"},
    {200, "OK"}
};

//MARK: HTTP Serializer
class httpSerializer{
    protected:
    std::vector<std::uint8_t> serializedHTTPRequest;
    HTTPRequest request;

    public:
    std::vector<std::uint8_t> buffer;

    httpSerializer(): serializedHTTPRequest({}), buffer({}){}
    ~httpSerializer(){}

    std::vector<std::uint8_t> ReturnSerializedResult(){ return serializedHTTPRequest; }

    std::expected<void, std::string> Serialize(HTTPParser& universalParser){
        //you know, i want to embed as much modern cpp as much into my code
        std::string statusCodeMessage;
        int statusCode;

        //call the parser to stretch the parsed header and body for the serializer
        auto middleconvertion = universalParser.parse();
        if(middleconvertion.has_value()){
            request = middleconvertion.value();
        }
        else{
            std::tie(statusCode, statusCodeMessage) = findTextWithCode(middleconvertion.error());
            return std::unexpected(statusCodeMessage);
        }

        //the emplace of the version in the head of the serializer
        serializedHTTPRequest.insert( serializedHTTPRequest.end() ,request.version.begin(), request.version.end());
        serializedHTTPRequest.emplace_back(static_cast<uint8_t>(' '));

        //the emplace of the status code
        auto tempstoreCodeStatus = statusCodeToText.find(200);
        std::string temporaryString = std::string(
                std::to_string((*tempstoreCodeStatus).first)+" "+(*tempstoreCodeStatus).second+universalCode);
        serializedHTTPRequest.insert( serializedHTTPRequest.end() ,temporaryString.begin(), temporaryString.end());

        //emmm, is this right? idk, maybe when i add the gtest after i learn how to add gtest
        //and may be tomorrow, who knows?
        for (const auto& [key, value] : request.headers) {
            // Explicitly add the colon, space, and a newline for EACH header
            std::string headerLine = key + ": " + value + universalCode;

            serializedHTTPRequest.insert(serializedHTTPRequest.end(), headerLine.begin(), headerLine.end());
        }
        std::string temporary = std::string(universalCode);
        serializedHTTPRequest.insert( serializedHTTPRequest.end(), temporary.begin(), temporary.end());

        //push the body into the serialized http request
        serializedHTTPRequest.insert(serializedHTTPRequest.end(), request.body.begin(), request.body.end());

        return {};
    }

    std::expected<void, std::string> writeToBuffer(int& systemSocket){
        int writeStatus = write(systemSocket, serializedHTTPRequest.data(), serializedHTTPRequest.size());
        if(writeStatus == -1){ return std::unexpected<std::string>("write to the socket failed");}
        else{ return {}; }
    }

    //MARK: Helper Method
    std::tuple<int, std::string> findTextWithCode(HTTPParserError error){
        //don't say why not use auto, well, that would be much less fun isn't it
        if(statusCodeSet.contains(error)){
            auto tempStore_Code = statusCodeSet.find(error);
            auto tempStore_text = statusCodeToText.find((*tempStore_Code).second);
            return std::make_tuple((*tempStore_text).first, (*tempStore_text).second);
        }
        else{ return std::make_tuple(0, ""); }
    }
};
