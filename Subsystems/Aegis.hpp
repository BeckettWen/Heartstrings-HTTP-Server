//welcome to the Aegis memory manager
#pragma once

#include <algorithm>
#include <alloca.h>
#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <array>
#include <tuple>
#include <unordered_map>
#include <vector>

using DefaultChunkOfMemory = std::array<std::byte, 1024*1024>;

namespace Aegis_MemoryManager{

    class Aegis_allocator{
        //use the arena structure first, may switch to a new high efficiency structure later
        friend class lunarfilament;
        //
        private:
            //using the 1 megabytes memory as the allocator's step inside the header file
            std::vector<std::unique_ptr<DefaultChunkOfMemory>> memoryPool;
            std::size_t currentUsablePosition = 0;
            std::size_t memoryBlockNumber, idleMemorySize;

            //this is used to store the allocated memory addresses and its size
            std::vector<std::tuple<std::byte*, std::size_t>> allocatedMemoryAddresses;

            std::vector<std::array<int, 2>> allocationRecorder;
            int currentIndex = 0, previousMemoryBlock = 0;

            //i need to setup one vector that holds the end and the start of each chunk, so that i can know if the
            // memory written process writes to the end of the current memory chunk
            std::unordered_map<std::weak_ptr<std::byte*>, std::weak_ptr<std::byte*>> beginningOfNextChunk;

        public:
            Aegis_allocator(){
                memoryPool.clear();
                memoryBlockNumber = 0;
                idleMemorySize = (memoryPool.capacity() - memoryPool.size()) *1024 *1024;
            }
            ~Aegis_allocator(){
                memoryPool.clear();
            }

        protected:
            //notice that while request the memory, you need to request in bytes
            std::byte* requestForNewMemory(std::size_t requestedSize){
                //allocate the memory
                // i want to fir all the memory tightly into single memory block, but it seems that the work
                // will be put into the second version
                currentIndex += 1;
                previousMemoryBlock = memoryBlockNumber;
                //
                if (idleMemorySize < requestedSize){
                    memoryBlockNumber += (requestedSize - idleMemorySize) / (1024 * 1024) + 1;
                    memoryPool.resize( (requestedSize - idleMemorySize) / (1024*1024) + 1);
                    idleMemorySize = (memoryPool.capacity() - memoryPool.size()) *1024 *1024;
                }
                currentUsablePosition = memoryPool.size() * 1024 * 1024;

                previousMemoryBlock = memoryBlockNumber - previousMemoryBlock;
                allocationRecorder.emplace_back(std::array<int, 2>({currentIndex, previousMemoryBlock}));

                return &((*(memoryPool.at(memoryBlockNumber))).front());
            }

            void releaseMemory(std::vector<int> pendingDeletion){
                int temporaryBlockStorage = 0, temporaryCounter = 0;
                std::sort(pendingDeletion.begin(), pendingDeletion.end());
                for (int item: pendingDeletion){
                    // calculate the current memory block to accurately release the memory
                    for (auto iteminArray: allocationRecorder){
                        if(iteminArray[0] < item){ temporaryBlockStorage += iteminArray[1]; }
                        else{ temporaryCounter = iteminArray[1]; break;}
                        //the end of the if-else switch expression, also finish the calculation of the memory block
                    }

                    //the deletion process
                    while (temporaryCounter > 0){
                        allocationRecorder[item][0] = 0;
                        memoryPool.at(temporaryBlockStorage + temporaryCounter - 1).reset();
                        temporaryCounter -= 1;
                    }
                }
            }


            //use the template function to achieve best flexibility
            template<typename ParameterType>
            std::expected<void, std::string> writeDataToAllocatedMemory(std::byte*& memoryAddress, const void * dataToBeWritten, std::size_t dataSize){
                //this is the example of the iteration that while the input data is the vector form
                const ParameterType *temporaryDataStorage = static_cast<const ParameterType*>(dataToBeWritten);
                if(idleMemorySize < dataSize){ return std::unexpected<std::string>("No Enough Memory"); }

                //here is the actual writing process

            }

            //helper methods
            std::vector<std::array<int, 2>> requestDeletableMemory(){
                for (auto item = allocationRecorder.begin(); item != allocationRecorder.end();){
                    if ((*item)[0] == 0){ allocationRecorder.erase(item); }
                    else{}

                    ++item;
                }

                //shrink the memory pool for the better efficiency of the memory
                memoryPool.shrink_to_fit();

                return allocationRecorder;
            }
    };

}
