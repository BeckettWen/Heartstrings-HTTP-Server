//welcome to the Aegis memory manager
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sys/socket.h>
#include <array>
#include <vector>

namespace NetworkingMemory {

    class networkingMemoryManager{
        //use the arena structure first, may switch to a new high efficiency structure later
        private:
            //using the 1 megabytes memory as the allocator's step
            using DefaultChunkOfMemory = std::array<std::byte, 1024*1024>;
            std::vector<std::unique_ptr<DefaultChunkOfMemory>> memoryPool;

        protected:
            networkingMemoryManager(){
                memoryPool.clear();
            }
            ~networkingMemoryManager(){}


    };

}
