#define NOMINMAX
#include <windows.h>
#include "../src/aim_assist.h"
#include "../src/aim_motion_protocol.h"
#include <cassert>
#include <cstdio>
#include <string>

using namespace aim_assist;
void testPhysics(){
    Ballistics b;b.verified=b.vacuumModel=b.cameraBoreAligned=true;b.speed=100;b.at=1000000;
    for(const Vec3 velocity:{Vec3{},Vec3{10,0,0},Vec3{-10,2,3}})for(const float gravity:{0.f,-10.f}){
        b.gravity={0,gravity,0};b.origin={20,10,0};b.launchVelocity={3,0,0};
        const Vec3 position{20,10,100};const auto hit=intercept(position,velocity,b);
        assert(hit.valid && hit.seconds>0);
        const Vec3 muzzleVelocity=mul(sub(hit.aim,b.origin),1./hit.seconds);
        assert(std::abs(norm(muzzleVelocity)-b.speed)<.01);
        const Vec3 projectile=add(add(b.origin,mul(add(muzzleVelocity,b.launchVelocity),hit.seconds)),mul(b.gravity,.5*hit.seconds*hit.seconds));
        assert(norm(sub(projectile,add(position,mul(velocity,hit.seconds))))<.01);
    }
    b.origin={};b.gravity={};b.launchVelocity={};
    assert(!intercept({0,0,100},{0,0,200},b).valid);
    b.verified=false;assert(!intercept({0,0,100},{},b).valid);
    b.verified=true;b.vacuumModel=false;assert(!intercept({0,0,100},{},b).valid);
    b.vacuumModel=true;b.cameraBoreAligned=false;assert(!intercept({0,0,100},{},b).valid);
    b.cameraBoreAligned=true;b.speed=0;assert(!intercept({0,0,100},{},b).valid);
}
void testMotion(){
    Motion m;
    for(int i=0;i<5;++i)assert(m.observe({float(i*.36),0,100},1000000+i*60000));
    assert(m.valid() && std::abs(m.velocity().x-6)<.001);
    assert(m.observe({1.08f,0,100},1300000));
    assert(!m.valid()); // Reverse invalidates the old positive history immediately.
    assert(m.observe({.72f,0,100},1360000));assert(m.valid() && m.velocity().x<0);
    for(int i=1;i<=4;++i)assert(m.observe({.72f,0,100},1360000+i*60000));
    assert(m.valid() && norm(m.velocity())<.001);
    assert(!m.observe({100,0,100},1660000));assert(!m.valid());
    assert(!m.observe({100,0,100},1600000));
    m.reset();assert(m.observe({0,0,0},1000000));
    assert(m.observe({0,0,0},1000000));assert(!m.valid());
    assert(!m.observe({1,0,0},1000000));
    assert(m.observe({0,0,0},2000000));assert(!m.observe({0,0,0},2300000));
}
void testController(){
    Config c;c.strength=.5;c.smoothMs=100;c.maxSpeed=200;c.maxAccel=400;
    double totals[3]{};int index=0;
    for(const int rate:{60,125,240}){
        Controller controller;double previous=0;
        for(int i=0;i<rate;++i){const auto move=controller.step(1000,0,1./rate,c);totals[index]+=move.x;
            assert(move.y==0 && std::abs(move.x)<=127);assert(controller.speed()<=c.maxSpeed+1e-6);
            assert(std::abs(controller.speed()-previous)<=c.maxAccel/rate+1e-6);previous=controller.speed();}
        assert(totals[index]>0);++index;
    }
    assert(std::abs(totals[0]-totals[2])<12);
    Controller reversed;for(int i=0;i<100;++i)reversed.step(1000,0,.01,c);
    for(int i=0;i<100;++i)assert(reversed.step(-1000,0,.01,c).x<=0);
    Controller fractional;int sum=0;for(int i=0;i<1000;++i)sum+=fractional.step(10,0,.001,c).x;
    assert(sum>0);fractional.reset();assert(fractional.step(0,0,.01,c).x==0);
    assert(fractional.step(100,0,.2,c).x==0);assert(fractional.speed()==0);
    for(const double smoothing:{10.,70.,300.}){
        Config profile;
        profile.smoothMs=smoothing;profile.responseMs=77.371;
        Controller paced;
        const auto first=paced.step(105,0,.08,profile,.1,16);
        assert(first.x>0 && first.x<16);
        for(int i=0;i<8;++i)paced.step(105,0,.08,profile,.1,16);
        assert(paced.step(105,0,.08,profile,.1,16).x>=15);
        assert(paced.step(-2,0,.08,profile,.1,16).x<=0);
    }
}
struct Fixture {
    Config c;
    Frame f;
    std::vector<Candidate> targets;
    Engine engine;
    Fixture(){
        c.enabled=true;c.key=67;c.countsX=c.countsY=1000;c.lensX=1;c.lensY=.5625;c.deadzonePx=0;
        f.session=1;f.now=1000000;f.cameraAt=f.now;f.camera={{1,0,0},{0,1,0},{0,0,1},{0,0,0},1,.5625f};
        f.width=1920;f.height=1080;f.held=f.inputReady=f.deviceReady=true;f.candidates=&targets;
        targets.push_back({7,f.now,f.now,{1,0,100},{1,-1,100},false,true});
    }
    Live tick(uint64_t us=10000){f.now+=us;f.cameraAt=f.now;f.physics.at=f.now;for(auto& p:targets)p.poseAt=p.rootAt=f.now;return engine.update(c,f);}
};
void testEngine(){
    Fixture x;int moves=0;
    for(int i=0;i<40;++i)moves+=x.tick().proposed.x;
    assert(moves>0 && x.engine.live().dryRun && x.engine.live().target==7);
    const auto clocked=x.engine.update(x.c,x.f);assert(!clocked.proposed.x && clocked.active);
    x.targets.push_back({8,x.f.now,x.f.now,{.1f,0,100},{.1f,-1,100},false,true});
    assert(x.tick().target==7); // Do not alternate between adjacent objects.
    x.targets[0].isFriend=true;assert(x.tick().target==8);assert(!x.engine.live().proposed.x);
    x.f.held=false;assert(!x.tick().active && !x.engine.live().proposed.x);
    x.f.held=true;x.f.capturing=true;assert(!x.tick().active);
    x.f.capturing=false;x.f.inputReady=false;assert(!x.tick().active);
    x.f.inputReady=true;x.targets.clear();assert(!x.tick().active);
    Fixture stale;stale.tick();stale.f.now+=80000;stale.f.cameraAt=stale.f.now;
    assert(!stale.engine.update(stale.c,stale.f).active);
    assert(!stale.engine.live().target && std::string(stale.engine.live().status).find("Root data stale")!=std::string::npos);
    Fixture camera;camera.tick();camera.f.now+=40000;
    assert(!camera.engine.update(camera.c,camera.f).active);
    Fixture zoom;zoom.tick();zoom.f.camera.px=.2f;assert(!zoom.tick().active);
    assert(zoom.engine.live().target==7 && !zoom.engine.live().proposed.x);
    Fixture missing;missing.c.countsX=0;assert(!missing.tick().active);
    assert(missing.engine.live().target==7 && missing.engine.live().errorPx>0);
    assert(!missing.engine.live().proposed.x && !missing.engine.live().proposed.y);
    missing.targets.clear();missing.tick();assert(missing.engine.live().target==0);
    Fixture output;output.c.dryRun=false;assert(!output.tick().active);
    assert(output.engine.live().target==7 && !output.engine.live().proposed.x);
    output.c.outputVerified=true;output.f.deviceReady=false;assert(!output.tick().active);
    output.f.deviceReady=true;assert(output.tick().active);
    output.f.outputPaused=true;output.f.outputIssue="Output paused - release key";
    assert(!output.tick().active && output.engine.live().target==7 && output.c.outputVerified);
    output.f.outputPaused=false;assert(output.tick().active); // Transient pause cannot erase the mapping.
    Fixture duplicate;duplicate.targets.push_back(duplicate.targets[0]);assert(!duplicate.tick().active);
    Fixture outside;outside.targets[0].point.x=20;assert(!outside.tick().active);
    assert(!outside.engine.live().target && std::string(outside.engine.live().status).find("Outside search circle")!=std::string::npos);
    Fixture friendly;friendly.targets[0].isFriend=true;assert(!friendly.tick().active);
    assert(!friendly.engine.live().target && std::string(friendly.engine.live().status).find("Friends excluded")!=std::string::npos);
    Fixture basis;basis.f.camera.right=basis.f.camera.fwd;assert(!basis.tick().active);
    Fixture session;for(int i=0;i<30;++i)session.tick();session.f.session=2;
    assert(!session.tick().proposed.x);
    Fixture idle;idle.targets[0].point={0,0,100};idle.c.deadzonePx=1;
    for(int i=0;i<100;++i)assert(!idle.tick().proposed.x);
    Fixture lead;lead.c.prediction=true;
    assert(!lead.tick().active && std::string(lead.engine.live().status).find("ballistics missing")!=std::string::npos);
    assert(lead.engine.live().target==7 && lead.engine.live().errorPx>0);
    assert(!lead.engine.live().proposed.x && !lead.engine.live().proposed.y);
    lead.c.prediction=false;assert(lead.tick().target==7 && lead.engine.live().active);
    lead.c.prediction=true;
    lead.f.physics.verified=lead.f.physics.vacuumModel=lead.f.physics.cameraBoreAligned=true;
    lead.f.physics.speed=100;
    for(int i=0;i<20;++i)lead.tick();
    assert(lead.engine.live().predictionValid);
    const auto flight=lead.engine.live().flightSeconds;
    // Camera movement changes angular error, never the stationary world's velocity.
    const float a=.01f;
    lead.f.camera.right={std::cos(a),0,-std::sin(a)};lead.f.camera.fwd={std::sin(a),0,std::cos(a)};
    lead.tick();assert(lead.engine.live().predictionValid && std::abs(lead.engine.live().flightSeconds-flight)<.001);
    lead.f.physics.verified=false;assert(!lead.tick().active && !lead.engine.live().proposed.x);
    for(const uint64_t dt:{uint64_t(2000),uint64_t(4167),uint64_t(8000),uint64_t(16667)}){
        Fixture fast;fast.c.prediction=true;fast.f.physics.verified=fast.f.physics.vacuumModel=fast.f.physics.cameraBoreAligned=true;fast.f.physics.speed=100;
        for(int i=0;i<150;++i)fast.tick(dt);
        assert(fast.engine.live().predictionValid);
    }
    Fixture disabled;disabled.c.enabled=false;assert(!disabled.tick().active);
    Fixture vertical;vertical.targets[0].point={0,2,100};int up=0;
    for(int i=0;i<40;++i)up+=vertical.tick().proposed.y;
    assert(up<0);
    Fixture rolled;rolled.targets[0].point={0,2,100};
    rolled.f.camera.right={.70710678f,.70710678f,0};rolled.f.camera.up={-.70710678f,.70710678f,0};
    int rollX=0,rollY=0;for(int i=0;i<40;++i){const auto step=rolled.tick();rollX+=step.proposed.x;rollY+=step.proposed.y;}
    assert(rollX==0 && rollY<0);
    Fixture wrap;const double yaw=179.*3.141592653589793/180.;
    wrap.f.camera.fwd={float(std::sin(yaw)),0,float(std::cos(yaw))};wrap.f.camera.right={float(std::cos(yaw)),0,float(-std::sin(yaw))};
    wrap.targets[0].point={float(-std::sin(yaw)*100),0,float(std::cos(yaw)*100)};
    int wrapped=0;for(int i=0;i<40;++i)wrapped+=wrap.tick().proposed.x;assert(wrapped>0);
    Fixture rates;
    for(const uint64_t dt:{uint64_t(4167),uint64_t(8000),uint64_t(16667)}){
        rates.engine.reset();int total=0;
        for(int i=0;i<240;++i)total+=rates.tick(dt).proposed.x;
        assert(total>0);
    }
    Fixture flood;for(int i=0;i<129;++i)flood.targets.push_back(flood.targets.front());assert(!flood.tick().active);
    Fixture busy;for(int i=0;i<20;++i)busy.tick();busy.f.outputBusy=true;
    for(int i=0;i<10;++i)assert(!busy.tick().proposed.x);
    busy.f.outputBusy=false;assert(!busy.tick().proposed.x); // No replay/catch-up after a long actuator stall.
    Fixture rawFrame;rawFrame.f.cameraAt=rawFrame.f.now-32000;
    assert(rawFrame.engine.update(rawFrame.c,rawFrame.f).active); // Display's 31 ms gate does not own control.
    Fixture latency;
    latency.c.responseMs=77.371;
    assert(latency.tick(20000).trackingLeadMs==0);
    for(int i=0;i<6;++i){
        latency.targets[0].root.x+=.1f;latency.targets[0].point.x+=.1f;
        latency.tick(20000);
    }
    assert(latency.engine.live().trackingLeadMs>0 && latency.engine.live().trackingLeadMs<=65);
    latency.targets[0].root.x-=.1f;latency.targets[0].point.x-=.1f;
    assert(latency.tick(20000).trackingLeadMs==0);
    for(int i=0;i<6;++i){
        latency.targets[0].root.x-=.1f;latency.targets[0].point.x-=.1f;
        latency.tick(20000);
    }
    assert(latency.engine.live().trackingLeadMs>0);
    assert(latency.tick(20000).trackingLeadMs==0); // Stop drops lead immediately.
    const auto measuredLead=[](bool enabled,double amount){
        Fixture moving;
        moving.f.directOnly=true;
        moving.c.responseMs=77.371;
        moving.c.motionPrediction=enabled;
        moving.c.predictionAmount=amount;
        for(int i=0;i<8;++i){
            moving.targets[0].root.x+=.1f;moving.targets[0].point.x+=.1f;
            moving.targets[0].velocity={5,0,0};
            moving.targets[0].positionAt=moving.f.now+20000;
            moving.tick(20000);
        }
        return moving.engine.live().trackingLeadMs;
    };
    assert(measuredLead(false,100)==0);
    const auto standard=measuredLead(true,100);
    assert(standard>0 && std::abs(measuredLead(true,50)-standard*.5)<.001);
    assert(measuredLead(true,150)>standard);
    Fixture nearby;
    nearby.f.directOnly=true;nearby.c.responseMs=77.371;
    nearby.targets[0].point={0,0,10};
    nearby.targets[0].hasBones=false;
    assert(!nearby.tick().active);
    nearby.targets[0].hasBones=true;
    nearby.targets[0].velocity={8,0,0};
    nearby.targets[0].positionAt=nearby.f.now;
    const auto nearbyLead=nearby.tick().trackingLeadMs;
    assert(nearbyLead>0 && nearbyLead<standard); // Reacquire immediately and reduce, rather than discard, the lead.
    const auto movement=[&](float speed){
        int counts=0;
        nearby.targets[0].velocity={speed,0,0};
        for(int i=0;i<12;++i){
            nearby.targets[0].positionAt=nearby.f.now;
            counts+=nearby.tick().proposed.x;
        }
        return counts;
    };
    assert(movement(8)>0);
    assert(movement(-8)<0);
    assert(nearby.tick(100000).trackingLeadMs==0);
    latency.f.held=false;
    const auto released=latency.tick();
    assert(!released.active && !released.proposed.x && !released.proposed.y);
}
void testClosedLoop(){
    // Actual observed legacy feedback was ~73 ms. Model a conservative 80 ms
    // one-command cycle; do not let the generic 50 ms discontinuity gate stall it.
    Fixture paced;paced.c.countsX=paced.c.countsY=1097.14;paced.c.outputVerified=true;paced.c.dryRun=false;
    paced.c.deadzonePx=1;paced.targets[0].point={10,0,100};paced.f.commandLimit=16;
    double pacedYaw=0;int pending=0;uint64_t deliveredAt=0;
    for(int i=0;i<250;++i){
        paced.f.outputCycleComplete=false;
        if(pending && paced.f.now+8000>=deliveredAt){pacedYaw+=pending/paced.c.countsX;pending=0;paced.f.outputCycleComplete=true;}
        paced.f.outputBusy=pending!=0;
        paced.f.camera.right={float(std::cos(pacedYaw)),0,float(-std::sin(pacedYaw))};
        paced.f.camera.fwd={float(std::sin(pacedYaw)),0,float(std::cos(pacedYaw))};
        const auto step=paced.tick(8000);
        if(step.proposed.x){assert(!pending && std::abs(step.proposed.x)<=16);pending=step.proposed.x;deliveredAt=paced.f.now+80000;}
    }
    assert(paced.engine.live().errorPx<3);
    for(const int rate:{60,125,240}){
        Fixture x;x.c.deadzonePx=.5;x.c.countsX=5000;x.targets[0].point={10,0,100};
        double yaw=0;
        for(int i=0;i<rate*5;++i){
            if(i==rate)yaw-=.02; // Manual input remains a separate camera change.
            x.f.camera.right={float(std::cos(yaw)),0,float(-std::sin(yaw))};
            x.f.camera.fwd={float(std::sin(yaw)),0,float(std::cos(yaw))};
            const auto live=x.tick(uint64_t(1000000/rate));yaw+=live.proposed.x/x.c.countsX;
        }
        assert(x.engine.live().errorPx<.75);
        const double finalYaw=yaw;
        for(int i=0;i<rate;++i)assert(!x.tick(uint64_t(1000000/rate)).proposed.x);
        assert(yaw==finalYaw); // No residual motion/drift after settling.
        x.f.held=false;assert(!x.tick().active && !x.engine.live().proposed.x);
    }
}
void testKeysAndIni(){
    clutch::State key;key.configure({true,67,clutch::Activation::Hold,false});
    dma_keyboard::Sample sample;sample.pid=1;sample.bits[67/4]|=1u<<(2*(67%4));
    key.update(sample,true);assert(!key.active);
    sample.bits={};key.update(sample,true);sample.bits[67/4]|=1u<<(2*(67%4));key.update(sample,true);assert(key.active);
    key.update({},true);assert(!key.active);key.update(sample,true);assert(!key.active);
    key.beginBinding();sample.bits={};key.update(sample,true);sample.bits[27/4]|=1u<<(2*(27%4));key.update(sample,true);
    assert(!key.binding && key.config().key==67);
    char dir[MAX_PATH],file[MAX_PATH];GetTempPathA(MAX_PATH,dir);GetTempFileNameA(dir,"aim",0,file);
    WritePrivateProfileStringA("HirschgangWare","names","1",file);
    Config c;c.enabled=true;c.key=67;c.countsX=1000;c.countsY=-900;c.lensX=.2;c.lensY=.1125;c.dryRun=false;
    c.motionPrediction=false;c.predictionAmount=75;
    save(c,[&](const char* k,const std::string& v){assert(WritePrivateProfileStringA("HirschgangWare",k,v.c_str(),file));});
    const auto restored=load([&](const char* k,const char* fallback){char text[128];GetPrivateProfileStringA("HirschgangWare",k,fallback,text,sizeof text,file);return std::string(text);});
    assert(restored.enabled && restored.key==67 && restored.countsY==-900 && !restored.dryRun && restored.lensX==.2);
    assert(!restored.motionPrediction && restored.predictionAmount==75);
    assert(GetPrivateProfileIntA("HirschgangWare","names",0,file)==1);
    WritePrivateProfileStringA("HirschgangWare","assist_key","nan",file);
    const auto corrupt=load([&](const char* k,const char* fallback){char text[128];GetPrivateProfileStringA("HirschgangWare",k,fallback,text,sizeof text,file);return std::string(text);});
    assert(corrupt.key==0);DeleteFileA(file);
}
void testSnapshotAdapter(){
    // Same source fields consumed from DzSnap::E in production; no pre-approved candidates.
    struct Entity {int kind=1;uint32_t webTrackId=1;uint64_t jointsUs=1000000,rootUs=1000000;
        Vec3 feet{1,-1,100};bool hasBones=true;Vec3 joints[16]{};std::string name="Enemy";
        Vec3 vel{5,0,0};uint64_t posUs=1000000;};
    Fixture x;x.f.now=x.f.cameraAt=1000000;x.f.directOnly=true;x.c.prediction=true;
    Entity e;e.joints[0]=e.joints[2]={1,0,100};std::vector<Entity> entities{e};
    Diagnostics stats;
    const auto update=[&]{stats=prepare(entities,x.f,x.c,x.engine.live().target,[](const std::string& name){return name=="Friend";},x.targets);
        x.f.diagnostics=&stats;return x.engine.update(x.c,x.f);};
    assert(update().target==1 && x.engine.live().active); // Legacy prediction preference cannot block direct mode.
    x.f.held=false;assert(update().target==1 && !x.engine.live().active); // Continuous preview without E.
    x.f.held=true;x.c.countsX=0;assert(update().target==1 && !x.engine.live().active);
    entities[0].name.clear();assert(update().target==1 && !x.engine.live().identified && !x.engine.live().proposed.x);
    assert(stats.get(Rejection::IdentityPending)==1);
    entities[0].name="Friend";assert(update().target==0 && stats.get(Rejection::Friend)==1);
    entities[0].name="Enemy";entities[0].hasBones=false;assert(!update().target && stats.get(Rejection::NoBones)==1);
    entities[0].hasBones=true;entities[0].jointsUs=900000;assert(!update().target && stats.get(Rejection::StaleBones)==1);
    entities[0].jointsUs=x.f.now;entities[0].rootUs=900000;assert(!update().target && stats.get(Rejection::StaleRoot)==1);
    entities[0].rootUs=x.f.now;entities[0].joints[2]={0,0,388};assert(!update().target && stats.get(Rejection::Range)==1);
    entities[0].joints[2]={20,0,100};assert(!update().target && stats.get(Rejection::Area)==1);
    entities[0].joints[2]={200,0,100};assert(!update().target && stats.get(Rejection::Offscreen)==1);
    entities[0].joints[2]={NAN,0,100};assert(!update().target && stats.get(Rejection::InvalidPoint)==1);
    entities[0].joints[2]={1,0,100};entities[0].webTrackId=0;assert(!update().target && stats.get(Rejection::InvalidId)==1);
    entities.clear();assert(!update().target && stats.players==0); // Death/removal clears lock.
    entities.push_back(e);entities[0].kind=2;assert(!update().target && stats.players==0);
    entities[0]=e;
    // Stable identity is never manufactured from the old occupant/name at a reused slot.
    update();entities[0].webTrackId=2;entities[0].name.clear();assert(update().target==2 && !x.engine.live().identified);
    entities.assign(160,e);for(unsigned i=0;i<entities.size();++i)entities[i].webTrackId=i+1;
    update();assert(x.targets.size()==128 && stats.players==160);
    x.f.session=2;entities.clear();assert(!update().target);
}
void testMotionProtocol(){
    MotionProtocol p;assert(!p.queryDue(1000000));p.begin();assert(p.queryDue(1000000));
    p.reply("km.moving()",1001000);p.reply(">>>",1001000);p.reply("km.MAKCU",1001000);
    assert(p.waiting && !p.known);
    p.reply("1",1002000);assert(p.known && !p.idleFresh(1003000));
    assert(!p.queryDue(1003000));assert(p.queryDue(1008000));p.reply("km.moving(0)",1009000);
    assert(p.idleFresh(1010000));assert(!p.queryDue(1020000));p.moved();assert(!p.idleFresh(1020000));
    assert(p.queryDue(1020000));p.reply("ERR",1021000);assert(!p.fault && p.versionQuery && !p.verified);
    assert(p.queryDue(1030000) && std::string(p.queryCommand())=="km.version()");
    assert(p.reply(">>> km.MAKCU",1031000) && p.cameraOnly && !p.verified && p.idleFresh(1032000));
    p.moved();assert(!p.idleFresh(1033000) && !p.queryDue(1040000));
    p.intervalCompleted(1050000);assert(p.idleFresh(1051000));p.reply("ERR",1052000);assert(p.fault);
    p.begin();assert(p.queryDue(2000000));p.queryDue(2250001);assert(!p.fault && p.versionQuery);
    assert(p.queryDue(2250002));p.queryDue(2500003);assert(p.fault);
    p.begin();p.reply("0",2300000);assert(!p.known); // Unsolicited/late response cannot verify idle.
    assert(p.queryDue(2300000));p.reply("0",2301000);p.verified=true;
    const auto generation=p.generation;p.reset();assert(!p.verified && !p.known && p.generation!=generation);
    p.begin();assert(p.queryDue(3000000));p.reply("7",3001000);assert(p.waiting);
    p.cancel();p.reply("0",3002000);assert(!p.known);
    p.begin();p.cameraOnly=true;p.verified=true;p.pause("Release hold key");
    assert(p.paused && p.verified && !p.fault && !p.queryDue(4000000));
    assert(!p.idleFresh(4000000) && p.resume() && p.verified && !p.paused);
    assert(p.queryDue(4000000) && std::string(p.queryCommand())=="km.version()");
    assert(p.reply("km.MAKCU",4001000) && p.idleFresh(4002000) && p.verified);
    p.fail("Real serial fault");p.pause("Not a real recovery");assert(p.fault && !p.verified && !p.resume());
    assert(!p.beginRestore());
    MotionProtocol restored;
    assert(restored.beginRestore() && !restored.verified);
    assert(!restored.reply("km.MAKCU",1000000));
    assert(restored.queryDue(1000000));
    assert(restored.reply("1",1001000) && !restored.verified);
    assert(restored.queryDue(1008000));
    assert(restored.reply("0",1009000) && restored.verified && !restored.restoring);
    restored.reset();assert(!restored.verified && restored.beginRestore());
    assert(restored.queryDue(2000000));
    restored.queryDue(2250001);
    assert(restored.queryDue(2250002) && !restored.verified);
    assert(restored.reply(">>> km.MAKCU",2251000) && restored.verified && restored.cameraOnly);
    restored.reset();assert(restored.beginRestore());
    assert(restored.queryDue(3000000));restored.queryDue(3250001);
    assert(restored.queryDue(3250002));restored.queryDue(3500003);
    assert(restored.fault && !restored.verified && !restored.beginRestore());
    Fixture released;released.c.outputVerified=true;released.f.held=false;
    p.begin();p.cameraOnly=true;p.verified=true;p.pause("Release hold key");
    assert(mayRearm(released.c,released.f,p,false));
    assert(!mayRearm(released.c,released.f,p,true)); // Initial-down suppression is not a physical release.
    released.f.held=true;assert(!mayRearm(released.c,released.f,p,false));
    released.f.held=false;released.f.inputReady=false;assert(!mayRearm(released.c,released.f,p,false));
    released.f.inputReady=true;released.f.capturing=true;assert(!mayRearm(released.c,released.f,p,false));
    released.f.capturing=false;released.f.camera.py=.2f;assert(!mayRearm(released.c,released.f,p,false));
}
struct CalibrationSimulation {
    Calibration cal;Calibration::Input input;
    double yaw=0,pitch=0,xCounts=1000,yCounts=-900;
    int commands=0,budgetX=0,budgetY=0;uint64_t lastSent=0;
    bool respond=true,pending=false,inconsistent=false;
    CalibrationSimulation(){
        input.now=input.cameraAt=1000000;input.session=1;input.generation=1;
        input.device=input.idle=true;input.idleAt=input.now;
        input.camera={{1,0,0},{0,1,0},{0,0,1},{0,0,0},1,.5625f};
        assert(cal.start(input));
    }
    void tick(){
        input.now+=10000;input.cameraAt=input.now;
        input.idle=!pending;input.idleAt=input.now;
        input.camera.right={float(std::cos(yaw)),0,float(-std::sin(yaw))};
        input.camera.fwd={float(std::sin(yaw)*std::cos(pitch)),float(-std::sin(pitch)),float(std::cos(yaw)*std::cos(pitch))};
        input.camera.up={float(std::sin(yaw)*std::sin(pitch)),float(std::cos(pitch)),float(std::cos(yaw)*std::sin(pitch))};
        const auto previous=cal.progress();const auto command=cal.tick(input);
        if(input.cameraOnly && cal.progress()>previous)pending=false;
        if(command.x||command.y){
            assert(std::abs(command.x)<=16 && std::abs(command.y)<=16 && !(command.x&&command.y));
            ++commands;budgetX+=std::abs(command.x);budgetY+=std::abs(command.y);lastSent=input.now;
            cal.submitted(true,input);
            if(input.cameraOnly)pending=true;
            if(respond){yaw+=command.x/xCounts*(inconsistent&&commands>1?2:1);pitch+=command.y/yCounts;}
        }
    }
    void finish(){for(int i=0;i<2200 && cal.running();++i)tick();assert(!cal.running());}
};
void testCalibration(){
    assert(!calibrationSourceLost(1035000,1000000,true));
    assert(!calibrationSourceLost(1249999,1000000,true));
    assert(calibrationSourceLost(1250000,1000000,true));
    assert(calibrationSourceLost(1000000,1000001,true));
    assert(calibrationSourceLost(1000000,1000000,false));
    CalibrationSimulation success;success.finish();assert(success.cal.complete() && success.commands==16);
    assert(std::abs(success.cal.result().countsX-1000)<.1 && std::abs(success.cal.result().countsY+900)<.1);
    assert(success.budgetX==58 && success.budgetY==58);
    CalibrationSimulation normal;normal.yCounts=1200;normal.finish();assert(normal.cal.complete() && normal.cal.result().countsY>0);
    CalibrationSimulation legacy;legacy.input.cameraOnly=true;legacy.finish();
    assert(legacy.cal.complete() && legacy.commands==16 && legacy.cal.result().responseUs>=16000 && legacy.cal.result().responseUs<=100000);
    CalibrationSimulation paused;paused.input.cameraOnly=true;while(!paused.commands)paused.tick();
    paused.input.now+=40000;paused.finish();assert(paused.cal.complete()); // Render pause is not a session loss.
    CalibrationSimulation gap;
    gap.input.now+=90000;
    assert(!gap.cal.tick(gap.input).x && gap.cal.running());
    for(int i=0;i<14;++i)gap.tick();
    assert(gap.commands==0); // A short gap restarts the quiet period before sending.
    gap.finish();assert(gap.cal.complete() && gap.commands==16);
    CalibrationSimulation delayedRender;delayedRender.input.cameraOnly=true;
    while(!delayedRender.commands)delayedRender.tick();
    delayedRender.input.now=delayedRender.lastSent+108000;
    delayedRender.input.cameraAt=delayedRender.lastSent+80000;
    delayedRender.input.camera.right={float(std::cos(delayedRender.yaw)),0,float(-std::sin(delayedRender.yaw))};
    delayedRender.input.camera.fwd={float(std::sin(delayedRender.yaw)),0,float(std::cos(delayedRender.yaw))};
    delayedRender.cal.tick(delayedRender.input);
    delayedRender.finish();
    assert(delayedRender.cal.complete() && delayedRender.cal.result().responseUs==88000);
    CalibrationSimulation lateCamera;lateCamera.input.cameraOnly=true;
    while(!lateCamera.commands)lateCamera.tick();
    lateCamera.input.now+=100000;
    lateCamera.finish();
    assert(!lateCamera.cal.complete() && lateCamera.commands==1);
    CalibrationSimulation legacySilent;legacySilent.input.cameraOnly=true;legacySilent.respond=false;legacySilent.finish();
    assert(!legacySilent.cal.complete() && legacySilent.commands==1);
    CalibrationSimulation silent;silent.respond=false;silent.finish();assert(!silent.cal.complete() && silent.commands==1);
    CalibrationSimulation slow;while(!slow.commands)slow.tick();slow.pending=true;slow.finish();assert(!slow.cal.complete() && slow.commands==1);
    CalibrationSimulation nonlinear;nonlinear.inconsistent=true;nonlinear.finish();assert(!nonlinear.cal.complete());
    CalibrationSimulation escaped;escaped.tick();escaped.cal.cancel();escaped.finish();assert(escaped.commands==0 && !escaped.cal.complete());
    CalibrationSimulation disconnected;disconnected.input.device=false;disconnected.finish();assert(!disconnected.commands);
    CalibrationSimulation reconnected;reconnected.input.generation=2;reconnected.finish();assert(!reconnected.commands);
    CalibrationSimulation session;session.input.session=2;session.finish();assert(!session.commands);
    CalibrationSimulation zoom;zoom.input.camera.px=.2f;zoom.finish();assert(!zoom.commands);
    CalibrationSimulation invalid;invalid.input.camera.fwd={0,0,0};invalid.cal.tick(invalid.input);
    assert(!invalid.cal.running() && !invalid.commands);
    CalibrationSimulation moved;moved.input.camera.pos.x=1;moved.finish();assert(!moved.commands);
    CalibrationSimulation interrupted;interrupted.input.interrupted=true;interrupted.finish();assert(!interrupted.commands);
    CalibrationSimulation stale;stale.input.now+=250000;
    assert(stale.cal.tick(stale.input).x==0 && !stale.cal.running());
    CalibrationSimulation huge;huge.xCounts=10;huge.finish();assert(!huge.cal.complete() && huge.commands==1);
    CalibrationSimulation rejected;Calibration::Command command;
    for(int i=0;i<20&&!command.x;++i){rejected.input.now+=10000;rejected.input.cameraAt=rejected.input.now;command=rejected.cal.tick(rejected.input);}
    assert(command.x);rejected.cal.submitted(false,rejected.input);assert(!rejected.cal.running());
    const auto previous=success.cal.result();success.input.interrupted=true;assert(!success.cal.start(success.input));
    assert(previous.countsX>0); // A caller only commits result on Complete, never on a failed/restarted attempt.
    OutputCycle ack;auto camera=normal.input.camera;camera.fwd={0,0,1};
    ack.start(camera,1000000,1,10,0,1000,1000,18);
    auto response=camera;response.fwd={float(std::sin(.01)),0,float(std::cos(.01))};
    assert(!ack.update(response,1010000,1010000,1));assert(ack.pending());
    assert(ack.update(response,1020000,1020000,1) && !ack.pending() && !ack.failed());
    ack.start(camera,2000000,1,10,0,1000,1000,18);
    assert(!ack.update(camera,2260000,2260000,1) && ack.failed());
    ack.start(camera,3000000,1,10,0,1000,1000,18);
    assert(!ack.update(response,3010000,3010000,2) && ack.failed());
    ack.start(camera,4000000,1,10,0,1000,1000,18);response.px=.2f;
    assert(!ack.update(response,4010000,4010000,1) && ack.failed());
    assert(!ack.releasedSourceReady(camera,4020000,4020000,1));
    assert(ack.releasedSourceReady(camera,4040000,4040000,1));
    ack.resetReleaseObservation();assert(!ack.releasedSourceReady(camera,4050000,4050000,1));
    // Reproduce the real failure: one count plus manual movement/recoil. Live
    // pacing must finish without treating a mixed camera delta as failed setup.
    ack.start(camera,5000000,1,1,0,1097.14,1097.14,81.386);
    response=camera;response.fwd={.1f,0,.9949874f};
    assert(!ack.update(response,5040000,5040000,1));
    assert(ack.update(response,5082000,5082000,1) && !ack.failed());
    // No unmeasured zero-delay output, even when the camera already moved.
    ack.start(camera,6000000,1,16,0,1097.14,1097.14,81.386);
    assert(!ack.update(response,6008000,6008000,1));
    assert(ack.update(response,6082000,6082000,1));
    ack.start(camera,7000000,1,1,0,1097.14,1097.14,81.386);
    assert(ack.update(camera,7082000,7082000,1)); // Pacing completion does not assert a physical camera ACK.
}
int main(){testPhysics();testMotion();testController();testEngine();testClosedLoop();testKeysAndIni();testSnapshotAdapter();testMotionProtocol();testCalibration();
    puts("PASS aim assist: snapshot adapter, identity/friend filters, projection, bounded following, signed calibration/abort, protocol, INI; no hardware output");}
