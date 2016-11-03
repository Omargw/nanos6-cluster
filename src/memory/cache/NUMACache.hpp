#ifndef NUMA_CACHE_HPP
#define NUMA_CACHE_HPP

#include "GenericCache.hpp" 

class NUMACache: public GenericCache {
public:
    NUMACache() {}
    virtual ~NUMACache() {
        //Iterate over _replicas and free all the replicas.
        for(const auto& replica : _replicas ) {
            free(replica.second._physicalAddress);
        }
    }
    virtual void * allocate(std::size_t size);
    virtual void deallocate(void * ptr);
    virtual void copyData(unsigned int sourceCache, unsigned int homeNode, Task * task);
    virtual void flush(); 
    virtual bool evict();
};

#endif //NUMA_CACHE_HPP
