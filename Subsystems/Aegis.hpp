//welcome to the Aegis memory manager

// this version is set to solve the problem of different bugs and optimization
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

#include "VersionInfo.h"

#define Default_Memory_Size (1024*1024)

using DefaultChunkOfMemory = std::array<std::byte, 1024*1024>;
using MemoryAddress = std::tuple<std::size_t, std::size_t>;

namespace Aegis_MemoryManager{

    class Aegis_allocator{
        //use the arena structure first, may switch to a new high efficiency structure later
        friend class lunarfilament;
        public:

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
        struct Memory_Slice {
            std::shared_ptr<Memory_Representation_Unified> Memory_tobe_Sliced;
            std::size_t slice_label;

            // this is the helper variable added to fix the problem of memory compression
            std::array<std::size_t, 2> destination;
            std::size_t byte_count = 0;
        };

        // this holds all of the unified memory address
        std::vector<std::shared_ptr<Memory_Representation_Unified>> memoryAddresses_Optimized;
        std::map<std::size_t, std::size_t> allocationRecorder_Optimized;
        std::map<std::size_t, std::shared_ptr<Memory_Slice>> memory_Fragmentation_table;
        std::size_t Memory_Slice_Allocation_index = 0;

        // here is the struct that needed in the optimize memory API


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
        // optimized memory allocation function
        std::size_t allocateMemory_Optimized(std::size_t requestedSize_memory) {
            // the original process of the allocation to record the memory address and give a unique index
            std::size_t requestedBlockNumber = requestedSize_memory / (1024*1024);
            if(requestedSize_memory % Default_Memory_Size != 0){ requestedBlockNumber += 1;}

            previousChunkNumber = currentAvailableChunkNumber;
            currentAvailableChunkNumber += requestedBlockNumber;

            // now request the acquired memory blocks
            std::generate_n(std::back_inserter(memoryPool), requestedBlockNumber, []() {
                return std::make_unique<DefaultChunkOfMemory>();
            });

            // push the current position and index into the allocation recorder
            memoryAddresses_Optimized.emplace_back(std::make_shared<Memory_Representation_Unified>(Memory_Representation_Unified{
                previousChunkNumber, 0, {previousChunkNumber, 0}, AllocationIndex, requestedBlockNumber
            }));

            
            allocationRecorder_Optimized.insert({AllocationIndex, memoryAddresses_Optimized.size() - 1});
            AllocationIndex += 1;
            return AllocationIndex - 1;
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

        std::expected<void, std::string> Delete_memory_Tellurium(std::size_t& memory_Index) {
            std::map<std::size_t, std::size_t>::iterator request_find_result;
            request_find_result = allocationRecorder_Optimized.find(memory_Index);
            if ( request_find_result == allocationRecorder_Optimized.end()) {
                // check if the request is valid and mis-terminated
                if (request_find_result->first == memory_Index){}
                else{ return std::unexpected<std::string>("Memory Not Found");}
            }

            Memory_Representation_Unified& memory_address_Reference =
                *memoryAddresses_Optimized[request_find_result->second];
            
        }


            // i want you to notice something that though the function theoratically accepts the data with every type
            // but still, if you use the general vector type would be much easier
            // and that is the official supported data type when writing examples and do some demonstrations
        public:

        // here will set the optimized version of the write function
        template<typename Datatype>
        // requires std::is_trivially_copyable_v<Datatype>
        std::expected<void, std::string> writeDataToMemory_Optimized(std::size_t requestedMemory, const Datatype& data) {
                // first search for the allocation record
                std::map<std::size_t, std::size_t>::iterator recordFindResult =
                    allocationRecorder_Optimized.find(requestedMemory);
                if (recordFindResult == allocationRecorder_Optimized.end()) {
                    return std::unexpected<std::string>("Requested Memory Not Found");
                }

                // first calculate the idle memory size
                std::size_t idle_memory_size = (memoryAddresses_Optimized[recordFindResult->second]->size * (1024*1024)) - 
                    ((memoryAddresses_Optimized[recordFindResult->second]->current_index[0] - 
                        memoryAddresses_Optimized[recordFindResult->second]->block_number) * (1024*1024) + 
                        memoryAddresses_Optimized[recordFindResult->second]->current_index[1]);
                

                if constexpr (std::is_same_v<Datatype, const char*>) {
                    if (idle_memory_size < std::strlen(data)) {
                        return std::unexpected<std::string>("No Enough Memory");
                    }
                }
                else {
                    if (idle_memory_size < std::size(data)) {
                        return std::unexpected<std::string>("No Enough Memory");
                    }
                }

                std::size_t temp_block_indicator = memoryAddresses_Optimized[recordFindResult->second]->current_index[0];
                std::size_t temp_withinblock_indicator = memoryAddresses_Optimized[recordFindResult->second]->current_index[1];

                std::byte temporary_data;

                // now writes the data into the memory
                for (auto item: data) {
                    // the core writing mechanism
                    temporary_data = static_cast<std::byte>(item);
                    (*memoryPool[temp_block_indicator])[temp_withinblock_indicator] = temporary_data;

                    temp_withinblock_indicator ++;
                    if (temp_withinblock_indicator == 1024*1024 ){                        
                        temp_block_indicator ++; 
                        temp_withinblock_indicator = 0;
                    }
                }

                // push the indicator to the original indicator to avoid the compression API mis-compressed the data
                memoryAddresses_Optimized[recordFindResult->second]->current_index[0] = temp_block_indicator;
                memoryAddresses_Optimized[recordFindResult->second]->current_index[1] = temp_withinblock_indicator;

                // delete data;
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


        // this is the optimized read Data api
        // still in the Design phase, will publish it in the next version
        // the next version is named "Tellurium"
            std::expected<std::vector<std::byte>, std::string> readData_Optimized(std::size_t& memoryRepresentation) {
                
                std::vector<std::byte> temp_result_optimized;
                // first need to obtain the allocation record
                std::map<std::size_t, std::size_t>::iterator allocationRecord_find_result =
                    allocationRecorder_Optimized.find(memoryRepresentation);
                if (allocationRecord_find_result == allocationRecorder_Optimized.end()) {
                    // return the error message with the readable string format
                    // this needs further investigation cause if the index is
                    // occasionally in the end, then the valid request would be terminated
                    if(allocationRecord_find_result->first != memoryRepresentation){
                        return std::unexpected<std::string>("Requested Memory Missing");
                    }
                    else{
                        std::expected<std::vector<std::byte>, std::string> result = read_Sliced_data_Tellurium(memoryRepresentation);
                        if(!result.has_value()){ return std::unexpected<std::string>(result.error()); }
                        else{ return result.value(); }
                    }
                }

                // now reads the data and put it into the temporary array
                std::size_t find_result_index = allocationRecord_find_result->second;
                std::size_t loop = 0;
                // this line (253) throws the segmentation fault (4th Oct 2026)
                for (loop = 0;loop < memoryAddresses_Optimized[find_result_index]->size;loop++) {
                    if (memoryPool[memoryAddresses_Optimized[find_result_index]->block_number + loop] == nullptr) {
                        continue;
                    }

                    // now after the ensure of the memory exists, finally read the data
                    for (std::byte item: (*memoryPool[memoryAddresses_Optimized[find_result_index]->block_number + loop])) {
                        temp_result_optimized.emplace_back(item);
                    }
                }

                return temp_result_optimized;
            }

            std::expected<std::vector<std::byte>, std::string> read_Sliced_data_Tellurium(std::size_t& index){
                // here should find all the matching record
                std::map<std::size_t, std::shared_ptr<Memory_Slice>> memory_Slice_find_result;
                std::for_each(memory_Fragmentation_table.begin(), memory_Fragmentation_table.end(), [&](
                    const std::pair<std::size_t, std::shared_ptr<Memory_Slice>>& sliced_unit
                ){
                    // if the sliced unit is found, then push it into the result function
                    if(sliced_unit.first == index){ 
                        memory_Slice_find_result.insert(memory_Slice_find_result.end(), {index, sliced_unit.second});
                    }
                });

                if(memory_Slice_find_result.empty()){ return std::unexpected<std::string>("No Matching Index Found\n");}

                // now reads the actual data
                std::vector<std::byte> temp_read_result;
                std::for_each(memory_Slice_find_result.begin(), memory_Slice_find_result.end(), [&](
                    const std::pair<std::size_t, std::shared_ptr<Memory_Slice>>& item_in_memorySlice
                ){
                    // iterate and read the result
                    // well, not optimized, but please 'sit back and relax' :)
                    std::shared_ptr<Memory_Slice> original_memory_slice = item_in_memorySlice.second;
                    std::shared_ptr<Memory_Representation_Unified> original_memory_slice_content = 
                        (*original_memory_slice).Memory_tobe_Sliced;

                    // here use the three variables, one is the block number, second is the size
                    // third is the current index
                    std::size_t current_block_number = (*original_memory_slice).destination[0];
                    std::size_t current_withinblock_number = (*original_memory_slice).destination[1];
                    
                    while (temp_read_result.size() < original_memory_slice->byte_count){
                        // read the actual data into memory
                        temp_read_result.emplace_back((*memoryPool[current_block_number])[current_withinblock_number]);
                        // current_block_number ++;
                        if (++current_withinblock_number == Default_Memory_Size) { current_withinblock_number = 0; ++current_block_number; }
                    }
                });

                return temp_read_result;
            }

        // this is the special memory optimization API
        std::expected<void, std::string> Memory_Compression() {
                // first, locate those blank chunks
                // notice that this is still in the early design phase, and lots of can change
                // well, hope the version Tellurium can make the release in October
                const std::shared_ptr<Memory_Representation_Unified>& Optimization_Object =
                    memoryAddresses_Optimized.back();

                // this is the temporary pointer that is used to fragment that memory
                std::array<std::size_t, 2> memory_read_index = {memoryAddresses_Optimized.back()->block_number, 0};
                std::byte temp_data;
                std::array<std::size_t, 2> memory_provide_space;
                std::shared_ptr<Memory_Slice> temp_storage_memory_slice = std::make_shared<Memory_Slice>();

                // this slices the optimized memory and record that fragmentation
                for (const std::shared_ptr<Memory_Representation_Unified>& item : memoryAddresses_Optimized) {
                    if (item->withinBlock_number == Default_Memory_Size - 1){ continue;}
                    if (item == Optimization_Object || memory_read_index == Optimization_Object->current_index) break;

                    //assign the temporary recorder the recorder of the memory that provides the space
                    memory_provide_space = item->current_index;

                    // add the memory provide space to the destination as the record
                    temp_storage_memory_slice->destination = memory_provide_space;
                    temp_storage_memory_slice->byte_count = 0;

                    // this makes sure the memory is fully used, by looping until the write finished or no memory available
                    while (memory_provide_space[0] < item->block_number + item->size 
                        && memory_read_index != Optimization_Object->current_index) {
                        // read the data out of the memory and write the data back to the idle space
                        ++temp_storage_memory_slice->byte_count;

                        (*memoryPool[memory_provide_space[0]])[memory_provide_space[1]] =
                            (*memoryPool[memory_read_index[0]])[memory_read_index[1]];

                        // now increment the index
                        if (memory_read_index[1] == Default_Memory_Size - 1) {
                            memory_read_index[1] = 0;
                            memory_read_index[0]++;
                        }
                        else{memory_read_index[1] ++;}

                        if (memory_provide_space[1] == Default_Memory_Size - 1) {
                            memory_provide_space[1] = 0;
                            memory_provide_space[0]++;
                        }
                        else{memory_provide_space[1] ++;}
                    }

                    item->current_index = memory_provide_space;

                    temp_storage_memory_slice->slice_label = Memory_Slice_Allocation_index;

                    temp_storage_memory_slice->Memory_tobe_Sliced = memoryAddresses_Optimized.back();
                    memory_Fragmentation_table.insert({Optimization_Object->allocation_index, temp_storage_memory_slice});
                }

                if (memory_read_index != Optimization_Object->current_index) {
                    return std::unexpected<std::string>("Not enough destination space");
                }

                // now release the original memory
                for (int range = 0; range<memoryAddresses_Optimized.back()->size; range++) {
                    memoryPool[memoryAddresses_Optimized.back()->block_number + range].reset();
                }

                return {};
            }

    //the end bracket of the class Aegis_allocator
    };
//the end bracket of the namespace
}
