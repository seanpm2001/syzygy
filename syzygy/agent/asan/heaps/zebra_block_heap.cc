// Copyright 2014 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "syzygy/agent/asan/heaps/zebra_block_heap.h"

#include <algorithm>

#include "syzygy/common/align.h"
#include "syzygy/common/asan_parameters.h"

namespace agent {
namespace asan {
namespace heaps {

const size_t ZebraBlockHeap::kSlabSize = 2 * GetPageSize();

const size_t ZebraBlockHeap::kMaximumAllocationSize = GetPageSize();

const size_t ZebraBlockHeap::kMaximumBlockAllocationSize =
    GetPageSize() - sizeof(BlockHeader);

ZebraBlockHeap::ZebraBlockHeap(size_t heap_size,
                               MemoryNotifierInterface* memory_notifier,
                               HeapInterface* internal_heap)
    : heap_address_(NULL),
      // Makes the heap_size a multiple of kSlabSize to avoid incomplete slabs
      // at the end of the reserved memory.
      heap_size_(::common::AlignUp(heap_size, kSlabSize)),
      slab_count_(heap_size_ / kSlabSize),
      slab_info_(HeapAllocator<SlabInfo>(internal_heap)),
      quarantine_ratio_(::common::kDefaultZebraBlockHeapQuarantineRatio),
      free_slabs_(slab_count_,
                  HeapAllocator<size_t>(internal_heap)),
      quarantine_(slab_count_,
                  HeapAllocator<size_t>(internal_heap)),
      memory_notifier_(memory_notifier) {
  DCHECK_NE(reinterpret_cast<MemoryNotifierInterface*>(NULL), memory_notifier);

  // Allocate the chunk of memory directly from the OS.
  heap_address_ = reinterpret_cast<uint8*>(
      ::VirtualAlloc(NULL,
                     heap_size_,
                     MEM_RESERVE | MEM_COMMIT,
                     PAGE_READWRITE));
  CHECK_NE(reinterpret_cast<uint8*>(NULL), heap_address_);
  DCHECK(::common::IsAligned(heap_address_, GetPageSize()));
  memory_notifier_->NotifyFutureHeapUse(heap_address_, heap_size_);

  // Initialize the metadata describing the state of our heap.
  slab_info_.resize(slab_count_);
  for (size_t i = 0; i < slab_count_; ++i) {
    slab_info_[i].state = kFreeSlab;
    ::memset(&slab_info_[i].info, 0, sizeof(slab_info_[i].info));
    free_slabs_.push(i);
  }
}

ZebraBlockHeap::~ZebraBlockHeap() {
  DCHECK_NE(reinterpret_cast<uint8*>(NULL), heap_address_);
  CHECK_NE(FALSE, ::VirtualFree(heap_address_, 0, MEM_RELEASE));
  memory_notifier_->NotifyReturnedToOS(heap_address_, heap_size_);
  heap_address_ = NULL;
}

uint32 ZebraBlockHeap::GetHeapFeatures() const {
  return kHeapSupportsIsAllocated | kHeapReportsReservations |
      kHeapSupportsGetAllocationSize;
}

void* ZebraBlockHeap::Allocate(size_t bytes) {
  SlabInfo* slab_info = AllocateImpl(bytes);
  if (slab_info == NULL)
    return NULL;
  return slab_info->info.block;
}

bool ZebraBlockHeap::Free(void* alloc) {
  if (alloc == NULL)
    return true;
  ::common::AutoRecursiveLock lock(lock_);
  size_t slab_index = GetSlabIndex(alloc);
  if (slab_index == kInvalidSlabIndex)
    return false;
  if (slab_info_[slab_index].info.block != alloc)
    return false;

  // Memory must be released from the quarantine before calling Free.
  DCHECK_NE(kQuarantinedSlab, slab_info_[slab_index].state);

  if (slab_info_[slab_index].state == kFreeSlab)
    return false;

  // Make the slab available for allocations.
  slab_info_[slab_index].state = kFreeSlab;
  ::memset(&slab_info_[slab_index].info, 0,
           sizeof(slab_info_[slab_index].info));
  free_slabs_.push(slab_index);
  return true;
}

bool ZebraBlockHeap::IsAllocated(const void* alloc) {
  if (alloc == NULL)
    return false;
  ::common::AutoRecursiveLock lock(lock_);
  size_t slab_index = GetSlabIndex(alloc);
  if (slab_index == kInvalidSlabIndex)
    return false;
  if (slab_info_[slab_index].state == kFreeSlab)
    return false;
  if (slab_info_[slab_index].info.block != alloc)
    return false;
  return true;
}

size_t ZebraBlockHeap::GetAllocationSize(const void* alloc) {
  if (alloc == NULL)
    return kUnknownSize;
  ::common::AutoRecursiveLock lock(lock_);
  size_t slab_index = GetSlabIndex(alloc);
  if (slab_index == kInvalidSlabIndex)
    return kUnknownSize;
  if (slab_info_[slab_index].state == kFreeSlab)
    return kUnknownSize;
  if (slab_info_[slab_index].info.block != alloc)
    return kUnknownSize;
  return slab_info_[slab_index].info.block_size;
}

void ZebraBlockHeap::Lock() {
  lock_.Acquire();
}

void ZebraBlockHeap::Unlock() {
  lock_.Release();
}

bool ZebraBlockHeap::TryLock() {
  return lock_.Try();
}

void* ZebraBlockHeap::AllocateBlock(size_t size,
                                    size_t min_left_redzone_size,
                                    size_t min_right_redzone_size,
                                    BlockLayout* layout) {
  DCHECK_NE(static_cast<BlockLayout*>(nullptr), layout);
  // Abort if the redzones do not fit in a page. Even if the allocation
  // is possible it will lead to a non-standard block layout.
  if (min_left_redzone_size + size > GetPageSize())
    return NULL;
  if (min_right_redzone_size > GetPageSize())
    return NULL;

  // Plan the block layout.
  if (!BlockPlanLayout(GetPageSize(),
                       kShadowRatio,
                       size,
                       min_left_redzone_size,
                       std::max(GetPageSize(), min_right_redzone_size),
                       layout)) {
    return nullptr;
  }

  if (layout->block_size != kSlabSize)
    return nullptr;
  size_t right_redzone_size = layout->trailer_size +
      layout->trailer_padding_size;
  // Part of the body lies inside an "odd" page.
  if (right_redzone_size < GetPageSize())
    return nullptr;
  // There should be less than kShadowRatio bytes between the body end
  // and the "odd" page.
  if (right_redzone_size - GetPageSize() >= kShadowRatio)
    return nullptr;

  // Allocate space for the block, and update the slab info to reflect the right
  // redzone.
  void* alloc = nullptr;
  SlabInfo* slab_info = AllocateImpl(GetPageSize());
  if (slab_info != nullptr) {
    slab_info->info.block_size = layout->block_size;
    slab_info->info.header_size = layout->header_size +
        layout->header_padding_size;
    slab_info->info.trailer_size = layout->trailer_size +
        layout->trailer_padding_size;
    slab_info->info.is_nested = false;
    alloc = slab_info->info.block;
  }

  DCHECK_EQ(0u, reinterpret_cast<uintptr_t>(alloc) % kShadowRatio);
  return alloc;
}

bool ZebraBlockHeap::FreeBlock(const BlockInfo& block_info) {
  DCHECK_NE(static_cast<uint8*>(NULL), block_info.block);
  if (!Free(block_info.block))
    return false;
  return true;
}

bool ZebraBlockHeap::Push(const CompactBlockInfo& info) {
  ::common::AutoRecursiveLock lock(lock_);
  size_t slab_index = GetSlabIndex(info.block);
  if (slab_index == kInvalidSlabIndex)
    return false;
  if (slab_info_[slab_index].state != kAllocatedSlab)
    return false;
  if (::memcmp(&slab_info_[slab_index].info, &info,
               sizeof(info)) != 0) {
    return false;
  }

  quarantine_.push(slab_index);
  slab_info_[slab_index].state = kQuarantinedSlab;
  return true;
}

bool ZebraBlockHeap::Pop(CompactBlockInfo* info) {
  ::common::AutoRecursiveLock lock(lock_);

  if (QuarantineInvariantIsSatisfied())
    return false;

  size_t slab_index = quarantine_.front();
  DCHECK_NE(kInvalidSlabIndex, slab_index);
  quarantine_.pop();

  DCHECK_EQ(kQuarantinedSlab, slab_info_[slab_index].state);
  slab_info_[slab_index].state = kAllocatedSlab;
  *info = slab_info_[slab_index].info;

  return true;
}

void ZebraBlockHeap::Empty(ObjectVector* infos) {
  ::common::AutoRecursiveLock lock(lock_);
  while (!quarantine_.empty()) {
    size_t slab_index = quarantine_.front();
    DCHECK_NE(kInvalidSlabIndex, slab_index);
    quarantine_.pop();

    // Do not free the slab, only release it from the quarantine.
    slab_info_[slab_index].state = kAllocatedSlab;
    infos->push_back(slab_info_[slab_index].info);
  }
}

size_t ZebraBlockHeap::GetCount() {
  ::common::AutoRecursiveLock lock(lock_);
  return quarantine_.size();
}

void ZebraBlockHeap::set_quarantine_ratio(float quarantine_ratio) {
  DCHECK_LE(0, quarantine_ratio);
  DCHECK_GE(1, quarantine_ratio);
  ::common::AutoRecursiveLock lock(lock_);
  quarantine_ratio_ = quarantine_ratio;
}

ZebraBlockHeap::SlabInfo* ZebraBlockHeap::AllocateImpl(size_t bytes) {
  if (bytes == 0 || bytes > GetPageSize())
    return NULL;
  ::common::AutoRecursiveLock lock(lock_);

  if (free_slabs_.empty())
    return NULL;

  size_t slab_index = free_slabs_.front();
  DCHECK_NE(kInvalidSlabIndex, slab_index);
  free_slabs_.pop();
  uint8* slab_address = GetSlabAddress(slab_index);
  DCHECK_NE(reinterpret_cast<uint8*>(NULL), slab_address);

  // Push the allocation to the end of the even page.
  uint8* alloc = slab_address + GetPageSize() - bytes;
  alloc = ::common::AlignDown(alloc, kShadowRatio);

  // Update the slab info.
  SlabInfo* slab_info = &slab_info_[slab_index];
  slab_info->state = kAllocatedSlab;
  slab_info->info.block = alloc;
  slab_info->info.block_size = bytes;
  slab_info->info.header_size = 0;
  slab_info->info.trailer_size = 0;
  slab_info->info.is_nested = false;

  return slab_info;
}

bool ZebraBlockHeap::QuarantineInvariantIsSatisfied() {
  return quarantine_.empty() ||
         (quarantine_.size() / static_cast<float>(slab_count_) <=
             quarantine_ratio_);
}

uint8* ZebraBlockHeap::GetSlabAddress(size_t index) {
  if (index >= slab_count_)
    return NULL;
  return heap_address_ + index * kSlabSize;
}

size_t ZebraBlockHeap::GetSlabIndex(const void* address) {
  if (address < heap_address_ || address >= heap_address_ + heap_size_)
    return kInvalidSlabIndex;
  return (reinterpret_cast<const uint8*>(address) - heap_address_) / kSlabSize;
}

}  // namespace heaps
}  // namespace asan
}  // namespace agent
