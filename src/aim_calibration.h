#pragma once
#include "aim_motion_protocol.h"
#include "dayz_camera.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace aim_assist {
inline constexpr double kMaxScopedResponseMs=180.;
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
        bool compensateDrift=false;
        uint64_t cameraSource=0;
    };
    struct Command {int x=0,y=0;};
    enum class State {Idle,Waiting,ReadyToSend,Response,Complete,Failed,Cancelled};
    struct Result {
        double countsX=0,countsY=0,lensX=0,lensY=0;
        uint64_t responseUs=16000;
        int commandLimit=kLegacyCommandLimit;
    };
private:
    State state_=State::Idle;
    const char* status_="Not started";
    uint64_t started_=0,lastNow_=0,stableAt_=0,sentAt_=0,lastCamera_=0,completedAt_=0,session_=0,responseUs_=16000;
    unsigned generation_=0,step_=0;
    dayz_camera::Cam initial_{},baseline_{},previous_{},sent_{};
    std::array<double,16> slopes_{};
    Result result_{};
    static constexpr std::array<int,8> counts_{{1,-1,4,-4,8,-8,16,-16}};
    struct Angles {
        double x=0,y=0;
    };
    // Each axis needs at least three of four balanced pairs. That requires a
    // valid large pair and a valid small pair in the gain consistency check.
    static constexpr std::array<int,10> scopePulses_{{0,12,-12,-kScopedCommandLimit,kScopedCommandLimit,
        kScopedCommandLimit,-kScopedCommandLimit,0,-12,12}};
    static constexpr std::array<uint64_t,6> scopeLags_{{40000,65000,90000,115000,140000,165000}};
    struct Observation {Angles angle{};bool valid=false;};
    std::array<std::array<Observation,scopeLags_.size()>,scopePulses_.size()*2> observations_{};
    uint64_t lastScopeAge_=0;
    Angles lastScopeAngle_{};
    bool compensateDrift_=false;
    uint64_t cameraSource_=0;
    static constexpr double pi=3.14159265358979323846;
    static double yaw(const dayz_camera::Cam& c){return std::atan2(c.fwd.x,c.fwd.z);}
    static double pitch(const dayz_camera::Cam& c){return -std::atan2(c.fwd.y,std::hypot(c.fwd.x,c.fwd.z));}
    static double wrap(double a){return std::atan2(std::sin(a),std::cos(a));}
    static Angles difference(const dayz_camera::Cam& a,const dayz_camera::Cam& b){
        return {wrap(yaw(a)-yaw(b)),pitch(a)-pitch(b)};
    }
    struct AxisFit {
        double gain=0;
        const char* failure=nullptr;
        bool valid()const{return failure==nullptr;}
    };
    AxisFit scopeFit(size_t axis,size_t lag)const{
        const auto own=[axis](Angles a){return axis?a.y:a.x;};
        const auto cross=[axis](Angles a){return axis?a.x:a.y;};
        const size_t offset=axis*scopePulses_.size();
        const auto sample=[&](size_t i){return observations_[offset+i][lag];};
        const auto angle=[&](size_t i){return sample(i).angle;};
        constexpr std::array<size_t,4> pairs{{1,3,5,8}};
        std::array<double,pairs.size()> slopes{},crosstalk{};
        std::array<bool,pairs.size()> available{};
        double fitSum=0,checkSum=0;
        unsigned fitCount=0,checkCount=0;
        for(size_t n=0;n<pairs.size();++n){
            const auto i=pairs[n];
            if(!sample(i).valid||!sample(i+1).valid)continue;
            const double counts=double(scopePulses_[i]-scopePulses_[i+1]);
            slopes[n]=(own(angle(i))-own(angle(i+1)))/counts;
            crosstalk[n]=(cross(angle(i))-cross(angle(i+1)))/counts;
            available[n]=true;
            if(n<2){fitSum+=slopes[n];++fitCount;}
            else{checkSum+=slopes[n];++checkCount;}
        }
        const bool sham1=sample(0).valid,sham2=sample(7).valid;
        if(!fitCount||!checkCount||fitCount+checkCount<3||(!sham1&&!sham2))
            return {0,"Scope sample coverage insufficient - no profile saved"};
        const double gain=fitSum/fitCount;
        if(std::abs(gain)<.00001)return {0,"Scope movement signal too weak - no profile saved"};
        // Estimate on the first two pairs and verify on the remaining pairs.
        for(size_t n=0;n<pairs.size();++n){
            if(!available[n])continue;
            if(std::abs(slopes[n]/gain-1)>.30)
                return {0,"Scope confirmation did not reproduce the gain - no profile saved"};
            if(std::abs(crosstalk[n])>.35*std::abs(gain))
                return {0,"Scope cross-axis response too large - no profile saved"};
        }
        // Compare the two no-input windows and the background component of
        // each balanced pair against the signal expected from the added input.
        const double shamA=own(angle(0)),shamB=own(angle(7));
        const double background=sham1&&sham2?(shamA+shamB)*.5:sham1?shamA:shamB;
        if(sham1&&sham2&&std::abs(shamA-shamB)>12*std::abs(gain))
            return {0,"Scope sway dominated the no-input windows - no profile saved"};
        for(size_t n=0;n<pairs.size();++n){
            if(!available[n])continue;
            const auto i=pairs[n];
            const double mean=(own(angle(i))+own(angle(i+1)))*.5;
            if(std::abs(mean-background)>12*std::abs(gain))
                return {0,"Scope background changed between pulses - no profile saved"};
        }
        return {(gain+checkSum/checkCount)*.5,nullptr};
    }
    void finishScope(){
        const auto x=scopeFit(0,scopeLags_.size()-1),y=scopeFit(1,scopeLags_.size()-1);
        if(!x.valid()||!y.valid()){fail(!x.valid()?x.failure:y.failure);return;}
        size_t settled=scopeLags_.size()-1;
        while(settled>0){
            const auto priorX=scopeFit(0,settled-1),priorY=scopeFit(1,settled-1);
            if(!priorX.valid()||!priorY.valid()||std::abs(priorX.gain/x.gain-1)>.15||
               std::abs(priorY.gain/y.gain-1)>.15)break;
            --settled;
        }
        responseUs_=scopeLags_[settled]+12000;
        result_={1./x.gain,1./y.gain,initial_.px,initial_.py,responseUs_,kScopedCommandLimit};
        state_=State::Complete;status_="Scope calibration measured and confirmed";
    }
    Command scopeTick(const Input& f){
        if(state_==State::ReadyToSend)return {};
        if(state_==State::Waiting){
            if(!f.idle||f.now-stableAt_<150000)return {};
            if(const int pulse=scopePulses_[step_%scopePulses_.size()]){
                state_=State::ReadyToSend;
                return step_<scopePulses_.size()?Command{pulse,0}:Command{0,pulse};
            }
            sent_=f.camera;sentAt_=f.now;state_=State::Response;
            lastScopeAge_=0;lastScopeAngle_={};
            return {};
        }
        if(f.cameraAt<=sentAt_)return {};
        const auto age=f.cameraAt-sentAt_;
        const auto angle=difference(f.camera,sent_);
        auto& row=observations_[step_];
        for(size_t i=0;i<row.size();++i){
            const auto lag=scopeLags_[i];
            if(row[i].valid||age<lag)continue;
            if(age==lag)row[i]={angle,true};
            else if(lastScopeAge_ && lastScopeAge_<lag && age-lastScopeAge_<=24000){
                const double t=double(lag-lastScopeAge_)/double(age-lastScopeAge_);
                row[i]={{lastScopeAngle_.x+(angle.x-lastScopeAngle_.x)*t,
                    lastScopeAngle_.y+(angle.y-lastScopeAngle_.y)*t},true};
            }else if(age<=lag+12000)row[i]={angle,true};
        }
        lastScopeAge_=age;lastScopeAngle_=angle;
        if(age<190000)return {};
        if(scopePulses_[step_%scopePulses_.size()] && !f.cameraOnly && (!f.idle||f.idleAt<sentAt_)){
            if(age>250000)fail("No idle confirmation for scope pulse");
            return {};
        }
        ++step_;
        if(step_==totalSteps()){finishScope();return {};}
        state_=State::Waiting;stableAt_=f.now;
        status_=step_<scopePulses_.size()?"Measuring and verifying horizontal response":"Measuring and verifying vertical response";
        return {};
    }
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
    unsigned totalSteps()const{return compensateDrift_?unsigned(observations_.size()):unsigned(slopes_.size());}
    bool usesScopeSequence()const{return compensateDrift_;}
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
        compensateDrift_=f.compensateDrift;
        cameraSource_=f.cameraSource;
        session_=f.session;generation_=f.generation;state_=State::Waiting;status_="Keep still - measuring X then Y";return true;
    }
    Command tick(const Input& f){
        if(!running())return {};
        if(f.now<lastNow_||f.now-started_>20000000){fail("Measurement clock/deadline exceeded");return {};}
        if(f.session!=session_||f.generation!=generation_||f.cameraSource!=cameraSource_){fail("Measurement session/camera/connection changed");return {};}
        if(!f.device||f.fault){fail("Device channel unavailable - see device diagnostic");return {};}
        if(f.interrupted){fail("Measurement interrupted by key/button/capture");return {};}
        lastNow_=f.now;
        const bool gap=calibrationSourceLost(f.now,lastCamera_,true);
        const bool canResume=compensateDrift_ && state_==State::Waiting && dayz_camera::fresh(f.cameraAt,f.now,35000);
        if((gap && !canResume)|| (f.cameraAt && f.cameraAt<lastCamera_)){
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
        if(compensateDrift_){
            if(state_==State::Waiting && f.cameraAt-lastCamera_>=35000)stableAt_=f.now;
            lastCamera_=f.cameraAt;
            return scopeTick(f);
        }
        if(f.cameraAt!=lastCamera_){
            if(delta(previous_,f.camera)>.000002){
                stableAt_=f.now;
                if(state_==State::Response && f.cameraAt>sentAt_)
                    responseUs_=std::max(responseUs_,f.cameraAt-sentAt_+8000);
            }
            if(state_==State::Waiting && f.cameraAt-lastCamera_>=35000){
                stableAt_=f.now;
            }
            previous_=f.camera;lastCamera_=f.cameraAt;
        }
        if(state_==State::ReadyToSend)return {}; // One proposal until caller acknowledges it.
        if(state_==State::Waiting){
            if(!f.idle || f.now-stableAt_<150000)return {};
            baseline_=f.camera;state_=State::ReadyToSend;
            const int value=counts_[step_%8];return step_<8?Command{value,0}:Command{0,value};
        }
        if(f.now-sentAt_>600000){fail("No stable camera response; calibration unchanged");return {};}
        if(f.cameraAt<=sentAt_)return {};
        const auto measured=difference(f.camera,sent_);
        const double main=step_<8?measured.x:measured.y,cross=step_<8?measured.y:measured.x;
        constexpr double threshold=.000001;
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
        if(responseUs_>100000){
            fail("Camera response too slow for bounded tracking");return {};
        }
        if(std::abs(main)<threshold){fail("No measurable camera response; no retries sent");return {};}
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
            state_=State::Complete;status_="Calibration measured and confirmed";return {};
        }
        baseline_=previous_=f.camera;stableAt_=f.now;state_=State::Waiting;
        status_=step_<8?"Measuring horizontal response":"Measuring vertical response";return {};
    }
    void submitted(bool accepted,const Input& f){
        if(state_!=State::ReadyToSend)return;
        if(!accepted){fail("Motion not submitted (click/capture/device busy); retry setup");return;}
        sent_=f.camera;sentAt_=stableAt_=f.now;completedAt_=0;state_=State::Response;
        lastScopeAge_=0;lastScopeAngle_={};
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
    void start(const dayz_camera::Cam& c,uint64_t now,uint64_t session,int x,int y,double countsX,double countsY,
               double settleMs,int commandLimit=kLegacyCommandLimit){
        reset();before_=c;sent_=now;session_=session;
        if(!countsX||!countsY||(!x&&!y)||(commandLimit!=kLegacyCommandLimit && commandLimit!=kScopedCommandLimit)||
           x<-commandLimit||x>commandLimit||y<-commandLimit||y>commandLimit){
            fail("Invalid calibrated output command");return;
        }
        minimum_=uint64_t(std::clamp(settleMs,16.,kMaxScopedResponseMs)*1000);pending_=true;
        reason_="Measured output interval pending";
    }
    bool update(const dayz_camera::Cam& c,uint64_t cameraAt,uint64_t now,uint64_t session,bool switchingMeasuredViews=false){
        if(!pending_)return false;
        if(session!=session_)return fail("Output paused: game session changed; release hold key");
        if(now<sent_||now-sent_>250000)return fail("Output paused: update gap; release hold key");
        if(!cameraAt||cameraAt<lastCamera_||now<cameraAt||now-cameraAt>=35000||!dayz_camera::finite(c.fwd))
            return fail("Output paused: camera unavailable; release hold key");
        // A profile switch never reuses this command. Keep the original
        // settling deadline before accepting a new proposal from the new view.
        if(!switchingMeasuredViews && !dayz_camera::sameLens(before_,c))return fail("Output paused: zoom changed; return to calibrated view and release key");
        if(cameraAt==lastCamera_)return false;lastCamera_=cameraAt;
        // The frame used for the next correction must have been captured
        // after the previous command had time to take effect.
        if(cameraAt>=sent_ && cameraAt-sent_>=minimum_){
            pending_=false;reason_="Measured output interval complete";return true;
        }
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
