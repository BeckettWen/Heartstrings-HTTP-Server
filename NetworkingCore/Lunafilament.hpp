#pragma once

#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <expected>
#include <fcntl.h>

class lunarfilament{
    // this is the network core of the http server
    // which handles all the send and receive of the data
    // and for the name .... Pragmata! Diana is just soooo adooorable
    private:
        int defaultPort = 8080;
        //here should be some necessary variables that will be needed in the later process
        sockaddr_in universalAddress;


    public:
        lunarfilament(){}
        ~lunarfilament(){}

    protected:
        std::expected<void, std::string> createSocketWithSpecificPort(){}

};
