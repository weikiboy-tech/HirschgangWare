#include "../src/player_roster.h"
#include <cassert>
#include <cstdio>
#include <functional>
#include <map>

class Memory final : public Process {
public:
    mutable std::map<uintptr_t,uint8_t> bytes;
    mutable std::map<uintptr_t,unsigned> reads;
    std::function<void(const Memory&,uintptr_t,unsigned)> hook;
    bool attach(const std::string&,std::string&) override {return true;}
    bool read(uintptr_t at,void* target,size_t length) const override {
        const auto number=++reads[at];if(hook)hook(*this,at,number);
        for(size_t i=0;i<length;++i) if(!bytes.count(at+i))return false;
        for(size_t i=0;i<length;++i)static_cast<uint8_t*>(target)[i]=bytes.at(at+i);
        return true;
    }
    bool readable(uintptr_t at) const override {return bytes.count(at)!=0;}
    std::vector<Region> regions(uint64_t) const override {return {};}
    bool alive() const override {return true;}
    template<class T> void put(uintptr_t at,T value) const {
        for(size_t i=0;i<sizeof value;++i)bytes[at+i]=reinterpret_cast<uint8_t*>(&value)[i];
    }
    void row(size_t slot,uint32_t id,const std::string& name,uintptr_t str=0) {
        const uintptr_t object=0x30000+slot*0x1000;
        if(!str)str=0x80000+slot*0x1000;
        put<uint64_t>(0x20000+slot*8,object);put<uint32_t>(object+0x30,id);put<uint64_t>(object+0xf8,str);
        for(size_t i=0;i<80;++i)bytes[str+16+i]=i<name.size() ? uint8_t(name[i]) : 0;
    }
    Memory() {put<uint64_t>(0x1000,0x10000);put<uint64_t>(0x10018,0x20000);put<int>(0x10020,3);row(0,7,"Alpha");row(1,8,"Bravo");row(2,9,"Charlie");}
};
const player_roster::Layout layout{0x18,0x30,0xf8,0x10,256};
auto roster(const Memory& memory) {return player_roster::read(memory,0x1000,layout,[](uint64_t p){return p>=0x10000 && p<0x7fffffffffff;});}
int main() {
    Memory m;auto result=roster(m);assert(result.names.size()==3 && result.stable==3);
    // A gap in one array slot used to reject the entire contiguous array read.
    m=Memory();for(int i=0;i<8;++i)m.bytes.erase(0x20008+i);
    result=roster(m);assert(result.names.size()==2 && result.names.count(7) && result.names.count(9));
    m=Memory();m.row(1,7,"Alpha");result=roster(m);assert(result.names.size()==2 && result.names.at(7)=="Alpha");
    m=Memory();m.row(1,7,"Wrong person");result=roster(m);assert(!result.names.count(7) && result.conflicts==1);
    // Existing entity ambiguity checks still reject two entities claiming one ID.
    assert(player_identity::resolve({{7,"Alpha"}},{{0x30000,7},{0x40000,7}}).empty());
    m=Memory();m.row(0,7,"Edge",0x80fe8);
    for(uintptr_t at=0x81000;at<0x81050;++at)m.bytes.erase(at);
    result=roster(m);assert(result.names.at(7)=="Edge");
    m=Memory();m.row(0,7,std::string(80,'A'));result=roster(m);assert(!result.names.count(7));
    m=Memory();m.row(0,7,"M\xc3\xbcller");result=roster(m);assert(result.names.at(7)=="M\xc3\xbcller");
    m=Memory();m.row(0,7,std::string("Bad\xc3\x28"));result=roster(m);assert(!result.names.count(7));
    m=Memory();m.hook=[](const Memory& x,uintptr_t at,unsigned n){if(at==0x20000 && n==2)x.put<uint64_t>(at,0xdeadbeef);};
    result=roster(m);assert(!result.names.count(7) && result.names.size()==2);
    m=Memory();m.hook=[](const Memory& x,uintptr_t at,unsigned n){if(at==0x30030 && n==2)x.put<uint32_t>(at,99);};
    result=roster(m);assert(!result.names.count(7) && result.names.size()==2);
    m=Memory();m.hook=[](const Memory& x,uintptr_t at,unsigned n){if(at==0x300f8 && n==2)x.put<uint64_t>(at,0x99000);};
    result=roster(m);assert(!result.names.count(7));
    m=Memory();m.hook=[](const Memory& x,uintptr_t at,unsigned n){if(at==0x1000 && n==2)x.put<uint64_t>(at,0x99000);};
    result=roster(m);assert(result.names.empty());
    m=Memory();m.hook=[](const Memory& x,uintptr_t at,unsigned n){if(at==0x10020 && n==2)x.put<int>(at,2);};
    result=roster(m);assert(result.names.size()==2 && !result.names.count(9));
    m=Memory();m.hook=[](const Memory& x,uintptr_t at,unsigned n){if(at==0x10020 && n==2)x.put<int>(at,4);};
    result=roster(m);assert(result.names.size()==3); // Stable old slots, no invented new row.
    m=Memory();m.put<int>(0x10020,257);assert(roster(m).names.empty());
    m.put<int>(0x10020,0);assert(roster(m).names.empty());
    m=Memory();assert(roster(m).names.size()==3); // No stale failure latch after recovery.
    puts("PASS player roster: partial tables, verified membership, changing counts, page-edge strings, UTF-8, duplicates/conflicts, identity reuse and recovery");
}
