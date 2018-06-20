#ifndef _DYNAMICPOOLALLOCATOR_HPP
#define _DYNAMICPOOLALLOCATOR_HPP

#include <cstddef>
#include <cassert>
#include <string>
#include <iostream>
#include <sstream>

#include "umpire/tpl/simpool/StdAllocator.hpp"
#include "umpire/tpl/simpool/FixedPoolAllocator.hpp"
#include "umpire/strategy/AllocationStrategy.hpp"
#include "umpire/util/Macros.hpp"

#define MARTY_DEBUGGING
#ifdef MARTY_DEBUGGING
#define MYDEBUG(msg)  {\
    std::stringstream _ss;\
    _ss << __PRETTY_FUNCTION__ << " " << __LINE__ << " " << msg;\
    dumpBlocks(_ss);\
}
#else
#define MYDEBUG(msg)
#endif

template <class IA = StdAllocator>
class DynamicPoolAllocator
{
protected:
  struct Block
  {
    char *data;
    std::size_t size;
    Block *next;
  };

  // Allocator for the underlying data
  typedef FixedPoolAllocator<struct Block, IA, (1<<6)> BlockAlloc;
  BlockAlloc blockAllocator;

  // Start of the nodes of used and free block lists
  struct Block *usedBlocks;
  struct Block *freeBlocks;

  // List of original allocations from resource sorted by starting address
  struct Block *originalAllocations;

  // Total size allocated (bytes)
  std::size_t totalBytes;

  // Allocated size (bytes)
  std::size_t allocBytes;

  // Minimum size for allocations
  std::size_t minBytes;

  std::size_t highestFreeBlockCount;

  // Pointer to our allocator's allocation strategy
  std::shared_ptr<umpire::strategy::AllocationStrategy> allocator;

  void dumpBlocks(const std::stringstream& header) {
    if ( freeBlocks == NULL ) {
      std::cerr 
        << header.str()
        << "\t" << " Free Blocks: EMPTY\n";
    }
    int i = 1;
    for ( struct Block *iter = freeBlocks ; iter ; iter = iter->next ) {
      std::cerr 
        << header.str()
        << "\t" << " Free Blocks: #" << i << " - " << std::hex << iter->size << ": " 
        << (void*)(&iter->data[0]) << " -- " << (void*)(&iter->data[iter->size]) << "\n";
      ++i;
    }

    if ( usedBlocks == NULL ) {
      std::cerr 
        << header.str()
        << "\t" << " Used Blocks: EMPTY\n";
    }
    i = 1;
    for ( struct Block *iter = usedBlocks ; iter ; iter = iter->next ) {
      std::cerr 
        << header.str()
        << "\t" << " Used Blocks: #" << i << " - " << std::hex << iter->size << ": " 
        << (void*)(&iter->data[0]) << " -- " << (void*)(&iter->data[iter->size]) << "\n";
      ++i;
    }

    if ( originalAllocations == NULL ) {
      std::cerr 
        << header.str()
        << "\t" << " Orig Blocks: EMPTY\n";
    }
    i = 1;
    for ( struct Block *iter = originalAllocations ; iter ; iter = iter->next ) {
      std::cerr 
        << header.str()
        << "\t" << " Orig Blocks: #" << i << " - " << std::hex << iter->size << ": " 
        << (void*)(&iter->data[0]) << " -- " << (void*)(&iter->data[iter->size]) << "\n";
      ++i;
    }
  }

  // Search the list of free blocks and return a usable one if that exists, else NULL
  void findUsableBlock(struct Block *&best, struct Block *&prev, std::size_t size) {
    best = prev = NULL;
    for ( struct Block *iter = freeBlocks, *iterPrev = NULL ; iter ; iter = iter->next ) {
      if ( iter->size >= size && (!best || iter->size < best->size) ) {
        best = iter;
        prev = iterPrev;
      }
      iterPrev = iter;
    }
  }

  inline std::size_t alignmentAdjust(const std::size_t size) {
    const std::size_t AlignmentBoundary = 16;
    return std::size_t (size + (AlignmentBoundary-1)) & ~(AlignmentBoundary-1);
  }

