#pragma once
#include "process.h"
#include "player_identity.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace player_roster {
struct Layout {
    uint32_t table, id, name, text;
    int maximum;
};
struct Result {
    player_identity::Names names;
    const char* status = "root unavailable";
    int count = -1;
    unsigned slots = 0, fields = 0, texts = 0, stable = 0, conflicts = 0;
};
// Called at roster cadence, not by drawing/HTTP. Only verified current rows are
// returned; an inaccessible array page cannot invalidate unrelated readable rows.
template<class ValidPointer>
Result read(const Process& process, uintptr_t root, Layout layout, ValidPointer valid) {
    Result result;
    uint64_t client=0,array=0;
    if (!process.read(root,client) || !valid(client) || !process.read(uintptr_t(client+layout.table),array) || !valid(array)) return result;
    if (!process.read(uintptr_t(client+layout.table+8),result.count) || result.count<0 || result.count>layout.maximum) {
        result.status="invalid/unreadable count";return result;
    }
    struct Row {
        uint64_t object=0,string=0,objectAgain=0,stringAgain=0;
        uint32_t id=0,idAgain=0;
        std::array<uint8_t,80> text{};
        size_t used=0,requested=0;
        bool fields=false,done=false,failed=false;
        std::string name;
    };
    std::vector<Row> rows(size_t(result.count));
    std::vector<Process::ReadReq> requests;
    for (size_t i=0;i<rows.size();++i) requests.push_back({uintptr_t(array+i*8),&rows[i].object,8});
    process.readMany(requests);
    std::vector<size_t> live;
    for (size_t i=0;i<rows.size();++i) if (requests[i].ok && valid(rows[i].object)) {live.push_back(i);++result.slots;}
    requests.clear();
    for (size_t i:live) {
        requests.push_back({uintptr_t(rows[i].object+layout.id),&rows[i].id,4});
        requests.push_back({uintptr_t(rows[i].object+layout.name),&rows[i].string,8});
    }
    process.readMany(requests);
    for (size_t n=0;n<live.size();++n) {
        auto& row=rows[live[n]];
        row.fields=requests[2*n].ok && requests[2*n+1].ok && row.id && valid(row.string);
        if (row.fields) ++result.fields;
    }
    // Stop at a proven terminator. Do not require unrelated bytes after a short
    // string to be readable, and never accept an unterminated/truncated prefix.
    for (unsigned pass=0;pass<80;++pass) {
        requests.clear();std::vector<size_t> pending;
        for (size_t i:live) {
            auto& row=rows[i];
            if (!row.fields || row.done || row.failed) continue;
            const uintptr_t address=uintptr_t(row.string+layout.text+row.used);
            row.requested=std::min({size_t(16),row.text.size()-row.used,size_t(4096-(address&4095))});
            if (!row.requested) {row.failed=true;continue;}
            pending.push_back(i);requests.push_back({address,row.text.data()+row.used,row.requested});
        }
        if (requests.empty()) break;
        process.readMany(requests);
        for (size_t n=0;n<pending.size();++n) {
            auto& row=rows[pending[n]];
            if (!requests[n].ok) {row.failed=true;continue;}
            const auto begin=row.text.begin()+row.used;
            row.done=std::find(begin,begin+row.requested,uint8_t(0))!=begin+row.requested;
            row.used+=row.requested;
            if (row.done) {row.name=player_identity::text(row.text.data(),row.used);if (!row.name.empty()) ++result.texts;}
            else if (row.used==row.text.size()) row.failed=true;
        }
    }
    requests.clear();
    for (size_t i:live) {
        auto& row=rows[i];
        requests.push_back({uintptr_t(array+i*8),&row.objectAgain,8});
        requests.push_back({uintptr_t(row.object+layout.id),&row.idAgain,4});
        requests.push_back({uintptr_t(row.object+layout.name),&row.stringAgain,8});
    }
    process.readMany(requests);
    uint64_t clientAgain=0,arrayAgain=0;
    int countAgain=-1;
    if (!process.read(root,clientAgain) || clientAgain!=client ||
        !process.read(uintptr_t(client+layout.table),arrayAgain) || arrayAgain!=array ||
        !process.read(uintptr_t(client+layout.table+8),countAgain) || countAgain<0 || countAgain>layout.maximum) {
        result.status="root changed/unreadable";return result;
    }
    std::unordered_set<uint32_t> conflicts;
    std::unordered_set<uint64_t> objects;
    for (size_t n=0;n<live.size();++n) {
        const size_t i=live[n];const auto& row=rows[i];
        if (i>=size_t(countAgain) || !row.fields || row.failed || row.name.empty() ||
            !requests[3*n].ok || !requests[3*n+1].ok || !requests[3*n+2].ok ||
            row.objectAgain!=row.object || row.idAgain!=row.id || row.stringAgain!=row.string) continue;
        if (!objects.insert(row.object).second) continue;
        ++result.stable;
        player_identity::insert(result.names,conflicts,row.id,row.name);
    }
    result.conflicts=unsigned(conflicts.size());
    result.status=result.names.empty() ? (result.count ? "no validated identities" : "empty roster") : "ready";
    return result;
}
}
