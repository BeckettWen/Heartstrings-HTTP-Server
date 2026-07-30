//welcome to the Aegis memory manager

// ready to be deprecated and become a single module
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
#include <thread>
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
            // using the 1 megabytes memory as the allocator's step inside the header file
            // these are the core part of the memory allocator
            std::vector<std::unique_ptr<DefaultChunkOfMemory>> memoryPool;
            std::vector<std::tuple<MemoryAddress, std::size_t>> memoryAddresses;

            // use the allocation recorder to find the record that holds the chunk number
            // use the allocated chunk size to find the record that holds the chunks allocated
            // then get to the memory address to get the detailed memory address and the indicator
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


            // i want you to notice something that though the function theoratically accepts the data with every type
            // but still, if you use the general vector type would be much easier
            // and that is the official supported data type when writing examples and do some demonstrations
        public:
            template<typename Datatype>
            std::expected<void, std::string> wirteDataToMemory(std::size_t requestedMemory, const void* data){
                // first you need to get the actual address and the avaliable chunks
                std::unordered_map<std::size_t, std::size_t>::iterator recordFindResult = allocationRecorder.find(requestedMemory);
                if (recordFindResult == allocationRecorder.end()){ return std::unexpected<std::string>("Error: From Memory Record Finding"); }
                else{

                    std::unordered_map<std::size_t, std::size_t>::iterator sizeFindResult = allocatedChunkSize.find(requestedMemory);
                    if(sizeFindResult == allocatedChunkSize.end()){ return std::unexpected<std::string>("Error: From Record Finding Process");}
                    else{

                        // here is the temporary variables that would be used to assist the processing of the 
                        // memory address, so be calm when seeing these variables
                        auto temporaryMemoryAddress = memoryAddresses[requestedMemory];
                        std::size_t ChunkNumberIndicator = std::get<0>(std::get<0>(temporaryMemoryAddress));
                        std::size_t indicatorInsidetheChunk = std::get<1>(std::get<0>(temporaryMemoryAddress));
                        std::size_t avaliableChunks = (*sizeFindResult).second;

                        const Datatype* dataStorage = static_cast<const Datatype*>(data);
                        for(auto item: dataStorage){
                            // here should have the basic process of handling the writting and checking the boundary
                            idleMemorySize = sizeFindResult->second * 1024 * 1024;
                            if(std::size(dataStorage) > idleMemorySize){ 
                                return std::unexpected<std::string>("Error: low memory size");
                                break;
                            }
                            
                            //write the data as the raw bytes
                            if(indicatorInsidetheChunk == 1024*1024 - 1){
                                avaliableChunks -= 1;
                                ChunkNumberIndicator += 1;
                                indicatorInsidetheChunk = 0;
                                (*memoryPool[ChunkNumberIndicator])[indicatorInsidetheChunk] = static_cast<std::byte>(item);
                            }
                            else{
                                indicatorInsidetheChunk += 1;
                                (*memoryPool[ChunkNumberIndicator])[indicatorInsidetheChunk] = static_cast<std::byte>(item);
                            }
                        }


                        delete dataStorage;
                        // this is the end of the writting process
                        return {};
                    // here is the end of the size find result
                    }
                // here is the end of the record find result
                }
            // here is the end of the whole writting function
            }

            public:
            // this is the size retrieve function that retrieve the size of the memory block
            std::expected<std::size_t, std::string> getAllocatedSize(std::size_t& memoryRepresentation){
                std::unordered_map<std::size_t, std::size_t>::iterator findResult;
                findResult = allocatedChunkSize.find(memoryRepresentation);

                if(findResult == allocatedChunkSize.end()){
                    return std::unexpected<std::string>("Error: No record Available");
                }
                else{ return (*findResult).second;}
            }

            std::expected<std::vector<std::byte>, std::string> readData(std::size_t& memoryRepresentation){
                // construct a temporary byte vector
                std::vector<std::byte> temporary_finalResult;
                std::unordered_map<std::size_t, std::size_t>::iterator temp_findResult;
                std::size_t memoryChunkIndicator, howmanychunks;

                // assign the memory chunk indicator with the find result of the memory find process
                temp_findResult = allocationRecorder.find(memoryRepresentation);
                if(temp_findResult == allocationRecorder.end()){ return std::unexpected<std::string>("Error: Memory not found");}
                memoryChunkIndicator = std::get<1>(*temp_findResult);

                // find the allocated chunk size according to the index
                temp_findResult = allocatedChunkSize.find(memoryRepresentation);
                if(temp_findResult == allocatedChunkSize.end()){ return std::unexpected<std::string>("Error: No Eligible Memory");}
                howmanychunks = std::get<1>(*temp_findResult);

                
            }


    //the end bracket of the class Aegis_allocator
    };

    class Aegis_Multithreading{};
//the end bracket of the namespace
}
