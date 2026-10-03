//this is the test file of the http handler, include the http parser and serializer
#include <cstdint>
#include <expected>
#include <gtest/gtest.h>
#include <iostream>
#include <ostream>
#include <vector>

#include "../Http_Handling/HttpSerializer.h"
#include "../Http_Handling/HttpParser.hpp"
#include "../Subsystems/Aegis.hpp"
#include "../NetworkingCore/Lunafilament.hpp"
#include "Subsystems/StateMachine.hpp"
#include "Subsystems/ThreadPool.hpp"

TEST(unittests, serializertest){
    //preparation for the test
    std::vector<std::uint8_t> testHttpRequest = {}, finalizedRequest = {}, answer{};
    httpSerializer testserializer;
    std::string rawRequest =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "Hello";
    testHttpRequest.insert(testHttpRequest.end(), rawRequest.begin(), rawRequest.end());
    HTTPParser testparser(testHttpRequest);

    //the test function and the result
    std::string expectedSerialized =
        "HTTP/1.1 200 OK\r\n"
        "content-length: 5\r\n"
        "host: localhost\r\n"
        "\r\n"
        "Hello";
    std::expected<void, std::string> testResult = testserializer.Serialize(testparser);


    // 1. Ensure serialization didn't return an error
    ASSERT_TRUE(testResult.has_value()) << "Serialization failed with error: " << testResult.error();

    auto result = testserializer.ReturnSerializedResult();
    finalizedRequest.insert(finalizedRequest.end(), result.begin(), result.end());
    answer.insert(answer.end(), expectedSerialized.begin(), expectedSerialized.end());

    // 2. Now check the actual output
    EXPECT_EQ(finalizedRequest, answer);
}

TEST(unittests, networking_test){}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
