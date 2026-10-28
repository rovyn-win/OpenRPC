#ifndef OPENRPC_JOBS_H
#define OPENRPC_JOBS_H
#include "RpcCore.h"
#include <new>
namespace OpenRpc {
template<class T, class Platform, unsigned Capacity = MaxPending> class JobStore {
    volatile long live_;
    PendingTable<T, Capacity> pending_;
    struct Guard {
        Guard() { Platform::Lock(); }
        ~Guard() { Platform::Unlock(); }
    };
public:
    JobStore() : live_(0) {}
    T* Create() {
        if (Platform::Increment(&live_) > long(Capacity)) { Platform::Decrement(&live_); return 0; }
        T* p = new(std::nothrow) T;
        if (!p) Platform::Decrement(&live_);
        return p;
    }
    void Add(T* p) { Platform::Increment(&p->references); }
    void Release(T* p) {
        if (p && Platform::Decrement(&p->references) == 0) { delete p; Platform::Decrement(&live_); }
    }
    U32 Publish(T* p, size_t owner) {
        Guard guard;
        U32 token = pending_.Insert(p);
        if (token) { p->owner = owner; p->token = token; Add(p); }
        return token;
    }
    T* Poll(U32 token, size_t owner, bool& completed) {
        Guard guard;
        T* p = pending_.Find(token); completed = false;
        if (!p || p->owner != owner) return 0;
        completed = Platform::Read(&p->done) != 0;
        if (completed) pending_.Remove(token);
        else Add(p);
        return p;
    }
    void Cancel(U32 token) {
        T* p;
        { Guard guard; p = pending_.Remove(token); }
        Release(p);
    }
    void Complete(T* p, U32 now) { p->finished = now; Platform::Exchange(&p->done, 1); }
    void Reap(U32 now, U32 ttl) {
        Guard guard;
        for (unsigned i = 0; i < Capacity; ++i) {
            T* p = pending_.At(i);
            if (p && Platform::Read(&p->done) && U32(now - p->finished) >= ttl) Release(pending_.Remove(pending_.TokenAt(i)));
        }
    }
    long Live() { return Platform::Read(&live_); }
private:
    JobStore(const JobStore&);
    JobStore& operator=(const JobStore&);
};
}
#endif
