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
    
};
