#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

namespace wowee::rendering::psp {
// Immutable broad phase, built from the exact decoded chunk bounds. Queries do
// not allocate, cache residency, or change the collision narrow phase.
class CollisionBoundsIndex {
public:
    struct Entry { glm::vec3 lo, hi; uint32_t id; };
    void build(std::vector<Entry> entries) {
        entries_ = std::move(entries);
        nodes_.clear();
        if (!entries_.empty()) buildNode(0, static_cast<uint32_t>(entries_.size()));
    }
    void clear() { std::vector<Entry>().swap(entries_); std::vector<Node>().swap(nodes_); }
    size_t bytes() const { return entries_.capacity()*sizeof(Entry)+nodes_.capacity()*sizeof(Node); }
    template<class F> void query(const glm::vec3& lo, const glm::vec3& hi, F&& visit) const {
        if (!nodes_.empty()) queryNode(0, lo, hi, visit);
    }
    // Near-first slab traversal. The callback may shorten maxT after a triangle hit;
    // subsequent nodes are tested against that shorter ray. No per-query allocation.
    template<class F> void rayQuery(const glm::vec3& origin,const glm::vec3& direction,float& maxT,F&& visit) const {
        if (!nodes_.empty() && maxT>=0.f) rayNode(0,origin,direction,maxT,visit);
    }
private:
    static bool rayBox(const glm::vec3& o,const glm::vec3& d,const glm::vec3& lo,
                       const glm::vec3& hi,float maxT,float& entry) {
        float first=0.f,last=maxT;
        for(int a=0;a<3;++a) {
            // Exact zero only: transformed, non-unit directions can be very small.
            if(d[a]==0.f) {if(o[a]<lo[a]-.0001f || o[a]>hi[a]+.0001f)return false;continue;}
            float x=(lo[a]-.0001f-o[a])/d[a],y=(hi[a]+.0001f-o[a])/d[a];
            if(x>y)std::swap(x,y);
            first=std::max(first,x);last=std::min(last,y);
            if(first>last)return false;
        }
        entry=first;return true;
    }
    template<class F> void rayNode(uint32_t idx,const glm::vec3& o,const glm::vec3& d,float& best,F& visit) const {
        const Node& n=nodes_[idx];float t;
        if(!rayBox(o,d,n.lo,n.hi,best,t))return;
        if(n.count) {
            // At most eight leaves: order the candidates on the stack.
            struct Hit {float t;uint32_t entry;};Hit hits[8];unsigned count=0;
            for(uint32_t i=n.first;i<n.first+n.count;++i)
                if(rayBox(o,d,entries_[i].lo,entries_[i].hi,best,t))hits[count++]={t,i};
            std::sort(hits,hits+count,[](const Hit& a,const Hit& b){return a.t<b.t;});
            for(unsigned i=0;i<count;++i)if(hits[i].t<=best)visit(entries_[hits[i].entry]);
            return;
        }
        float l=0,r=0;
        const bool left=rayBox(o,d,nodes_[n.left].lo,nodes_[n.left].hi,best,l);
        const bool right=rayBox(o,d,nodes_[n.right].lo,nodes_[n.right].hi,best,r);
        if(left && right) {
            rayNode(l<=r?n.left:n.right,o,d,best,visit);
            rayNode(l<=r?n.right:n.left,o,d,best,visit);
        } else if(left)rayNode(n.left,o,d,best,visit);
        else if(right)rayNode(n.right,o,d,best,visit);
    }
    struct Node { glm::vec3 lo, hi; uint32_t first, count, left, right; };
    std::vector<Entry> entries_;
    std::vector<Node> nodes_;
    static bool overlaps(const glm::vec3& a, const glm::vec3& b,
                         const glm::vec3& lo, const glm::vec3& hi) {
        return a.x<=hi.x && b.x>=lo.x && a.y<=hi.y && b.y>=lo.y && a.z<=hi.z && b.z>=lo.z;
    }
    uint32_t buildNode(uint32_t first, uint32_t count) {
        Node node{entries_[first].lo, entries_[first].hi, first, count, 0, 0};
        for (uint32_t i=first+1; i<first+count; ++i) {
            node.lo=glm::min(node.lo,entries_[i].lo); node.hi=glm::max(node.hi,entries_[i].hi);
        }
        const uint32_t idx=static_cast<uint32_t>(nodes_.size());
        nodes_.push_back(node);
        if (count>8) {
            const glm::vec3 span=node.hi-node.lo;
            const int axis=span.x>=span.y && span.x>=span.z ? 0 : span.y>=span.z ? 1 : 2;
            const uint32_t mid=first+count/2;
            std::nth_element(entries_.begin()+first, entries_.begin()+mid, entries_.begin()+first+count,
                [axis](const Entry& a,const Entry& b) { return a.lo[axis]+a.hi[axis]<b.lo[axis]+b.hi[axis]; });
            const uint32_t left=buildNode(first,count/2), right=buildNode(mid,count-count/2);
            nodes_[idx].left=left; nodes_[idx].right=right; nodes_[idx].count=0;
        }
        return idx;
    }
    template<class F> void queryNode(uint32_t idx,const glm::vec3& lo,const glm::vec3& hi,F& visit) const {
        const Node& n=nodes_[idx];
        if (!overlaps(n.lo,n.hi,lo,hi)) return;
        if (!n.count) { queryNode(n.left,lo,hi,visit); queryNode(n.right,lo,hi,visit); return; }
        for (uint32_t i=n.first; i<n.first+n.count; ++i) {
            const Entry& e=entries_[i];
            if (overlaps(e.lo,e.hi,lo,hi)) visit(e);
        }
    }
};
}
