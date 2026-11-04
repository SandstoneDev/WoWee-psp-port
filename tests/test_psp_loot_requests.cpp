#include "platform/psp/psp_loot_requests.hpp"
#include <cassert>
using wowee::platform::psp::LootRequests;
int main() {
    LootRequests q;q.enqueue(1);q.enqueue(2);q.enqueue(3);q.enqueue(1);
    assert(q.next(100)==1 && q.request(1,100));
    assert(!q.request(2,101) && q.next(101)==-1);
    q.complete(4);assert(q.pending());q.complete(1);
    assert(q.next(150)==2 && q.request(2,150));
    q.fail(200);assert(q.manual() && !q.pending() && q.next(800)==-1);
    assert(!q.request(2,300));assert(q.request(2,701));q.complete(2);
    q.enqueue(3);assert(q.next(702)==3); // explicit retry after making room
    q.reset();assert(!q.manual());q.enqueue(4);q.enqueue(5);
    assert(q.next(1000)==4 && q.request(4,1000));
    assert(q.next(3999)==-1 && !q.manual());
    assert(q.next(4000)==-1 && q.manual()); // bounded timeout, no automatic retry storm
    q.reset();for(int i=0;i<64;++i)q.enqueue(i);
    for(int i=0;i<64;++i){assert(q.next(6000+i)==i);assert(q.request(i,6000+i));q.complete(i);}
    assert(q.next(7000)==-1);
}
