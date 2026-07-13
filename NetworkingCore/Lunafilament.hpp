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
#include <boost/asio.hpp>

#include "../Subsystems/Aegis.hpp"

namespace asio = boost::asio;

struct lunarfilament_connection{
    int fileDescriptor;
    enum State { READING_HEADERS, WRITING_RESPONSE, CLOSED } state;

    // here is the custom read and the write buffer of the customized connection machine
    // also use the memory allocator to manage the memory efficiently
    std::size_t buffer_read;
    std::size_t buffer_write;

    lunarfilament_connection(int socket_fd) : fileDescriptor(socket_fd), state(READING_HEADERS) {}
};

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

        asio::io_context lunarfilament_io_context;
        asio::posix::stream_descriptor listeningDescriptor;

        // this is used to register a list that which connection is which
        std::unordered_map<int, std::shared_ptr<lunarfilament_connection>> connectionMap;
        


    public:
        lunarfilament(int nativeFileDescriptor): listeningDescriptor(lunarfilament_io_context, nativeFileDescriptor){
            
        }
        ~lunarfilament(){}

    protected:
        std::expected<void, std::string> initializationWithSpecificPort(int16_t SpecificPort){}

        // this is the running function of the context
        std::expected<void, std::string> run(){
            lunarfilament_io_context.run();
        }

        // here is the function that handles the handshake when there is a connection
        std::expected<void, std::string> expect_connection(){}

        // here is the helper method that handles the accept of the connection
        std::expected<void, std::string> connection_acception(){
            int temporary_clientfileDescriptor = accept(listeningDescriptor.native_handle(), nullptr, nullptr);
            if(temporary_clientfileDescriptor < 0){ return std::unexpected<std::string>("Error: File Descriptor"); }

            int temporary_flags = fcntl(temporary_clientfileDescriptor, F_GETFL);
            fcntl(temporary_clientfileDescriptor, F_SETFL, temporary_flags | O_NONBLOCK);

            std::shared_ptr<lunarfilament_connection> temporary_connection = std::make_shared<lunarfilament_connection>();
            connectionMap.insert(connectionMap.end(), {temporary_clientfileDescriptor, temporary_connection});

            // here is something that i do not quite get into it
            // it says that this is the brand new connection acceptor that handles the connection
            std::shared_ptr<asio::posix::stream_descriptor> socket_watcher = 
                std::make_shared<asio::posix::stream_descriptor>(lunarfilament_io_context, temporary_clientfileDescriptor);
            
            // now use the read and the write function to read or write the data into the kernel waitlist
            clientRead(socket_watcher, temporary_connection);
        }

        std::expected<void, std::string> clientRead(std::shared_ptr<asio::posix::stream_descriptor> socketWatcher, 
            std::shared_ptr<lunarfilament_connection> connection){}

        // here is the clean up of the connection to prevent the memory leak
        void fileDescriptorCleanUp(int& fileDescriptor){
            close(fileDescriptor);
            connectionMap.erase(fileDescriptor);
        }

};
