#pragma once

#include <iostream>
#include <map>
#include <expected>
#include <string>
#include "HttpParser.hpp"

#define universalCode "\r\n"

struct httpresponse{
    int statusCode;
    std::string statusReason;
    std::unordered_map<std::string, std::string> headers; 
    std::string body;
};

std::unordered_map<HTTPParserError, int> statusCodeSet{
    {HTTPParserError::InvalidContentLength, 400}
};

std::unordered_map<int, std::string> statusCodeToText{
    {400, "Bad Request"},
    {200, "OK"}
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
        std::__hash_map_iterator tempstoreCodeStatus = statusCodeToText.find(200);
        std::string temporaryString = std::string(
                std::to_string((*tempstoreCodeStatus).first)+" "+(*tempstoreCodeStatus).second+universalCode);
        serializedHTTPRequest.insert( serializedHTTPRequest.end() ,temporaryString.begin(), temporaryString.end());
        
        //emmm, is this right? idk, maybe when i add the gtest after i learn how to add gtest
        //and may be tomorrow, who knows?
        std::string temporaryString01, temporaryString02;
        for (auto item: request.headers){
            temporaryString01 = std::get<0>(item);
            temporaryString02 = std::get<1>(item);
            serializedHTTPRequest.insert( serializedHTTPRequest.end(),temporaryString01.begin(), temporaryString02.end());
            serializedHTTPRequest.insert( serializedHTTPRequest.end(), temporaryString02.begin(), temporaryString02.end());
        }
        std::string temporary = std::string(universalCode);
        serializedHTTPRequest.insert( serializedHTTPRequest.end(), temporary.begin(), temporary.end());
        
        //push the body into the serialized http request
        serializedHTTPRequest.insert(serializedHTTPRequest.end(), request.body.begin(), request.body.end());
    }

    //MARK: Helper Method
    std::tuple<int, std::string> findTextWithCode(HTTPParserError error){
        //don't say why not use auto, well, that would be much less fun isn't it
        if(statusCodeSet.contains(error)){
            std::__hash_map_iterator tempStore_Code = statusCodeSet.find(error);
            std::__hash_map_iterator tempStore_text = statusCodeToText.find((*tempStore_Code).second);
            return std::make_tuple((*tempStore_text).first, (*tempStore_text).second);
        }
        else{ return std::make_tuple(0, ""); }
    }
};
