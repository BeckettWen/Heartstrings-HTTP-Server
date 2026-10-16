#pragma once

// this is the header that implements the non-blocking socket

#include <iostream>
#include <sys/event.h>
#include <fcntl.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <vector>

struct basic_socket {
    // this is the general context of the whole socket
    boost::asio::io_context context;
    
    boost::asio::ip::tcp target_ip;
    boost::asio::ip::tcp::socket socket;
    
    // this is the universal data container
    std::vector<std::byte> socket_data;
    
    std::expected<void, std::string> set_non_blocking(){
        try {
            // here should use the fcntl function as the native solution,
            // but i want to make two version, one is the cross platform while the other is not
            socket.non_blocking(true);
            return {};
        } catch (std::exception& error) {
            return std::unexpected<std::string>("Failed to set the socket non blocking\n");
        }
    }
};

struct server_socket {
    // this struct is the specific tuned socket for server
    basic_socket socket_server;
    
    // this is the listener vector
    std::vector<boost::asio::ip::tcp::acceptor> listener_list;
};

struct client_socket{
    // this struct is the specific tuned socket for client
    basic_socket socket_client;
};

// here is the core part of the non_blocking_socket
struct non_blocking_socket{
private:
    // this is the general two types of socket, while you can abort any of them
    server_socket s_server;
    client_socket c_server;
    
    
};
