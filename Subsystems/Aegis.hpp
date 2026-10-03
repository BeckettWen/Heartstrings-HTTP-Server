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
#include <map>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#define Default_Memory_Size 1024*1024

using DefaultChunkOfMemory = std::array<std::byte, 1024*1024>;
using MemoryAddress = std::tuple<std::size_t, std::size_t>;

namespace Aegis_MemoryManager{

    class Aegis_allocator{
        //use the arena structure first, may switch to a new high efficiency structure later
        friend class lunarfilament;
        public:
        std::string major_version = "Version Calcium";
        std::string minor_version = "10240.38";

        private:
        // using the 1 megabytes memory as the allocator's step inside the header file
        // these are the core part of the memory allocator
        // which is generic for both before-optimized and after-optimized
        std::vector<std::unique_ptr<DefaultChunkOfMemory>> memoryPool;

        // this is the memory address before the optimization, not generic
        std::vector<std::tuple<MemoryAddress, std::size_t>> memoryAddresses;

        // use the allocation recorder to find the record that holds the chunk number
        // use the allocated chunk size to find the record that holds the chunks allocated
        // then get to the memory address to get the detailed memory address and the indicator
        std::unordered_map<std::size_t, std::size_t> allocatedChunkSize, allocationRecorder;

        std::size_t memoryBlockNumber_InsideChunk, idleMemorySize, currentAvailableChunkNumber, previousChunkNumber;
        MemoryAddress temporaryAddress;

        std::size_t AllocationIndex = 0;

        // code upon is the original code
        // code below is the optimized code
        struct Memory_Representation_Unified {
            std::size_t block_number;
            std::size_t withinBlock_number;
            std::array<std::size_t, 2> current_index;
            std::size_t allocation_index;
            std::size_t size;
        };

        // this holds all of the unified memory address
        std::vector<std::unique_ptr<Memory_Representation_Unified>> memoryAddresses_Optimized;
        std::map<std::size_t, std::size_t> allocationRecorder_Optimized;

        public:
            Aegis_allocator(): currentAvailableChunkNumber(0), previousChunkNumber(0){
                memoryPool.clear();
                memoryAddresses.clear();
                allocatedChunkSize.clear();
            }
            ~Aegis_allocator(){
                memoryPool.clear();
            }

        public:
        //memory allocation, return the index to achieve maximize simplicity
        std::size_t allocateMemory(std::size_t requestedSize){
                previousChunkNumber = currentAvailableChunkNumber;
                currentAvailableChunkNumber += requestedSize / (1024*1024) + 1;
                memoryPool.reserve(memoryPool.size() + currentAvailableChunkNumber - previousChunkNumber);
                std::generate_n(std::back_inserter(memoryPool), currentAvailableChunkNumber - previousChunkNumber, []() {
                    return std::make_unique<DefaultChunkOfMemory>();
                });

                // get the memory address, aka the special index
                AllocationIndex ++;
                temporaryAddress = std::make_tuple(currentAvailableChunkNumber, 0);
                memoryAddresses.insert(memoryAddresses.end(), std::make_tuple(temporaryAddress, AllocationIndex));

                allocationRecorder.insert(allocationRecorder.end(), {AllocationIndex, currentAvailableChunkNumber});

                allocatedChunkSize.insert(allocatedChunkSize.end(), {AllocationIndex, requestedSize / (1024*1024) + 1});
                return memoryAddresses.size();
            }

        // optimized memory allocation function
        std::size_t allocateMemory_Optimized(std::size_t requestedSize_memory) {
            // the original process of the allocation to record the memory address and give a unique index
            std::size_t requestedBlockNumber = requestedSize_memory / (1024*1024);
            previousChunkNumber = currentAvailableChunkNumber;
            currentAvailableChunkNumber += requestedBlockNumber + 1;

            // now request the acquired memory blocks
            std::generate_n(std::back_inserter(memoryPool), requestedBlockNumber, []() {
                return std::make_unique<DefaultChunkOfMemory>();
            });

            // push the current position and index into the allocation recorder
            memoryAddresses_Optimized.reserve(memoryAddresses_Optimized.size() + 1);
            memoryAddresses_Optimized.back() = std::make_unique<Memory_Representation_Unified>(Memory_Representation_Unified{
                previousChunkNumber, 0, {0, 0}, AllocationIndex, requestedBlockNumber
            });

            allocationRecorder_Optimized.insert(allocationRecorder_Optimized.end(), {AllocationIndex, memoryAddresses_Optimized.size() - 1});
            return AllocationIndex;
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

        // here will set the optimized version of the write function
        template<typename Datatype>
        std::expected<void, std::string> writeDataToMemory_Optimized(std::size_t requestedMemory, const void* data) {
                // first search for the allocation record
                std::map<std::size_t, std::size_t>::iterator recordFindResult =
                    allocationRecorder_Optimized.find(requestedMemory);
                if (recordFindResult == allocationRecorder_Optimized.end()) {
                    return std::unexpected<std::string>("Requested Memory Not Found");
                }

                const Datatype* data_converted = static_cast<const Datatype*>(data);

                // first calculate the idle memory size
                std::size_t idle_memory_size =
                    memoryAddresses_Optimized[recordFindResult->second]->size -
                        (memoryAddresses_Optimized[recordFindResult->second]->current_index[0]
                - memoryAddresses_Optimized[recordFindResult->second]->block_number - 1) * Default_Memory_Size
                - memoryAddresses_Optimized[recordFindResult->second]->current_index[1] - 1;

                if (idle_memory_size < std::size(data_converted)) {
                    return std::unexpected<std::string>("No Enough Memory");
                }

                std::size_t temp_block_indicator = memoryAddresses_Optimized[recordFindResult->second]->current_index[0];
                std::size_t temp_withinblock_indicator = memoryAddresses_Optimized[recordFindResult->second]->current_index[1];

                std::byte temporary_data;

                // now writes the data into the memory
                for (auto item: data_converted) {
                    // the core writing mechanism
                    temporary_data = static_cast<std::byte>(item);
                    (*memoryPool[temp_block_indicator])[temp_withinblock_indicator] = temporary_data;

                    if (temp_withinblock_indicator == 1024*1024){ temp_block_indicator ++; temp_withinblock_indicator = 0;}
                    else{ temp_withinblock_indicator ++;}
                }

                delete data_converted;
                return {};
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

        // this is the optimized read Data api
        std::expected<std::vector<std::byte>, std::string> readData_Optimized(std::size_t& memoryRepresentation) {

            }


    //the end bracket of the class Aegis_allocator
    };
//the end bracket of the namespace
}
