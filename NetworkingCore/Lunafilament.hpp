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


enum State { 
    READING_HEADERS, WRITING_RESPONSE, CLOSED 
};

enum class ConnState {
    READING_HEADERS,
    READING_BODY,
    PROCESSING,
    WRITING_RESPONSES,
    KEEP_ALIVE_IDLE,
    CLOSING
};

enum buffer_type: int{
    type_read, type_write
};

struct lunarfilament_connection{
    int fileDescriptor;
    ConnState state;

    // here is the custom read and the write buffer of the customized connection machine
    // also use the memory allocator to manage the memory efficiently
    std::size_t buffer_read;
    std::size_t buffer_write;

    std::vector<char> temporaryBuffer;

    lunarfilament_connection(int socket_fd) : fileDescriptor(socket_fd), state(ConnState::READING_HEADERS) {}
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

        boost::asio::io_context lunarfilament_io_context;
        boost::asio::posix::stream_descriptor listeningDescriptor;

        // this is used to register a list that which connection is which
        std::unordered_map<int, std::shared_ptr<lunarfilament_connection>> connectionMap;

        // this is the cursor that shows the position of the parsing 
        std::size_t parseCursor_lunarfilament;
        


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

            std::shared_ptr<lunarfilament_connection> temporary_connection;
            connectionMap.insert(connectionMap.end(), {temporary_clientfileDescriptor, temporary_connection});

            // here is something that i do not quite get into it
            // it says that this is the brand new connection acceptor that handles the connection
            std::shared_ptr<boost::asio::posix::stream_descriptor> socket_watcher = 
                std::make_shared<boost::asio::posix::stream_descriptor>(lunarfilament_io_context, temporary_clientfileDescriptor);
            
            // now use the read and the write function to read or write the data into the kernel waitlist
            clientRead(socket_watcher, temporary_connection);
        }

        std::expected<void, std::string> clientRead(std::shared_ptr<boost::asio::posix::stream_descriptor> socketWatcher, 
            std::shared_ptr<lunarfilament_connection> connection){
                // this tells the socket watcher to wake up as the client transfers the data
                (*socketWatcher).async_wait(boost::asio::posix::stream_descriptor::wait_read, 
                    [this, socketWatcher, connection](const boost::system::error_code& errorCode){
                        // here is the error handling of the system error
                        if(errorCode){ return std::unexpected<std::string>("Error: From Client Read"); }

                        // now use the raw read function to read the data from the socket
                        std::array<char, 1024> buffer;
                        ssize_t readResult = read((*connection).fileDescriptor, &buffer, buffer.size());

                        if(readResult > 0){
                            // insert the data into the buffer inside the custom connection
                            (*connection).temporaryBuffer.insert((*connection).temporaryBuffer.end(), 
                                buffer.begin(), buffer.end());
                            
                            // this is the phase 4 work that need to handle the data

                            // now you should use the reversal function to continue the process
                            clientRead(socketWatcher, connection);
                        }
                        else if(readResult == 0){
                            // the client closed the connection, use the pre-built function to close 
                            // the connection and clean up the file descriptor
                            fileDescriptorCleanUp((*connection).fileDescriptor);
                        }
                        else{
                            // check if the buffer zone of the system is just temporarily empty
                            // to achieve this, use the file flag to define the operation status
                            if(errno == EAGAIN || errno == EWOULDBLOCK){
                                // the temporary buffer is clear, next step is allowed to be done
                                clientRead(socketWatcher, connection);
                            }
                            else{
                                fileDescriptorCleanUp((*connection).fileDescriptor);
                            }
                        }
                    });
            }

        // here is the clean up of the connection to prevent the memory leak
        void fileDescriptorCleanUp(int& fileDescriptor){
            close(fileDescriptor);
            connectionMap.erase(fileDescriptor);
        }

        std::expected<std::size_t, std::string> getAllocatorSize(lunarfilament_connection& conn ,buffer_type typeOfBuffer){
            std::expected<std::size_t, std::string> result;
            switch (typeOfBuffer)
            {
            case buffer_type::type_read:
                result = memoryAllocator.getAllocatedSize(conn.buffer_read);
                if(result){ return result.value(); }
                else{ return std::unexpected<std::string>(result.error());  }
                break;
            case buffer_type::type_write:
                result = memoryAllocator.getAllocatedSize(conn.buffer_write);
                if(result){ return result.value(); }
                else{ return std::unexpected<std::string>(result.error());  }
                break;
            default:
                return std::unexpected<std::string>("Error: buffer Not Found");
                break;
            }
        }

};
