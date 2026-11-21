#pragma once
// Pure, deterministic math/state. No device, DMA, Windows or click operations.
#include "dayz_camera.h"
#include "clutch.h"
#include "aim_calibration.h"
#include "aim_motion_protocol.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <tuple>
#include <vector>

namespace aim_assist {
using Vec3=dayz_camera::Vec3;
inline Vec3 add(Vec3 a,Vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
inline Vec3 sub(Vec3 a,Vec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline Vec3 mul(Vec3 a,double s){return {float(a.x*s),float(a.y*s),float(a.z*s)};}
inline double norm(Vec3 a){return std::sqrt(double(a.x)*a.x+double(a.y)*a.y+double(a.z)*a.z);}
inline bool fresh(uint64_t at,uint64_t now,uint64_t limit){return at && now>=at && now-at<limit;}
struct Config {
    bool enabled=false,prediction=false,dryRun=true,outputVerified=false,showArea=false;
    int key=0,point=2; // Existing joint indexes: 0 head, 2 chest/spine.
    double smoothMs=120,strength=.35,radiusPx=120,maxRange=300;
    double maxSpeed=700,maxAccel=3500,deadzonePx=1;
    double countsX=0,countsY=0,lensX=0,lensY=0;
    double responseMs=16;
    bool motionPrediction=true;
    double predictionAmount=100;
};
inline Config validated(Config c){
    if(!clutch::bindable(c.key))c.key=0;
    if(c.point!=0)c.point=2;
    const auto clean=[](double n,double lo,double hi,double fallback){return std::isfinite(n)?std::clamp(n,lo,hi):fallback;};
    c.smoothMs=clean(c.smoothMs,10,1000,120);c.strength=clean(c.strength,0,1,.35);
    c.radiusPx=clean(c.radiusPx,10,600,120);c.maxRange=clean(c.maxRange,1,1000,300);
    c.maxSpeed=clean(c.maxSpeed,1,2000,700);c.maxAccel=clean(c.maxAccel,1,20000,3500);
    c.deadzonePx=clean(c.deadzonePx,0,20,1);
    c.responseMs=clean(c.responseMs,16,100,16);
    c.predictionAmount=clean(c.predictionAmount,25,150,100);
    for(double* p:{&c.countsX,&c.countsY})if(!std::isfinite(*p)||std::abs(*p)<1||std::abs(*p)>1e7)*p=0;
    for(double* p:{&c.lensX,&c.lensY})if(!std::isfinite(*p)||*p<=0||*p>10)*p=0;
    return c;
}
inline auto values(const Config& c){return std::tie(c.enabled,c.prediction,c.dryRun,c.outputVerified,c.key,c.point,c.smoothMs,c.strength,c.radiusPx,c.maxRange,c.maxSpeed,c.maxAccel,c.deadzonePx,c.countsX,c.countsY,c.lensX,c.lensY,c.responseMs,c.motionPrediction,c.predictionAmount);}
template<class Read> Config load(Read read){
    Config c;
    const auto number=[&](const char* key,double fallback){const std::string text=read(key,std::to_string(fallback).c_str());double v=0;const auto r=std::from_chars(text.data(),text.data()+text.size(),v);return r.ec==std::errc{} && r.ptr==text.data()+text.size() && std::isfinite(v)?v:fallback;};
    c.enabled=read("assist_enabled","0")=="1";c.prediction=read("assist_prediction","0")=="1";
    c.dryRun=read("assist_dry_run","1")!="0";c.outputVerified=read("assist_immediate_verified","0")=="1";
    c.showArea=read("assist_show_area","0")=="1";
    c.responseMs=number("assist_response_ms",16);
    c.motionPrediction=read("assist_motion_prediction","1")=="1";
    c.predictionAmount=number("assist_prediction_amount",100);
    c.key=int(std::clamp(number("assist_key",0),0.,255.));c.point=int(std::clamp(number("assist_point",2),0.,2.));
    c.smoothMs=number("assist_smooth_ms",120);c.strength=number("assist_strength",.35);c.radiusPx=number("assist_radius_px",120);
    c.maxRange=number("assist_range_m",300);c.maxSpeed=number("assist_speed",700);c.maxAccel=number("assist_accel",3500);
    c.deadzonePx=number("assist_deadzone_px",1);c.countsX=number("assist_counts_x",0);c.countsY=number("assist_counts_y",0);
    c.lensX=number("assist_lens_x",0);c.lensY=number("assist_lens_y",0);return validated(c);
}
template<class Write> void save(Config c,Write write){
    c=validated(c);
    write("assist_enabled",c.enabled?"1":"0");write("assist_prediction",c.prediction?"1":"0");
    write("assist_dry_run",c.dryRun?"1":"0");write("assist_immediate_verified",c.outputVerified?"1":"0");
    write("assist_show_area",c.showArea?"1":"0");
    write("assist_response_ms",std::to_string(c.responseMs));
    write("assist_motion_prediction",c.motionPrediction?"1":"0");
    write("assist_prediction_amount",std::to_string(c.predictionAmount));
    write("assist_key",std::to_string(c.key));write("assist_point",std::to_string(c.point));
    for(const auto& entry: {std::pair<const char*,double>{"assist_smooth_ms",c.smoothMs},{"assist_strength",c.strength},
        {"assist_radius_px",c.radiusPx},{"assist_range_m",c.maxRange},{"assist_speed",c.maxSpeed},{"assist_accel",c.maxAccel},
        {"assist_deadzone_px",c.deadzonePx},{"assist_counts_x",c.countsX},{"assist_counts_y",c.countsY},{"assist_lens_x",c.lensX},{"assist_lens_y",c.lensY}})
        write(entry.first,std::to_string(entry.second));
}
// Optional verified provider boundary. Current DayZ reader supplies none of this:
// no camera-as-muzzle substitution, invented speed/gravity or assumed drag model.
struct Ballistics {
    bool verified=false,vacuumModel=false,cameraBoreAligned=false;
    uint64_t at=0;
    Vec3 origin{},launchVelocity{},gravity{};
    double speed=0,maxFlightSeconds=8;
};
struct Intercept {bool valid=false;Vec3 aim{};double seconds=0;};
inline Intercept intercept(Vec3 position,Vec3 velocity,const Ballistics& b){
    if(!b.verified||!b.vacuumModel||!b.cameraBoreAligned||!dayz_camera::finite(position)||!dayz_camera::finite(velocity)||
        !dayz_camera::finite(b.origin)||!dayz_camera::finite(b.launchVelocity)||!dayz_camera::finite(b.gravity)||
        !std::isfinite(b.speed)||b.speed<=0||!std::isfinite(b.maxFlightSeconds)||b.maxFlightSeconds<=0||b.maxFlightSeconds>30)return {};
    const Vec3 p=sub(position,b.origin),v=sub(velocity,b.launchVelocity);
    if(norm(p)<.01)return {};
    const auto required=[&](double t){return sub(add(p,mul(v,t)),mul(b.gravity,.5*t*t));};
    const auto f=[&](double t){const double length=norm(required(t));return length-b.speed*t;};
    // Earliest bracketed (low-time) root. A nonbracketed/tangent solution is
    // conservatively rejected. Fixed work budget, never an unbounded search.
    double lo=0,fl=f(0);
    for(int n=1;n<=256;++n){
        double hi=b.maxFlightSeconds*n/256.,fh=f(hi);
        if(!std::isfinite(fh))return {};
        if(fl>=0 && fh<=0){
            for(int k=0;k<48;++k){const double mid=(lo+hi)*.5;if(f(mid)>0)lo=mid;else hi=mid;}
            const double t=(lo+hi)*.5;const Vec3 r=required(t);
            if(std::abs(norm(r)-b.speed*t)>.01)return {};
            return {true,add(b.origin,r),t};
        }
        lo=hi;fl=fh;
    }
    return {};
}
class Motion {
    struct Sample{Vec3 p{};uint64_t at=0;};
    std::array<Sample,8> samples_{};size_t size_=0;
    Vec3 velocity_{};bool valid_=false;
public:
    void reset(){size_=0;velocity_={};valid_=false;}
    bool valid()const{return valid_;} Vec3 velocity()const{return velocity_;}
    // false marks a discontinuity, not a zero-velocity measurement.
    bool observe(Vec3 p,uint64_t at){
        if(!at||!dayz_camera::finite(p)){reset();return false;}
        if(size_ && at==samples_[size_-1].at){if(norm(sub(p,samples_[size_-1].p))>.001){reset();return false;}return true;}
        if(size_){
            const auto previous=samples_[size_-1];
            if(at<previous.at||at-previous.at>250000||norm(sub(p,previous.p))>20.*double(at-previous.at)/1e6+.5){reset();samples_[0]={p,at};size_=1;return false;}
            const double dt=double(at-previous.at)/1e6;
            if(dt<.01)return true; // Time-spaced history, not eight render frames.
            const Vec3 instant=mul(sub(p,previous.p),1./dt);
            const double speed=norm(instant), priorSpeed=norm(velocity_);
            if(valid_ && priorSpeed>.5 && (speed<priorSpeed*.35 ||
                dayz_camera::dot(instant,velocity_)<.5*speed*priorSpeed)){
                samples_[0]=previous;size_=1;valid_=false;
            }
        }
        if(size_==samples_.size()){std::move(samples_.begin()+1,samples_.end(),samples_.begin());--size_;}
        samples_[size_++]={p,at};
        size_t first=size_-1;
        while(first && at-samples_[first-1].at<=120000)--first;
        const double span=double(at-samples_[first].at)/1e6;
        valid_=size_-first>=3 && span>=.05;
        if(valid_){velocity_=mul(sub(p,samples_[first].p),1./span);if(norm(velocity_)>20){reset();return false;}}
        return true;
    }
};
struct Move {int x=0,y=0;};
class Controller {
    double vx_=0,vy_=0,rx_=0,ry_=0;
public:
    void reset(){vx_=vy_=rx_=ry_=0;}
    double speed()const{return std::hypot(vx_,vy_);}
    Move step(double ex,double ey,double dt,const Config& c,double maxDt=.05,int commandLimit=127){
        maxDt=std::isfinite(maxDt)?std::clamp(maxDt,.05,.1):.05;commandLimit=std::clamp(commandLimit,1,127);
        if(!std::isfinite(ex)||!std::isfinite(ey)||!std::isfinite(dt)||dt<=0||dt>maxDt){reset();return {};}
        if(c.strength<=0){reset();return {};}
        const double distance=std::hypot(ex,ey);
        if(distance==0){reset();return {};}
        // Smoothing shapes the velocity transition. Tracking gain is independent.
        const double transition=.018+.0002*c.smoothMs;
        const double outputSpeed=commandLimit<127
            ? commandLimit/std::max(dt,c.responseMs*.001)
            : c.maxSpeed;
        const double targetSpeed=std::min({distance*c.strength/.10,c.maxSpeed,
            outputSpeed,std::sqrt(2*c.maxAccel*distance)});
        const double tx=ex/distance*targetSpeed,ty=ey/distance*targetSpeed;
        // Integrate the transition over the full interval instead of applying
        // the final velocity retroactively to an entire device cycle.
        const double alpha=-std::expm1(-dt/transition);
        double dx=(tx-vx_)*alpha,dy=(ty-vy_)*alpha;
        const double change=std::hypot(dx,dy),limit=c.maxAccel*dt;
        const bool accelLimited=change>limit;
        if(accelLimited){dx*=limit/change;dy*=limit/change;}
        const double average=accelLimited?.5:std::clamp(1./alpha-transition/dt,0.,1.);
        double mx=(vx_+dx*average)*dt,my=(vy_+dy*average)*dt;
        vx_+=dx;vy_+=dy;
        // Do not release an old fractional residual in a newly reversed direction.
        if(rx_*ex<0)rx_=0;if(ry_*ey<0)ry_=0;
        // Manual motion can overtake the current aim point. Never submit an
        // old opposite-direction displacement while the bounded state brakes.
        if(mx*ex<=0){mx=0;rx_=0;}if(my*ey<=0){my=0;ry_=0;}
        if(std::abs(mx)>std::abs(ex) && mx*ex>0)mx=ex;
        if(std::abs(my)>std::abs(ey) && my*ey>0)my=ey;
        rx_+=mx;ry_+=my;
        const double ix=std::trunc(rx_),iy=std::trunc(ry_);
        Move out{int(std::clamp(ix,-double(commandLimit),double(commandLimit))),int(std::clamp(iy,-double(commandLimit),double(commandLimit)))};
        // Keep only sub-count fractions, never unsent whole-command backlog.
        if(std::abs(ix)>commandLimit)rx_=out.x+(rx_-ix);
        if(std::abs(iy)>commandLimit)ry_=out.y+(ry_-iy);
        if(out.x*ex>0 && std::abs(out.x)>std::abs(ex)){out.x=0;rx_=0;}
        if(out.y*ey>0 && std::abs(out.y)>std::abs(ey)){out.y=0;ry_=0;}
        rx_-=out.x;ry_-=out.y;return out;
    }
};
struct Candidate {
    uint64_t id=0,poseAt=0,rootAt=0;
    Vec3 point{},root{};
    bool isFriend=false,visible=false;
    bool hasBones=true,identified=true;
    Vec3 velocity{};
    uint64_t positionAt=0;
};
enum class Rejection : unsigned { Ready, IdentityPending, Friend, NoBones, InvalidId,
    StaleRoot, StaleBones, InvalidPoint, Offscreen, Range, Area, Count };
inline const char* reasonText(Rejection r){
    static constexpr const char* names[]={"TARGET ACQUIRED","TARGET DETECTED - identity pending",
        "Friends excluded","Bones missing","Stable ID missing","Root data stale","Bone data stale",
        "Invalid bone position","Outside screen","Outside range","Outside search circle"};
    return unsigned(r)<unsigned(Rejection::Count)?names[unsigned(r)]:"Invalid target";
}
struct Diagnostics {
    std::array<unsigned,unsigned(Rejection::Count)> counts{};
    unsigned players=0;
    unsigned get(Rejection r)const{return counts[unsigned(r)];}
    const char* missingReason()const{
        if(!players)return "NO TARGET - no human players in snapshot";
        for(const auto r:{Rejection::Area,Rejection::Range,Rejection::Offscreen,Rejection::IdentityPending,
            Rejection::StaleBones,Rejection::StaleRoot,Rejection::NoBones,Rejection::Friend,Rejection::InvalidId,Rejection::InvalidPoint})
            if(get(r))return reasonText(r);
        return "NO TARGET - no usable player pose";
    }
};
struct Frame {
    uint64_t now=0,session=0,cameraAt=0;
    dayz_camera::Cam camera{};
    double width=0,height=0;
    bool held=false,inputReady=false,capturing=false,deviceReady=false;
    const std::vector<Candidate>* candidates=nullptr;
    Ballistics physics;
    const Diagnostics* diagnostics=nullptr;
    bool directOnly=false;
    bool outputBusy=false;
    bool outputCycleComplete=false,outputPaused=false;
    const char* outputIssue=nullptr;
    int commandLimit=127;
};
inline bool mayRearm(const Config& c,const Frame& f,const MotionProtocol& p,bool physicallyHeld){
    return c.enabled&&c.outputVerified&&c.countsX&&c.countsY&&c.lensX&&c.lensY &&
        p.cameraOnly&&p.paused&&p.verified&&!p.fault && f.session && f.inputReady&&!f.held&&!physicallyHeld&&!f.capturing &&
        fresh(f.cameraAt,f.now,35000)&&dayz_camera::validLens(f.camera)&&dayz_camera::finite(f.camera.fwd)&&
        std::abs(dayz_camera::dot(f.camera.fwd,f.camera.fwd)-1)<=.02 &&
        std::abs(f.camera.px/c.lensX-1)<=.001&&std::abs(f.camera.py/c.lensY-1)<=.001;
}
struct Projection { double x=0,y=0,pixels=0,range=0,yaw=0,pitch=0; };
inline Rejection project(const Frame& f,const Config& c,Vec3 p,Projection& out){
    if(!dayz_camera::finite(p)||!dayz_camera::validLens(f.camera)||f.width<=0||f.height<=0)return Rejection::InvalidPoint;
    const auto d=sub(p,f.camera.pos);out.range=norm(d);
    if(!std::isfinite(out.range))return Rejection::InvalidPoint;
    float sx=0,sy=0;
    if(!dayz_camera::toScreen(f.camera,p,float(f.width),float(f.height),sx,sy))return Rejection::Offscreen;
    out.x=sx;out.y=sy;out.pixels=std::hypot(out.x-f.width*.5,out.y-f.height*.5);
    if(out.x<0||out.y<0||out.x>=f.width||out.y>=f.height)return Rejection::Offscreen;
    if(out.range>c.maxRange)return Rejection::Range;
    if(out.pixels>c.radiusPx)return Rejection::Area;
    if(std::hypot(f.camera.fwd.x,f.camera.fwd.z)<.01)return Rejection::InvalidPoint;
    const double yaw=std::atan2(d.x,d.z)-std::atan2(f.camera.fwd.x,f.camera.fwd.z);
    out.yaw=std::atan2(std::sin(yaw),std::cos(yaw));
    out.pitch=-std::atan2(d.y,std::hypot(d.x,d.z))+std::atan2(f.camera.fwd.y,std::hypot(f.camera.fwd.x,f.camera.fwd.z));
    return std::isfinite(out.yaw)&&std::isfinite(out.pitch)?Rejection::Ready:Rejection::InvalidPoint;
}
inline Rejection inspect(const Candidate& p,const Frame& f,const Config& c,Projection& out){
    if(!p.id)return Rejection::InvalidId;
    if(p.isFriend)return Rejection::Friend;
    if(!p.hasBones)return Rejection::NoBones;
    if(!fresh(p.rootAt,f.now,75000)||!dayz_camera::finite(p.root))return Rejection::StaleRoot;
    if(!fresh(p.poseAt,f.now,75000))return Rejection::StaleBones;
    if(!p.visible)return Rejection::Offscreen;
    const auto result=project(f,c,p.point,out);
    return result==Rejection::Ready && !p.identified?Rejection::IdentityPending:result;
}
// Uses the actual snapshot entity fields. Tests feed the same field layout,
// including missing names/joints/timestamps, rather than pre-approved candidates.
template<class Entities,class IsFriend>
Diagnostics prepare(const Entities& entities,const Frame& f,const Config& c,uint64_t locked,
                    IsFriend isFriend,std::vector<Candidate>& result){
    Diagnostics stats;result.clear();if(result.capacity()<128)result.reserve(128);
    for(const auto& e:entities){
        if(e.kind!=1)continue;
        ++stats.players;
        Candidate candidate{e.webTrackId,e.jointsUs,e.rootUs,e.hasBones?e.joints[c.point]:Vec3{},e.feet,
            !e.name.empty()&&isFriend(e.name),true,e.hasBones,!e.name.empty(),e.vel,e.posUs};
        Projection position;const auto reason=inspect(candidate,f,c,position);++stats.counts[unsigned(reason)];
        if(reason!=Rejection::Ready&&reason!=Rejection::IdentityPending)continue;
        if(result.size()<128){result.push_back(candidate);continue;}
        // Retain the lock and nearest identified candidates if the bounded set fills.
        const auto score=[&](const Candidate& p){Projection q;project(f,c,p.point,q);
            return p.id==locked?-1.:q.pixels+(p.identified?0.:10000.);};
        const auto worst=std::max_element(result.begin(),result.end(),[&](const Candidate& a,const Candidate& b){return score(a)<score(b);});
        if(score(candidate)<score(*worst))*worst=candidate;
    }
    return stats;
}
struct Live {
    const char* status="OFF";
    bool active=false,predictionValid=false,dryRun=true;
    uint64_t target=0;
    double errorPx=0,flightSeconds=0;
    Move proposed{};
    Diagnostics diagnostics;
    bool identified=false;
    double range=0;
    double trackingLeadMs=0;
};
class Engine {
    Config previous_{};uint64_t session_=0,target_=0,last_=0;
    Motion motion_;Controller controller_;
    Live live_;
    bool wasBusy_=false,inDeadzone_=false;
    Live stop(const char* why,bool history=true){
        controller_.reset();last_=0;
        wasBusy_=false;
        inDeadzone_=false;
        if(history){target_=0;motion_.reset();}
        const auto stats=live_.diagnostics;
        if(history)live_={};
        live_.diagnostics=stats;
        live_.active=false;live_.proposed={};
        live_.status=why;live_.dryRun=previous_.dryRun;return live_;
    }
public:
    const Live& live()const{return live_;}
    void suspend(const char* why){stop(why);}
    void reset(){stop("IDLE");session_=0;}
    Live update(Config config,const Frame& f){
        config=validated(config);
        if(values(config)!=values(previous_)){reset();previous_=config;}
        live_.diagnostics=f.diagnostics?*f.diagnostics:Diagnostics{};
        if(!config.enabled)return stop("OFF");
        if(f.capturing)return stop("BINDING");
        if(!f.session||!fresh(f.cameraAt,f.now,35000)||!dayz_camera::validLens(f.camera)||!dayz_camera::finite(f.camera.pos)||f.width<=0||f.height<=0)return stop("CAMERA INVALID/STALE");
        if(session_!=f.session){reset();session_=f.session;}
        if(!f.candidates || f.candidates->size()>128)return stop("INVALID TARGET SNAPSHOT");
        for(const auto axis:{f.camera.right,f.camera.up,f.camera.fwd})
            if(!dayz_camera::finite(axis)||std::abs(dayz_camera::dot(axis,axis)-1)>.02)return stop("CAMERA BASIS INVALID");
        if(std::abs(dayz_camera::dot(f.camera.right,f.camera.up))>.02 ||
           std::abs(dayz_camera::dot(f.camera.right,f.camera.fwd))>.02 ||
           std::abs(dayz_camera::dot(f.camera.up,f.camera.fwd))>.02)return stop("CAMERA BASIS INVALID");
        const Candidate* selected=nullptr;double best=INFINITY;Projection selectedPosition;
        Diagnostics stats=f.diagnostics?*f.diagnostics:Diagnostics{};
        if(f.candidates) for(const auto& p:*f.candidates){
            Projection position;const auto why=inspect(p,f,config,position);
            if(!f.diagnostics){++stats.players;++stats.counts[unsigned(why)];}
            if(why!=Rejection::Ready&&why!=Rejection::IdentityPending)continue;
            const double score=position.pixels+(p.identified?0.:10000.);
            if(p.id==target_&&p.identified&&f.held){selected=&p;selectedPosition=position;best=-1.;continue;}
            if(score<best){selected=&p;best=score;selectedPosition=position;}
        }
        live_.diagnostics=stats;
        if(!selected)return stop(stats.missingReason());
        if(std::count_if(f.candidates->begin(),f.candidates->end(),[&](const Candidate& p){return p.id==selected->id;})!=1)return stop("AMBIGUOUS TARGET ID");
        if(selected->id!=target_){controller_.reset();motion_.reset();last_=0;wasBusy_=inDeadzone_=false;target_=selected->id;}
        const auto previousLive=live_;
        live_={};live_.target=target_;live_.dryRun=config.dryRun;live_.diagnostics=stats;
        live_.identified=selected->identified;live_.errorPx=selectedPosition.pixels;live_.range=selectedPosition.range;
        if(!selected->identified)return stop("TARGET DETECTED - identity pending; movement blocked",false);
        if(!f.inputReady){motion_.reset();return stop("NO INPUT - target acquired",false);}
        if(!f.held){motion_.reset();return stop("READY - target acquired; hold activation key",false);}
        if(!f.directOnly && !motion_.observe(selected->root,selected->rootAt))return stop("POSITION DISCONTINUITY");
        Vec3 aim=selected->point;bool predicted=false;double flight=0;
        if(config.prediction&&!f.directOnly){
            if(!fresh(f.physics.at,f.now,75000)||!f.physics.verified)return stop("PREDICTION BLOCKED - verified ballistics missing",false);
            if(!motion_.valid())return stop("PREDICTION WARMING - velocity",false);
            aim=add(aim,mul(motion_.velocity(),double(f.now-selected->poseAt)/1e6));
            const auto solution=intercept(aim,motion_.velocity(),f.physics);
            if(!solution.valid)return stop("PREDICTION BLOCKED - no verified solution",false);
            aim=solution.aim;flight=solution.seconds;predicted=true;
        }
        Projection projection=selectedPosition;
        if(predicted && project(f,config,aim,projection)!=Rejection::Ready)return stop("AIM POINT OUTSIDE ACQUISITION AREA");
        const bool freshVelocity=f.directOnly?fresh(selected->positionAt,f.now,100000):motion_.valid();
        const Vec3 velocity=f.directOnly?selected->velocity:motion_.velocity();
        const double speed=norm(velocity);
        if(!predicted && config.motionPrediction && freshVelocity && speed>=.5 && speed<=15.){
            const double sampleAgeMs=double(f.now-selected->poseAt)/1000.;
            double horizonMs=std::min(65.,(sampleAgeMs+.5*config.responseMs)*config.predictionAmount/100.);
            // Reduce the lead when it would exceed the screen-space limit or
            // acquisition area. A fast nearby target can still get some lead.
            for(int attempt=0;attempt<5 && horizonMs>=1.;++attempt){
                Projection estimated;
                if(project(f,config,add(selected->point,mul(velocity,horizonMs*.001)),estimated)==Rejection::Ready){
                    const double pixels=std::hypot(estimated.x-selectedPosition.x,estimated.y-selectedPosition.y);
                    if(pixels<=12.){
                        projection=estimated;
                        live_.trackingLeadMs=horizonMs;
                        break;
                    }
                    horizonMs*=std::min(.9,12./pixels);
                }else{
                    horizonMs*=.5;
                }
            }
        }
        const double yaw=projection.yaw,pitch=projection.pitch,pixels=projection.pixels;
        live_.predictionValid=predicted;live_.flightSeconds=flight;
        if(!config.countsX||!config.countsY||!config.lensX||!config.lensY)return stop("CALIBRATION REQUIRED",false);
        if(std::abs(f.camera.px/config.lensX-1)>.001||std::abs(f.camera.py/config.lensY-1)>.001)return stop("ZOOM CHANGED - recalibrate",false);
        if(!config.dryRun && f.outputPaused)return stop(f.outputIssue?f.outputIssue:"OUTPUT PAUSED - release hold key",false);
        if(!config.dryRun && (!f.deviceReady||!config.outputVerified))return stop(f.outputIssue?f.outputIssue:"OUTPUT BLOCKED - device verification required",false);
        inDeadzone_=pixels<=config.deadzonePx*(inDeadzone_?1.35:1.);
        if(inDeadzone_){controller_.reset();last_=f.now;wasBusy_=false;live_.status="IN DEADZONE";return live_;}
        if(f.outputBusy){wasBusy_=true;live_.active=true;live_.status="WAITING FOR OUTPUT INTERVAL / DEVICE";return live_;}
        if(last_ && f.now<last_)return stop("CLOCK DISCONTINUITY");
        const double dt=last_?double(f.now-last_)/1e6:0;
        const bool completedCycle=wasBusy_ && f.outputCycleComplete;wasBusy_=false;
        if(last_ && dt<.008){live_.active=previousLive.active;live_.status=previousLive.status;return live_;} // Never resubmit a previous movement.
        last_=f.now;
        live_.proposed=controller_.step(yaw*config.countsX,pitch*config.countsY,dt,config,completedCycle?.1:.05,f.commandLimit);
        live_.active=true;live_.status=config.dryRun?"DRY RUN - movement not sent":predicted?"TRACKING - verified prediction":
            live_.trackingLeadMs>0?"TRACKING - latency compensation":"TRACKING - direct";
        return live_;
    }
};
struct Runtime {
    clutch::State hotkey;Engine engine;
    Calibration calibration;
    OutputCycle outputCycle;
    bool calibrationStart=false,calibrationApplied=false;
    dayz_camera::Cam camera{};uint64_t cameraAt=0;
    const char* outputStatus="no movement sent";
    Diagnostics diagnostics;
};
inline Runtime& runtime(){static Runtime state;return state;}
}
