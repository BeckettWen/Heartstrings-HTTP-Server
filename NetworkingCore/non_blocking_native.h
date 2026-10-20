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
#include <unordered_map>

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
    
    // this is the changelist used in the kevent
    struct kevent changelist;
    
    // here will add a flag to differentiate the listener socket and the client socket
    // true is the client socket, while the false is the listener socket
    std::unordered_map<int, bool>(socket_type_table);
    
    // this is the vector that holds the kevent bytes, which is also a vector
    std::vector<std::array<struct kevent, 64>>(kevent_vector);
    
public:
    // here is the function section of the native socket
    socket_native(int port): s_port(port){
        // initialize the vector with one empty vector to prevent the segmentation fault
        kevent_vector.resize(1);
    }
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
    
    // here creates the non-blocking socket with kqueue
    std::expected<void, std::string> set_non_blocking(int& index){
        // fill the registration with the evset function
        EV_SET(&changelist, sockets_vector[index], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        
        // set the nonblocking with kevent
        const int result_non_blocking = kevent(general_kqueue, &changelist, 1, nullptr, 0, nullptr);
        if (result_non_blocking == -1) {
            // here also use the same mechanism to collect the error and output it
            std::error_code(error_code)(result_non_blocking, std::generic_category());
            close(sockets_vector[index]);
            std::string error_message = "Failed at set_non_blocking:" + error_code.message();
            return std::unexpected<std::string>(error_message);
        }
        return {};
    }
    
    std::expected<void, std::string> listen_and_handle(int& index_socket){
        // now is the actual listening process
        // check if there is any active event, if not, then the kevent should wait
        int event_count = kevent(general_kqueue, nullptr, 0, kevent_vector[index_socket].data(), 64, nullptr);
        
        // now if the error or anything interrupted the kevent, then the event_count variable should be -1
        // which in this case, the signal interruption should not be counted as the error
        
        // use the same mechanism, :) should make it a function in the later process, but not what i would
        // do in the current constructing process
        if(event_count == -1){
            if (errno == EINTR) {};  // A signal interrupted the wait.
            // here also use the same mechanism to collect the error and output it
            std::error_code(error_code)(event_count, std::generic_category());
            close(sockets_vector[index_socket]);
            std::string error_message = "Failed at kevent:" + error_code.message();
            return std::unexpected<std::string>(error_message);
        }
        
        // now actual process all the events
        for(int event_count_process_loop = 0; event_count_process_loop < event_count; event_count_process_loop ++){
            
            // here stripes out the file descriptor from the event vector
            int file_descriptor = static_cast<int>(kevent_vector[index_socket][event_count_process_loop].ident);
            
            // now handles the logic of if its time to read or time to write
            if(kevent_vector[index_socket][event_count_process_loop].filter == EVFILT_WRITE){
                // executes the write function, which will be implemented the next day
            }
            else if(kevent_vector[index_socket][event_count_process_loop].filter == EVFILT_READ){
                // executes the read function, also, next day
            }
        }
        
        return {};
    }
    
    bool set_socket_nonblocking(int& socket_filedescriptor){
        int flags = fcntl(socket_filedescriptor, F_GETFL);
        return flags != -1 && fcntl(socket_filedescriptor, F_SETFL, flags | O_NONBLOCK) != -1;
    }
};
