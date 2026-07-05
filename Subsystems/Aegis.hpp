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
            std::vector<std::tuple<MemoryAddress, std::size_t>> memoryAddresses;
            std::unordered_map<std::size_t, std::size_t> allocatedChunkSize, allocationRecorder;

            std::size_t memoryBlockNumber_InsideChunk, idleMemorySize, currentAvaliableChunkNumber, previousChunkNumber;
            MemoryAddress temporaryAddress;

            std::size_t AllocationIndex = 0;


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
            //memory allocation, return the index to achieve maximize simplicity
            std::size_t allocateMemory(std::size_t requestedSize){
                previousChunkNumber = currentAvaliableChunkNumber;
                currentAvaliableChunkNumber += requestedSize / (1024*1024) + 1;
                memoryPool.reserve(memoryPool.size() + currentAvaliableChunkNumber - previousChunkNumber);
                std::generate_n(std::back_inserter(memoryPool), currentAvaliableChunkNumber - previousChunkNumber, []() {
                    return std::make_unique<DefaultChunkOfMemory>();
                });

                // get the memory address, aka the special index
                AllocationIndex ++;
                temporaryAddress = std::make_tuple(currentAvaliableChunkNumber, 0);
                memoryAddresses.insert(memoryAddresses.end(), std::make_tuple(temporaryAddress, AllocationIndex));

                allocationRecorder.insert(allocationRecorder.end(), {AllocationIndex, currentAvaliableChunkNumber});

                allocatedChunkSize.insert(allocatedChunkSize.end(), {AllocationIndex, requestedSize / (1024*1024) + 1});
                return memoryAddresses.size();
            }

            std::expected<void, std::string> DeleteMemory(std::size_t requestedDeletion){
                //use the passed in request index to find the address and the step
                std::unordered_map<std::size_t, std::size_t>::iterator findResult = allocationRecorder.find(requestedDeletion);
                if(findResult != allocationRecorder.end()){
                    std::unordered_map<std::size_t, std::size_t>::iterator sizeFindResult = allocatedChunkSize.find(findResult->second);
                    if(sizeFindResult == allocatedChunkSize.end()){ return std::unexpected<std::string>("Chunk Size Unavailable");}
                    else{
                        // now is the main process of the deletion process
                        // btw, it will be so much less fun if i use the auto keyword doesn't it
                        int loop_DeletionProcess = findResult->first;
                        while (loop_DeletionProcess < sizeFindResult->second + findResult->first) {
                            memoryPool[loop_DeletionProcess].reset();
                            loop_DeletionProcess ++ ;
                        }
                        return {};
                    }
                }
                else{ return std::unexpected<std::string>("Requested area doesn't exist"); }
            }

            std::expected<void, std::string> wirteDataToMemory(std::size_t requestedMemory){}


    //the end bracket of the class
    };
//the end bracket of the namespace
}
