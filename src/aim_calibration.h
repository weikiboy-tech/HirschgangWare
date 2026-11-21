#pragma once
#include "dayz_camera.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace aim_assist {
// The overlay's stricter projection gate may miss a render frame. Measurement
// issues no command on that frame, but only a sustained source loss cancels it.
inline bool calibrationSourceLost(uint64_t now,uint64_t at,bool sameWorld){
    return !sameWorld||!at||now<at||now-at>=250000;
}
class Calibration {
public:
    struct Input {
        uint64_t now=0,session=0,cameraAt=0,idleAt=0;
        dayz_camera::Cam camera{};
        bool device=false,idle=false,fault=false,interrupted=false,cameraOnly=false;
        unsigned generation=0;
    };
    struct Command {int x=0,y=0;};
    enum class State {Idle,Waiting,ReadyToSend,Response,Complete,Failed,Cancelled};
    struct Result {double countsX=0,countsY=0,lensX=0,lensY=0;uint64_t responseUs=16000;};
private:
    State state_=State::Idle;
    const char* status_="Not started";
    uint64_t started_=0,lastNow_=0,stableAt_=0,sentAt_=0,lastCamera_=0,completedAt_=0,session_=0,responseUs_=16000;
    unsigned generation_=0,step_=0;
    dayz_camera::Cam initial_{},baseline_{},previous_{},sent_{};
    std::array<double,16> slopes_{};
    Result result_{};
    static constexpr std::array<int,8> counts_{{1,-1,4,-4,8,-8,16,-16}};
    static constexpr double pi=3.14159265358979323846;
    static double yaw(const dayz_camera::Cam& c){return std::atan2(c.fwd.x,c.fwd.z);}
    static double pitch(const dayz_camera::Cam& c){return -std::atan2(c.fwd.y,std::hypot(c.fwd.x,c.fwd.z));}
    static double wrap(double a){return std::atan2(std::sin(a),std::cos(a));}
    static double delta(const dayz_camera::Cam& a,const dayz_camera::Cam& b){return std::hypot(wrap(yaw(a)-yaw(b)),pitch(a)-pitch(b));}
    static bool validCamera(const dayz_camera::Cam& c){
        if(!dayz_camera::validLens(c)||!dayz_camera::finite(c.pos))return false;
        for(const auto axis:{c.right,c.up,c.fwd})if(!dayz_camera::finite(axis)||std::abs(dayz_camera::dot(axis,axis)-1)>.02)return false;
        return std::abs(dayz_camera::dot(c.right,c.up))<=.02 && std::abs(dayz_camera::dot(c.right,c.fwd))<=.02 &&
            std::abs(dayz_camera::dot(c.up,c.fwd))<=.02;
    }
    void fail(const char* why){state_=State::Failed;status_=why;}
public:
    State state()const{return state_;}
    const char* status()const{return status_;}
    bool running()const{return state_==State::Waiting||state_==State::ReadyToSend||state_==State::Response;}
    bool complete()const{return state_==State::Complete;}
    unsigned progress()const{return step_;}
    uint64_t observedResponseUs()const{return responseUs_;}
    const Result& result()const{return result_;}
    void reset(){*this={};}
    void cancel(){if(running()){state_=State::Cancelled;status_="Cancelled - no further movements";}}
    bool start(const Input& f){
        reset();
        if(!f.device||f.fault||!f.session||!f.cameraAt||f.now<f.cameraAt||f.now-f.cameraAt>=35000||
           !validCamera(f.camera)||f.interrupted){
            fail("Not ready: release keys/buttons; require live camera and MAKCU");return false;
        }
        if(std::abs(f.camera.fwd.y)>.95f||std::abs(f.camera.right.y)>.08f){fail("Look approximately level; stop leaning");return false;}
        initial_=baseline_=previous_=f.camera;started_=lastNow_=stableAt_=f.now;lastCamera_=f.cameraAt;
        session_=f.session;generation_=f.generation;state_=State::Waiting;status_="Keep still - measuring X then Y";return true;
    }
    Command tick(const Input& f){
        if(!running())return {};
        if(f.now<lastNow_||f.now-started_>20000000){fail("Measurement clock/deadline exceeded");return {};}
        if(f.session!=session_||f.generation!=generation_){fail("Measurement session/connection changed");return {};}
        if(!f.device||f.fault){fail("Device channel unavailable - see device diagnostic");return {};}
        if(f.interrupted){fail("Measurement interrupted by key/button/capture");return {};}
        lastNow_=f.now;
        if(calibrationSourceLost(f.now,lastCamera_,true)|| (f.cameraAt && f.cameraAt<lastCamera_)){
            fail("Measurement stopped: camera stream interrupted");return {};
        }
        if(!dayz_camera::fresh(f.cameraAt,f.now,35000)){
            if(state_==State::Waiting)stableAt_=f.now;
            return {};
        }
        if(!validCamera(f.camera)||!dayz_camera::sameLens(initial_,f.camera)){
            fail("Measurement stopped: camera invalid or zoom changed");return {};
        }
        const auto& a=initial_.pos;const auto& b=f.camera.pos;
        if(std::hypot(std::hypot(double(a.x-b.x),double(a.y-b.y)),double(a.z-b.z))>.05||delta(initial_,f.camera)>3*pi/180.){
            fail("Measurement stopped: camera moved too far");return {};
        }
        if(f.cameraAt!=lastCamera_){
            if(delta(previous_,f.camera)>.000002){
                stableAt_=f.now;
                if(state_==State::Response && f.cameraAt>sentAt_)
                    responseUs_=std::max(responseUs_,f.cameraAt-sentAt_+8000);
            } else if(state_==State::Waiting && f.cameraAt-lastCamera_>=35000){
                stableAt_=f.now;
            }
            previous_=f.camera;lastCamera_=f.cameraAt;
        }
        if(state_==State::ReadyToSend)return {}; // One proposal until caller acknowledges it.
        if(state_==State::Waiting){
            if(f.now-stableAt_<150000||!f.idle)return {};
            baseline_=f.camera;state_=State::ReadyToSend;
            const int value=counts_[step_%8];return step_<8?Command{value,0}:Command{0,value};
        }
        if(f.now-sentAt_>600000){fail("No stable camera response; calibration unchanged");return {};}
        if(f.cameraAt<=sentAt_)return {};
        if(f.cameraOnly&&!completedAt_){
            if(delta(sent_,f.camera)>.000001){
                completedAt_=f.cameraAt;
                responseUs_=std::max(responseUs_,completedAt_-sentAt_+8000);
            }
            else if(f.now-sentAt_>200000)fail("Legacy move produced no camera response");
            if(!completedAt_)return {};
        }
        if(!f.cameraOnly&&!completedAt_){
            if(!f.idle||f.idleAt<sentAt_){if(f.now-sentAt_>50000)fail("Device movement completion too slow/unconfirmed");return {};}
            if(f.idleAt-sentAt_>50000){fail("Device movement completion too slow");return {};}
            completedAt_=f.idleAt;
        }
        if((!f.cameraOnly&&!f.idle)||f.cameraAt<=completedAt_||f.now-sentAt_<100000||f.now-stableAt_<60000)return {};
        if(responseUs_>100000){fail("Camera response too slow for bounded tracking");return {};}
        const double dx=wrap(yaw(f.camera)-yaw(sent_)),dy=pitch(f.camera)-pitch(sent_);
        const double main=step_<8?dx:dy,cross=step_<8?dy:dx;
        if(std::abs(main)<.000001){fail("No measurable camera response; no retries sent");return {};}
        if(std::abs(cross)>std::max(.000005,std::abs(main)*.10)){fail("Axes coupled/manual movement; retry while still");return {};}
        slopes_[step_]=main/counts_[step_%8];
        const unsigned first=step_<8?0:8;
        for(unsigned n=first;n<step_;++n)if(slopes_[n]*slopes_[step_]<=0||std::abs(slopes_[step_]/slopes_[n]-1.)>.10){
            fail("Inconsistent sensitivity (>10%); calibration unchanged");return {};
        }
        ++step_;
        if(step_==16){
            double x=0,y=0;for(unsigned n=0;n<8;++n){x+=slopes_[n];y+=slopes_[n+8];}
            result_={8./x,8./y,initial_.px,initial_.py,responseUs_};
            if(!std::isfinite(result_.countsX)||!std::isfinite(result_.countsY)||std::abs(result_.countsX)<1||std::abs(result_.countsY)<1){fail("Invalid measured mapping");return {};}
            state_=State::Complete;status_="Calibration measured - dry run remains ON";return {};
        }
        baseline_=previous_=f.camera;stableAt_=f.now;state_=State::Waiting;
        status_=step_<8?"Measuring horizontal response":"Measuring vertical response";return {};
    }
    void submitted(bool accepted,const Input& f){
        if(state_!=State::ReadyToSend)return;
        if(!accepted){fail("Motion not submitted (click/capture/device busy); retry setup");return;}
        sent_=f.camera;sentAt_=stableAt_=f.now;completedAt_=0;state_=State::Response;
    }
};
// Calibration verifies isolated camera responses. In gameplay, manual input,
// recoil and sway make the total camera delta unsuitable as a command ACK.
// Legacy output is paced by the measured settling bound, with fresh observations
// and no backlog. This is not a claim that a hardware FIFO was queried/cleared.
class OutputCycle {
    bool pending_=false,failed_=false;
    uint64_t sent_=0,session_=0,quiet_=0,lastCamera_=0,minimum_=16000;
    dayz_camera::Cam before_{};
    const char* reason_="No output pending";
    bool fail(const char* why){failed_=true;pending_=false;quiet_=0;lastCamera_=0;reason_=why;return false;}
public:
    bool pending()const{return pending_;}bool failed()const{return failed_;}
    void resetReleaseObservation(){quiet_=0;}
    const char* reason()const{return reason_;}
    void reset(){*this={};}
    void start(const dayz_camera::Cam& c,uint64_t now,uint64_t session,int x,int y,double countsX,double countsY,double settleMs){
        reset();before_=c;sent_=now;session_=session;
        if(!countsX||!countsY||(!x&&!y)||std::abs(x)>16||std::abs(y)>16){fail("Invalid calibrated output command");return;}
        minimum_=uint64_t(std::clamp(settleMs,16.,100.)*1000);pending_=true;
        reason_="Measured output interval pending";
    }
    bool update(const dayz_camera::Cam& c,uint64_t cameraAt,uint64_t now,uint64_t session){
        if(!pending_)return false;
        if(session!=session_)return fail("Output paused: game session changed; release hold key");
        if(now<sent_||now-sent_>250000)return fail("Output paused: update gap; release hold key");
        if(!cameraAt||cameraAt<lastCamera_||now<cameraAt||now-cameraAt>=35000||!dayz_camera::finite(c.fwd))
            return fail("Output paused: camera unavailable; release hold key");
        if(!dayz_camera::sameLens(before_,c))return fail("Output paused: zoom changed; return to calibrated view and release key");
        if(cameraAt==lastCamera_)return false;lastCamera_=cameraAt;
        if(cameraAt>sent_ && now-sent_>=minimum_){pending_=false;reason_="Measured output interval complete";return true;}
        return false;
    }
    // Caller additionally requires an actually released Main-PC key, a valid
    // input source, calibrated lens and a positive serial-channel control.
    bool releasedSourceReady(const dayz_camera::Cam& c,uint64_t cameraAt,uint64_t now,uint64_t session){
        if(!failed_||!session||now<sent_||now-sent_<minimum_||!cameraAt||now<cameraAt||now-cameraAt>=35000||
           !dayz_camera::validLens(c)||!dayz_camera::finite(c.fwd)){quiet_=0;return false;}
        if(!quiet_){quiet_=cameraAt;lastCamera_=cameraAt;session_=session;return false;}
        if(session!=session_||cameraAt<lastCamera_){quiet_=0;return false;}
        lastCamera_=cameraAt;
        return cameraAt>=quiet_ && cameraAt-quiet_>=16000;
    }
};
}
