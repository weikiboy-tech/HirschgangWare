#pragma once
#include <cstdint>
#include <string>

namespace aim_assist {
inline constexpr int kLegacyCommandLimit=16;
inline constexpr int kScopedCommandLimit=32;

// Read-only pending-motion queries, consumed by the existing serial owner.
// A fresh channel reply can restore a previously measured profile. It cannot
// establish a new movement calibration.
struct MotionProtocol {
    bool enabled=false,waiting=false,known=false,idle=false,fault=false,verified=false;
    bool versionQuery=false,cameraOnly=false,awaitingCamera=false;
    bool paused=false,restoring=false;
    uint64_t asked=0,at=0,lastPoll=0;
    unsigned generation=0;
    const char* note="Not checked";
    void reset(){const unsigned next=generation+1;*this={};generation=next;}
    void begin(){reset();enabled=true;note="Checking pending movement";}
    bool beginRestore(){
        if(enabled||fault)return false;
        begin();restoring=true;note="Checking connection for saved calibration";return true;
    }
    void fail(const char* why){waiting=false;known=false;fault=true;paused=false;verified=restoring=false;note=why;}
    void cancel(){enabled=false;waiting=false;known=false;paused=false;verified=restoring=false;if(!fault)note="Setup stopped";}
    void restoreIfIdle(){
        if(restoring && idle){
            verified=true;restoring=false;note="Saved calibration restored after channel check";
        }
    }
    void pause(const char* why){
        if(fault||!verified)return;
        paused=true;waiting=false;known=false;awaitingCamera=false;note=why;
    }
    bool resume(){
        if(!paused||fault||!verified||!enabled)return false;
        paused=false;waiting=false;known=false;idle=false;awaitingCamera=false;
        versionQuery=true;lastPoll=0;note="Checking device channel after release";return true;
    }
    const char* queryCommand()const{return versionQuery?"km.version()":"km.moving()";}
    bool queryDue(uint64_t now){
        if(!enabled||fault||paused)return false;
        if(waiting){
            if(now<asked){fail("Motion query clock changed");return false;}
            if(now-asked>250000){
                if(!versionQuery){waiting=false;versionQuery=true;known=false;note="No pending-status reply; checking legacy channel";}
                else fail("MAKCU identity control query timed out");
            }
            return false;
        }
        if(cameraOnly && (awaitingCamera || (lastPoll&&now-lastPoll<500000)))return false;
        if(known&&idle&&now>=at&&now-at<30000)return false; // Cache idle while no move was submitted.
        if(lastPoll&&now-lastPoll<8000)return false;
        waiting=true;asked=lastPoll=now;return true;
    }
    bool reply(std::string line,uint64_t now){
        if(!enabled)return false;
        const auto first=line.find_first_not_of(" \t>\r\n");if(first==std::string::npos)return false;
        line=line.substr(first);const auto end=line.find_last_not_of(" \t\r\n");line.resize(end+1);
        if(line=="ERR"||line.rfind("ERR ",0)==0){
            if(waiting&&!versionQuery){waiting=false;versionQuery=true;known=false;note="Pending status unavailable; checking legacy channel";return true;}
            fail("MAKCU rejected motion/query command");return true;
        }
        if(!waiting||now<asked||now-asked>250000)return false;
        if(versionQuery){
            if(line!="km.MAKCU")return false;
            waiting=false;known=true;idle=true;cameraOnly=true;at=now;
            note=verified?"Legacy channel ready":"Legacy channel alive - camera response must be measured";
            restoreIfIdle();return true;
        }
        // V4 result and explicit tagged result. Ignore query echoes/prompts,
        // identities, input frames and arbitrary numeric/text values.
        int value=-1;
        if(line=="0"||line=="km.moving(0)")value=0;
        if(line=="1"||line=="km.moving(1)")value=1;
        if(value<0)return false;
        waiting=false;known=true;idle=value==0;at=now;note=idle?"MAKCU idle":"MAKCU movement pending";
        restoreIfIdle();return true;
    }
    bool idleFresh(uint64_t now)const{return enabled&&!fault&&!paused&&known&&!waiting&&idle&&now>=at&&now-at<(cameraOnly?1000000u:50000u);}
    void moved(bool calibration=false){known=false;waiting=false;awaitingCamera=cameraOnly;if(!cameraOnly)lastPoll=0;
        note=cameraOnly?(calibration?"Awaiting calibration camera response":"Measured output interval pending"):"Awaiting movement completion";}
    void intervalCompleted(uint64_t now,bool isolatedResponse=false){if(cameraOnly&&enabled&&!fault&&!paused){
        awaitingCamera=false;known=idle=true;at=now;
        note=isolatedResponse?"Calibration camera response observed (queue unknown)":"Measured output interval elapsed (queue not queried)";
    }}
};
}