  // Allocate a new block and add it to the list of free blocks
  void allocateBlock(struct Block *&curr, struct Block *&prev, const std::size_t size) {
    const std::size_t sizeToAlloc = std::max(alignmentAdjust(size), minBytes);
    curr = prev = NULL;
    void *data = NULL;

    // Allocate data
    try {
      data = allocator->allocate(sizeToAlloc);
    }
    catch (...) {
      freeOriginalAllocationBlocks();
      data = allocator->allocate(sizeToAlloc);
    }

    totalBytes += sizeToAlloc;
    UMPIRE_ASSERT("Memory Allocation Failed" && data);

    // Allocate block for freeBlocks
    curr = (struct Block *) blockAllocator.allocate();
    assert("Failed to allocate block for freeBlock List" && curr);

    // Allocate block for original allocation
    struct Block *orig;
    orig = (struct Block *) blockAllocator.allocate();
    assert("Failed to allocate block for originalAllocations List" && orig);

    // Find next and prev such that next->data is still smaller than data (keep ordered)
    struct Block *next;
    for ( next = freeBlocks; next && next->data < data; next = next->next ) {
      prev = next;
    }
    // Insert
    curr->data = static_cast<char *>(data);
    curr->size = sizeToAlloc;
    curr->next = next;
    if (prev) prev->next = curr;
    else freeBlocks = curr;

    // Find next and prev such that next->data is still smaller than data (keep ordered)
    struct Block* orig_prev = NULL;
    for ( next = originalAllocations; next && next->data < data; next = next->next ) {
      orig_prev = next;
    }

    // Insert
    orig->data = static_cast<char *>(data);
    orig->size = sizeToAlloc;
    orig->next = next;
    if (orig_prev) orig_prev->next = orig;
    else originalAllocations = orig;
  }

  void splitBlock(struct Block *&curr, struct Block *&prev, const std::size_t size) {
    struct Block *next;
    const std::size_t alignedsize = alignmentAdjust(size);

    if ( curr->size == size || curr->size == alignedsize ) {
      // Keep it
      next = curr->next;
    }
    else {
      // Split the block
      std::size_t remaining = curr->size - alignedsize;
      struct Block *newBlock = (struct Block *) blockAllocator.allocate();
      if (!newBlock) return;
      newBlock->data = curr->data + alignedsize;
      newBlock->size = remaining;
      newBlock->next = curr->next;
      next = newBlock;
      curr->size = alignedsize;
    }

    if (prev) prev->next = next;
    else freeBlocks = next;
  }

  void releaseBlock(struct Block *curr, struct Block *prev) {
#if 0
    struct Block* loc_curr = curr;
    size_t size = curr->size;
#endif
    assert(curr != NULL);

    if (prev) prev->next = curr->next;
    else usedBlocks = curr->next;

    // Find location to put this block in the freeBlocks list
    prev = NULL;
    for ( struct Block *temp = freeBlocks ; temp && temp->data < curr->data ; temp = temp->next ) {
      prev = temp;
    }

    // Keep track of the successor
    struct Block *next = prev ? prev->next : freeBlocks;

    // Check if prev and curr can be merged
    if ( prev && prev->data + prev->size == curr->data ) {
      prev->size = prev->size + curr->size;
      blockAllocator.deallocate(curr); // keep data
      curr = prev;
    }
    else if (prev) {
      prev->next = curr;
    }
    else {
      freeBlocks = curr;
    }

    // Check if curr and next can be merged
    if ( next && curr->data + curr->size == next->data ) {
      curr->size = curr->size + next->size;
      curr->next = next->next;
      blockAllocator.deallocate(next); // keep data
    }
    else {
      curr->next = next;
    }

    //MYDEBUG("RETURN");
  }

