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
#include <queue>

#include "../Subsystems/Aegis.hpp"
#include "../Http_Handling/HttpParser.hpp"

template <typename T>
constexpr T& as_lvalue(T&& val) noexcept {
    return val; // 'val' has a name, so inside this function it is an lvalue
}

enum ConnState {
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

struct ResponseFrame{
    std::string headers; // pre-serialized headers
    std::string body; // target payload data
    std::size_t bytesWritten = 0;
};

struct lunarfilament_connection: public std::enable_shared_from_this<lunarfilament_connection> {
    boost::asio::posix::stream_descriptor fileDescriptor;
    ConnState state;
    time_t timeoutIndicator;
    bool if_keepalive;

    std::size_t parseBuffer, parseCursor = 0;

    // here is the custom read and the write buffer of the customized connection machine
    // also use the memory allocator to manage the memory efficiently
    std::size_t buffer_read;
    std::size_t buffer_write;

    // this holds the outbound connection
    std::queue<ResponseFrame> outboundConnection;

    std::vector<char> temporaryBuffer;

    boost::system::error_code errorCode;

    lunarfilament_connection(boost::asio::io_context& context ,int socket_fd) :fileDescriptor(context), state(ConnState::READING_HEADERS) {
        fileDescriptor.assign(socket_fd, errorCode);
    }
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
        
        // this is the parser reference that holds the universal parser
        // while this universal parser should be held and managed by the user
        HTTPParser& universalParser;



    public:
        lunarfilament(int nativeFileDescriptor, HTTPParser& userParser)
            : listeningDescriptor(lunarfilament_io_context, nativeFileDescriptor), universalParser(userParser){}
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
                        ssize_t readResult = read((*connection).fileDescriptor.native_handle(), &buffer, buffer.size());

