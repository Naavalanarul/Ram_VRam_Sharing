#include <meminfo/client/memclient.h>
namespace meminfo { namespace client {
void rmem_init() {}
void rmem_shutdown() {}
void* rmem_alloc(size_t /*size*/) { return nullptr; }
void rmem_write(void* /*ptr*/, const void* /*src*/, size_t /*size*/) {}
void rmem_read(const void* /*ptr*/, void* /*dst*/, size_t /*size*/) {}
void rmem_free(void* /*ptr*/) {}
}}
