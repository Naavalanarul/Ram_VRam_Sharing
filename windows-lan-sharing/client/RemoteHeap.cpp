#include "RemoteHeap.hpp"
#include "IPageFaultBackend.hpp"
#include "PagingWireProtocol.hpp"
#include <windows.h>
#include <cassert>

thread_local RemoteHeap* RemoteHeap::current_heap_ = nullptr;

RemoteHeap::RemoteHeap(size_t reserve_size)
    : reserve_size_(reserve_size) {
    base_ = VirtualAlloc(nullptr, reserve_size_, MEM_RESERVE, PAGE_NOACCESS);
    if (!base_) {
        return;
    }

    current_heap_ = this;
    AddVectoredExceptionHandler(1, VectoredHandler);
}

RemoteHeap::~RemoteHeap() {
    if (current_heap_ == this) {
        current_heap_ = nullptr;
    }
    RemoveVectoredExceptionHandler(VectoredHandler);
    if (base_) {
        VirtualFree(base_, 0, MEM_RELEASE);
        base_ = nullptr;
    }
}

bool RemoteHeap::initialize(IPageFaultBackend* backend) {
    if (!base_) {
        return false;
    }
    backend_ = backend;
    return true;
}

bool RemoteHeap::contains(void* addr) const noexcept {
    uint8_t* ptr = static_cast<uint8_t*>(addr);
    uint8_t* base = static_cast<uint8_t*>(base_);
    return ptr >= base && ptr < base + reserve_size_;
}

LONG WINAPI RemoteHeap::VectoredHandler(EXCEPTION_POINTERS* pExceptionInfo) {
    EXCEPTION_RECORD* record = pExceptionInfo->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (record->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    uintptr_t fault_addr = record->ExceptionInformation[1];
    RemoteHeap* heap = current_heap_;
    if (!heap || !heap->contains(reinterpret_cast<void*>(fault_addr)) || !heap->backend_) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    uint64_t cluster_base = align_down_cluster(fault_addr);
    void* commit_addr = reinterpret_cast<void*>(cluster_base);

    void* result = VirtualAlloc(commit_addr, CLUSTER_SIZE, MEM_COMMIT, PAGE_READWRITE);
    if (!result) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    bool ok = heap->backend_->fetch_page_cluster(cluster_base, CLUSTER_SIZE, commit_addr);
    if (!ok) {
        VirtualFree(commit_addr, CLUSTER_SIZE, MEM_DECOMMIT);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}