#pragma once

#include <cstddef>
#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <expected>
#include <fcntl.h>
#include <asio.hpp>
#include <tuple>
#include <vector>

#include "../Subsystems/Aegis.hpp"

class lunarfilament{
    // this is the network core of the http server
    // which handles all the send and receive of the data
    // and for the name .... Pragmata! Diana is just soooo adooorable
    private:
        int defaultPort = 50000;
        //here should be some necessary variables that will be needed in the later process
        sockaddr_in universalAddress;
        //use the memoryManager to manage the memory, avoid the memory leak and better, easier memory allocation
        Aegis_MemoryManager::Aegis_allocator memoryAllocator;
        int fileDescriptor;
        


    public:
        lunarfilament(){}
        ~lunarfilament(){}

    protected:
        std::expected<void, std::string> initializationWithSpecificPort(int16_t SpecificPort){}

};
