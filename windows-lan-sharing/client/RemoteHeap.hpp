#pragma once

#include <cstdint>
#include <memory>
#include <windows.h>

class IPageFaultBackend;

class RemoteHeap {
public:
    RemoteHeap(size_t reserve_size = 8ull * 1024 * 1024 * 1024); // 8 GiB default
    ~RemoteHeap();

    RemoteHeap(const RemoteHeap&) = delete;
    RemoteHeap& operator=(const RemoteHeap&) = delete;
    RemoteHeap(RemoteHeap&&) = delete;
    RemoteHeap& operator=(RemoteHeap&&) = delete;

    bool initialize(IPageFaultBackend* backend);
    void* base() const noexcept { return base_; }
    size_t size() const noexcept { return reserve_size_; }
    bool contains(void* addr) const noexcept;

    static LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* pExceptionInfo);

private:
    void* base_ = nullptr;
    size_t reserve_size_ = 0;
    IPageFaultBackend* backend_ = nullptr;
    static thread_local RemoteHeap* current_heap_;
};