                        if(readResult > 0){
                            // insert the data into the buffer inside the custom connection
                            (*connection).temporaryBuffer.insert((*connection).temporaryBuffer.end(), 
                                buffer.begin(), buffer.end() + readResult);
                            
                            // this is the phase 4 work that need to handle the data

                            // now you should use the reversal function to continue the process
                            clientRead(socketWatcher, connection);
                        }
                        else if(readResult == 0){
                            // the client closed the connection, use the pre-built function to close 
                            // the connection and clean up the file descriptor
                            int temp = (*connection).fileDescriptor.native_handle();
                            fileDescriptorCleanUp(temp);
                        }
                        else{
                            // check if the buffer zone of the system is just temporarily empty
                            // to achieve this, use the file flag to define the operation status
                            if(errno == EAGAIN || errno == EWOULDBLOCK){
                                // the temporary buffer is clear, next step is allowed to be done
                                clientRead(socketWatcher, connection);
                            }
                            else{
                                int temp = (*connection).fileDescriptor.native_handle();
                                fileDescriptorCleanUp(temp);
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

        // from the following is the merge of the response handler
        // to integrate the response handler into one single united class

        // MARK: Merged Functions

        std::expected<void, std::string> process_pipelineHandler(lunarfilament_connection& connection){
            while(connection.state == ConnState::READING_HEADERS){
                // as long as the code state is reading headers, we keep processing the buffer
                std::string buffer_view(connection.temporaryBuffer.begin()+ connection.parseCursor, connection.temporaryBuffer.end());
                std::size_t buffer_view_findResult = buffer_view.find("\r\n\r\n");

                if(buffer_view_findResult == std::string::npos){ return std::unexpected<std::string>("Invalid HTTP Request");}

                // now you should continue the parsing process
                std::size_t totalRequestedBytes = connection.parseCursor + buffer_view_findResult + 4;
                connection.parseCursor = totalRequestedBytes;

                if(connection.parseCursor >= connection.temporaryBuffer.size()){
                    connection.parseCursor = 0;
                    connection.temporaryBuffer.clear();
                }
            }
        } // this bracket is the end of this member function

        // MARK: Member Functions
        std::expected<void, std::string> clientWrite(lunarfilament_connection& connection){

            auto self = connection.shared_from_this();
            
            // check if the outbound connection is empty, if empty, set the 
            // state to the keep alive idle to wait for the incoming connections
            if(connection.outboundConnection.empty()){
                connection.state = ConnState::KEEP_ALIVE_IDLE;
                connection.fileDescriptor.async_wait(boost::asio::posix::stream_descriptor::wait_read,
                [this, self](const boost::system::error_code& error){
                    std::shared_ptr<boost::asio::posix::stream_descriptor> filedescriptor = 
                        std::make_shared<boost::asio::posix::stream_descriptor>(lunarfilament_io_context ,(*self).fileDescriptor.native_handle()); 
                    ;
                    if(!error){ clientRead(std::move(filedescriptor), self); }
                });
                return {};
            }

            // calculate the indicator before the write of the data
            if(connection.outboundConnection.front().bytesWritten > connection.outboundConnection.front().headers.size()){}
            else{}

            // use the temporary buffer to store the data, then write the data using the member function
            // inside the memory allocator
            connection.temporaryBuffer.resize(1024);
            int temp_bytesIndicator = connection.outboundConnection.front().bytesWritten;
            std::string::iterator pointer;
            std::string extractedString;

            // calculate the bytes to be written into the socket
            std::size_t bytes_offset;
            std::size_t remainingBytes;

            // add the control flow to calculate the bytes_offset indicator
            if(connection.outboundConnection.front().bytesWritten < connection.outboundConnection.front().headers.size()){
                bytes_offset = temp_bytesIndicator;
                remainingBytes = connection.outboundConnection.front().headers.size() - bytes_offset;
            }
            else{
                bytes_offset = temp_bytesIndicator - connection.outboundConnection.front().headers.size();
                remainingBytes = connection.outboundConnection.front().body.size() - bytes_offset;
            }

            std::size_t bytes_to_write = std::min(static_cast<int>(remainingBytes), 1024);

            if(temp_bytesIndicator < connection.outboundConnection.front().headers.size()){
                pointer = connection.outboundConnection.front().headers.begin() + temp_bytesIndicator;
                extractedString = std::string(pointer, connection.outboundConnection.front().headers.end());
            }
            else{
                pointer = connection.outboundConnection.front().body.begin() +
                    connection.outboundConnection.front().bytesWritten - connection.outboundConnection.front().headers.size();
                extractedString = std::string(pointer, connection.outboundConnection.front().body.end());
            }

            
            ssize_t writeResult = write(connection.fileDescriptor.native_handle(), extractedString.data(), bytes_to_write);
            
            // here is the loop that continuously write the bytes left
            // and handles the data storage and processing
            
            if(writeResult == -1){ 
                // inspect the error code
                if(errno == EAGAIN || errno == EWOULDBLOCK){
                    connection.fileDescriptor.async_wait(boost::asio::posix::stream_descriptor::wait_write,
                    [this, self](const boost::system::error_code& errorCode){
                        if(!errorCode){ clientWrite((*self)); }
                    });
                }
                else{ fileDescriptorCleanUp(as_lvalue(connection.fileDescriptor.native_handle()));}
            }
            else{

                // fix : only write to the memory after confirm the write process is successful
                memoryAllocator.wirteDataToMemory<std::vector<char>>(connection.buffer_write, extractedString.data());

                
                // move the written bytes cursor forward
                connection.outboundConnection.front().bytesWritten += writeResult;

                // check if the queue is empty
                // if(connection.outboundConnection.empty()){ return {}; }

                // now you should do the evaluation of the frame completion
                if(connection.outboundConnection.front().bytesWritten == 
                connection.outboundConnection.front().headers.size() + connection.outboundConnection.front().body.size()){
                    // pop out the pending, already flushed frame
                    connection.outboundConnection.pop();

                    // empty check 
                    if(connection.outboundConnection.empty()){ 
                        connection.state = ConnState::KEEP_ALIVE_IDLE;
                        connection.fileDescriptor.async_wait(
                            boost::asio::posix::stream_descriptor::wait_read,
                            [this, self](const boost::system::error_code& error) {
                                if (!error) { 
                                    std::shared_ptr<boost::asio::posix::stream_descriptor> socket_watcher = 
                                        std::make_shared<boost::asio::posix::stream_descriptor>(
                                            lunarfilament_io_context, 
                                            self->fileDescriptor.native_handle());
                                    clientRead(socket_watcher, self); 
                                }
                            }
                        );
                        return {}; 
                    }else {
                        connection.fileDescriptor.async_wait(
                        boost::asio::posix::stream_descriptor::wait_write,
                        [this, self](const boost::system::error_code& errorCode) {
                                if (!errorCode) { clientWrite(*self); }
                            }
                        );
                    }
                }
                // in this condition, the indicator must be strictly smaller than the summarized size
                else if (connection.outboundConnection.front().bytesWritten < 
                connection.outboundConnection.front().headers.size() + connection.outboundConnection.front().body.size()){
                    connection.fileDescriptor.async_wait(
                        boost::asio::posix::stream_descriptor::wait_write,
                        [this, self](const boost::system::error_code& errorCode) {
                            if (!errorCode) { clientWrite(*self); }
                        }
                    );
                }
                else{}
            }
            
            return {};
        }// the closure of the bracket


        // MARK: drive State Machine
        std::expected<void, std::string> driveStateMachine(lunarfilament_connection& connection){
            // use a temp vector to store the memory content
            std::expected<std::vector<std::byte>, std::string> temporary_memoryContent = 
                memoryAllocator.readData(connection.buffer_read);
            
            // if the error occurs, finish the function immediately
            if(temporary_memoryContent){ return std::unexpected<std::string>(temporary_memoryContent.error()); }

            // cast the raw bytes into standard string
            std::string temporary_string(
                reinterpret_cast<const char*>(temporary_memoryContent.value().data()),
                temporary_memoryContent.value().size()
            );

            std::string terminator = "\r\n\r\n";
            std::size_t findResult_string = temporary_string.find(terminator);

            std::shared_ptr<lunarfilament_connection> temporaryPointer = connection.shared_from_this();
            if(findResult_string == std::string::npos){
                // leave the state as the reading headers
                // then wait for more socket read notification
                connection.fileDescriptor.async_wait(
                    boost::asio::posix::stream_descriptor::wait_read,
                    [this, temporaryPointer](const boost::system::error_code& error){
                        std::shared_ptr<boost::asio::posix::stream_descriptor> socket_watcher = 
                                        std::make_shared<boost::asio::posix::stream_descriptor>(
                                            lunarfilament_io_context, 
                                            temporaryPointer->fileDescriptor.native_handle());
                        if(!error){clientRead(socket_watcher, temporaryPointer);}
                    }
                );
            }// the closure of the if branch

            
        }// the end of the member function

        std::expected<void, std::string> timeoutGarbageCollector(){}
};
