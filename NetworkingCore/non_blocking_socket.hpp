#pragma once

// this is the header that implements the non-blocking socket

#include <iostream>
#include <sys/event.h>
#include <fcntl.h>
#include <boost/asio.hpp>

struct basic_socket {
    // this is the general context of the whole socket
    boost::asio::io_context context;
    
    boost::asio::ip::tcp target_ip;
    boost::asio::ip::tcp::socket socket;
};

// here is the core part of the non_blocking_socket
struct non_blocking_socket{
private:
    
};
