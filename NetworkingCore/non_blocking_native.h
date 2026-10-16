#pragma once

// this file is exclusively set to use on macOS, which learns the native system calls like the kqueue and fcntl

#include <iostream>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <expected>
#include <vector>
#include <unistd.h>
#include <system_error>

// these two macros are set to define the behaviour on the socket operations
#define allowed 1
#define denied 0


struct socket_native {
private:
    // this is the socket vector, which stores the whole bunch of sockets
    // when necessary, this vector will be taken over by the system
    std::vector<int>(sockets_vector);
    
    sockaddr_in address{};
    int socket_operation_status = allowed;
    int s_port;
    
    int general_kqueue = kqueue();
    
public:
    // here is the function section of the native socket
    socket_native(int port): s_port(port){}
    ~socket_native(){}
    
    std::expected<void, std::string> set_listening_socket(int& socket_index){
        // here sets the listner socket
        setsockopt(sockets_vector[socket_index], SOL_SOCKET, SO_REUSEADDR, &socket_operation_status, sizeof(allowed));
        
        // here is the pre-bind process
        address.sin_family = AF_INET;
        address.sin_port = htons(s_port);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        
        // bind the socket to the actual port and listen
        int result_bind = bind(sockets_vector[socket_index], reinterpret_cast<sockaddr*>(&address), sizeof(address));
        
        // if any error happens, close the socket in time to prevent further issues
        // and the small mechanism to tell what happens in this process
        if(result_bind == -1){
            std::error_code(error_code)(result_bind, std::generic_category());
            close(sockets_vector[socket_index]);
            std::string error_message = "Failed at bind:" + error_code.message();
            return std::unexpected<std::string>(error_message);
        }
        
        // same goes for the listen process
        int result_listen = listen(sockets_vector[socket_index], SOMAXCONN);
        
        if(result_listen == -1){
            std::error_code(error_code)(result_bind, std::generic_category());
            close(sockets_vector[socket_index]);
            std::string error_message = "Failed at bind:" + error_code.message();
            return std::unexpected<std::string>(error_message);
        }
        
        set_socket_nonblocking(sockets_vector[socket_index]);
        return {};
    }
    
    bool set_socket_nonblocking(int& socket_filedescriptor){
        int flags = fcntl(socket_filedescriptor, F_GETFL);
        return flags != -1 && fcntl(socket_filedescriptor, F_SETFL, flags | O_NONBLOCK) != -1;
    }
};