  void freeOriginalAllocationBlocks() {
    struct Block* fb = freeBlocks;
    struct Block* fbprev = NULL;
    struct Block* orig_prev = NULL;

    MYDEBUG("ENTER ");
    for ( struct Block* orig = originalAllocations ; orig && fb; ) {
      char* orig_edata = orig->data + orig->size;
      char* fb_edata = fb->data + fb->size;

      while (fb && fb_edata < orig_edata) {
        fbprev = fb;
        fb = fb->next;
      }

      if ( fb && fb->data <= orig->data && fb_edata >= orig_edata ) {
        // We found something we can free, now we need to carve it out of the free list

        // Carve off lower fragment
        if (fb->data < orig->data) {
          MYDEBUG(" STRANGENESS ");
          assert(0);

          struct Block* newBlock = (struct Block*)blockAllocator.allocate();
          UMPIRE_ASSERT("Failed to allocate split block during resource reclaim" && newBlock);
          newBlock->data = fb->data;
          newBlock->size = orig->data - fb->data;
          newBlock->next = fb;
          fb->data = orig->data;
          fb->size -= newBlock->size;

          if ( fbprev ) 
            fbprev->next = newBlock;
          else
            freeBlocks = newBlock;
          fbprev = newBlock;
        }

        // at this point, fb->data == orig->data.  Free back this portion
        UMPIRE_ASSERT("Pointer Manipulation Error" && (fb->data == orig->data));
        allocator->deallocate(orig->data);
        totalBytes -= orig->size;
        fb->size -= orig->size;
        fb->data += orig->size;

        if ( fb->size == 0 ) {
          struct Block* tempBlock = fb->next;
          blockAllocator.deallocate(fb);

          if ( fbprev ) 
            fbprev->next = tempBlock;
          else
            freeBlocks = tempBlock;
          fb = tempBlock;
        }
        // Resume at beggining of upper fragment

        if ( orig_prev )
          orig_prev->next = orig->next;
        else
          originalAllocations = orig->next;

        struct Block* tempBlock = orig->next;
        blockAllocator.deallocate(orig);
        orig = tempBlock;
        //MYDEBUG(" SHOULD HAVE FREED SOMETHING ");
      }
      else {
        orig_prev = orig;
        orig = orig->next;
      }
    }

    MYDEBUG(" EXIT ");
  }

  void freeReleasedBlocks() {
    // Release the unused blocks
    while(originalAllocations) {
      allocator->deallocate(originalAllocations->data);
      totalBytes -= originalAllocations->size;
      struct Block *curr = originalAllocations;
      originalAllocations = originalAllocations->next;
      blockAllocator.deallocate(curr);
    }
    freeBlocks = NULL;
  }

  void freeAllBlocks() {
    // Release the used blocks
    while(usedBlocks) {
      releaseBlock(usedBlocks, NULL);
    }

    freeReleasedBlocks();
  }

public:
  DynamicPoolAllocator(
      std::shared_ptr<umpire::strategy::AllocationStrategy> strat,
      const std::size_t _minBytes = (1 << 8)
      )
    : blockAllocator(),
      usedBlocks(NULL),
      freeBlocks(NULL),
      originalAllocations(NULL),
      totalBytes(0),
      allocBytes(0),
      minBytes(_minBytes),
      highestFreeBlockCount(0),
      allocator(strat) { }

  ~DynamicPoolAllocator() { freeAllBlocks(); }

  void *allocate(std::size_t size) {
    struct Block *best, *prev;
    findUsableBlock(best, prev, size);

    // Allocate a block if needed
    if (!best) allocateBlock(best, prev, size);
    assert(best);

    // Split the free block
    splitBlock(best, prev, size);

    // Push node to the list of used nodes
    best->next = usedBlocks;
    usedBlocks = best;

    // Increment the allocated size
    allocBytes += size;

    // Return the new pointer
    return usedBlocks->data;
  }

  void deallocate(void *ptr) {
    assert(ptr);

    // Find the associated block
    struct Block *curr = usedBlocks, *prev = NULL;
    for ( ; curr && curr->data != ptr; curr = curr->next ) {
      prev = curr;
    }
    if (!curr) return;

    // Remove from allocBytes
    allocBytes -= curr->size;

    // Release it
    releaseBlock(curr, prev);
  }

  std::size_t allocatedSize() const { return allocBytes; }

  std::size_t totalSize() const {
    return totalBytes + blockAllocator.totalSize();
  }

  std::size_t numFreeBlocks() const {
    std::size_t nb = 0;
    for (struct Block *temp = freeBlocks; temp; temp = temp->next) nb++;
    return nb;
  }

  std::size_t numUsedBlocks() const {
    std::size_t nb = 0;
    for (struct Block *temp = usedBlocks; temp; temp = temp->next) nb++;
    return nb;
  }

  std::size_t numOriginalAllocations() const {
    std::size_t nb = 0;
    for (struct Block *temp = originalAllocations; temp; temp = temp->next) nb++;
    return nb;
  }
};

#endif
