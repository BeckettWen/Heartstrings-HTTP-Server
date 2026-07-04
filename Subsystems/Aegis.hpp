//welcome to the Aegis memory manager
#pragma once

#include <algorithm>
#include <alloca.h>
#include <array>
#include <cstddef>
#include <expected>
#include <iterator>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <array>
#include <tuple>
#include <unordered_map>
#include <vector>

using DefaultChunkOfMemory = std::array<std::byte, 1024*1024>;
using MemoryAddress = std::tuple<std::size_t, std::size_t>;

namespace Aegis_MemoryManager{

    class Aegis_allocator{
        //use the arena structure first, may switch to a new high efficiency structure later
        friend class lunarfilament;
        public:
            std::string version = "Version 1 Update 1";

        private:
            //using the 1 megabytes memory as the allocator's step inside the header file
            // these are the core part of the memory allocator
            std::vector<std::unique_ptr<DefaultChunkOfMemory>> memoryPool;
            std::vector<MemoryAddress> memoryAddresses;
            std::unordered_map<MemoryAddress, std::size_t> allocatedChunkSize;

            std::size_t memoryBlockNumber_InsideChunk, idleMemorySize, currentAvaliableChunkNumber, previousChunkNumber;
            MemoryAddress temporaryAddress;


        public:
            Aegis_allocator(): currentAvaliableChunkNumber(0), previousChunkNumber(0){
                memoryPool.clear();
                memoryAddresses.clear();
                allocatedChunkSize.clear();
            }
            ~Aegis_allocator(){
                memoryPool.clear();
            }

        protected:
            std::size_t allocateMemory(std::size_t requestedSize){
                previousChunkNumber = currentAvaliableChunkNumber;
                currentAvaliableChunkNumber += requestedSize / (1024*1024) + 1;
                memoryPool.reserve(memoryPool.size() + currentAvaliableChunkNumber - previousChunkNumber);
                std::generate_n(std::back_inserter(memoryPool), currentAvaliableChunkNumber - previousChunkNumber, []() {
                    return std::make_unique<DefaultChunkOfMemory>();
                });

                // get the memory address, aka the special index
                temporaryAddress = std::make_tuple(currentAvaliableChunkNumber, 0);
                memoryAddresses.emplace_back(temporaryAddress);

                allocatedChunkSize.insert(allocatedChunkSize.end(), {memoryAddresses.back(), requestedSize / (1024*1024) + 1});
                return memoryAddresses.size();
            }

            std::expected<void, std::string> DeleteMemory(std::size_t requestedDeletion){

            }
    };

}
