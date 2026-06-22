
#include <iostream>
#include <expected>
#include "Http_Handling/HttpParser.hpp"
#include "Http_Handling/HttpSerializer.h"

int main() {
    // 1. Create a sample raw HTTP request string (with a body)
    std::string raw_request =
        "POST /api/v1/login HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 27\r\n"
        "\r\n"
        "{\"user\": \"admin\", \"pass\": 123}";

    // 2. Convert the string into raw bytes (Data) for the parser
    Data input_bytes(raw_request.begin(), raw_request.end());

    // 3. Initialize the parser and execute
    HTTPParser parser(input_bytes);
    auto result = parser.parse();

    // 4. Output the results
    std::cout << "--- Parser Output ---" << std::endl;
    if (result.has_value()) {
        const auto& req = result.value();
        std::cout << "Method:  " << req.method << std::endl;
        std::cout << "Target:  " << req.target << std::endl;
        std::cout << "Version: " << req.version << std::endl;
        
        std::cout << "\nHeaders:" << std::endl;
        for (const auto& [key, val] : req.headers) {
            std::cout << "  " << key << ": " << val << std::endl;
        }

        std::string body_as_string(req.body.begin(), req.body.end());
        std::cout << "\nBody:\n" << body_as_string << std::endl;
    } else {
        std::cout << "Parsing Failed with error code: "
                  << static_cast<int>(result.error()) << std::endl;
    }

    return 0;
}
