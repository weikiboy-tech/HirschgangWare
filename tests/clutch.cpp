#define NOMINMAX
#include <windows.h>
#include "../src/clutch.h"
#include "../src/trigger.h"
#include <cassert>
#include <cstdio>
#include <string>

dma_keyboard::Sample remote;
int reads=0;
dma_keyboard::Sample sample() { ++reads;return remote; }
// Sleep(9) need not advance coarse GetTickCount64 by the production 8 ms gate.
void waitForInputTick() {const auto before=GetTickCount64();while(GetTickCount64()-before<9)Sleep(1);}
void press(int key, bool down) {
    const auto mask=uint8_t(1u << (2*(key%4)));
    if (down) remote.bits[size_t(key)/4]|=mask;else remote.bits[size_t(key)/4]&=uint8_t(~mask);
}
int main() {
    clutch::State state;
    clutch::Config config;config.enabled=true;config.key=67;
    state.configure(config);remote.pid=123;
    press(67,true);state.update(remote,true);assert(!state.active); // Initially held.
    press(67,false);state.update(remote,true);
    press(67,true);state.update(remote,true);assert(state.active);
    for (int i=0;i<100;++i) state.update(remote,true);
    assert(state.active);
    press(67,false);state.update(remote,true);assert(state.active);
    press(67,true);state.update(remote,true);assert(!state.active);
    press(67,false);state.update(remote,true);
    press(67,true);state.update(remote,true);assert(state.active);
    state.update({},true);assert(!state.active && !state.ready);
    state.update(remote,true);assert(!state.active); // Reconnect, same PID, held key.
    press(67,false);state.update(remote,true);
    press(67,true);state.update(remote,true);assert(state.active);
    remote.pid=124;state.update(remote,true);assert(!state.active);
    config.activation=clutch::Activation::Hold;state.configure(config);
    state.update(remote,true);assert(!state.active);
    press(67,false);state.update(remote,true);
    press(67,true);state.update(remote,true);assert(state.active);
    press(67,false);state.update(remote,true);assert(!state.active);
    state.update(remote,false);assert(!state.ready);
    state.beginBinding();press(68,true);state.update(remote,true);assert(state.binding);
    press(68,false);state.update(remote,true);
    press(68,true);state.update(remote,true);assert(!state.binding && state.boundKey==68);
    assert(state.config().key==67); // Binding result is committed explicitly by application.
    state.boundKey=0;press(68,false);
    state.beginBinding();state.update(remote,true);
    press(69,true);press(70,true);state.update(remote,true);assert(state.binding && !state.boundKey);
    press(70,false);state.update(remote,true);assert(state.binding && !state.boundKey);
    press(69,false);state.update(remote,true);
    press(27,true);state.update(remote,true);assert(!state.binding && !state.boundKey && state.config().key==67);
    press(27,false);state.update(remote,true);
    state.beginBinding();state.update(remote,true,false,true);assert(!state.binding);
    for (int key : {0,1,5,16,17,18,27,35,91,92,119,160,255}) assert(!clutch::bindable(key));
    state.beginBinding();state.update(remote,true,true);assert(!state.binding && !state.active);

    // Actual shared input sampling, disabled trigger, no primary/secondary bind.
    trigger::cfg().enabled=false;trigger::cfg().key=0;trigger::cfg().secondary={};
    trigger::setLocalKeyReader([](int){return false;});
    trigger::setRemoteKeys(sample);trigger::setRemoteInputNeeded(true);
    trigger::pollInputs();assert(reads==1 && trigger::remoteSample().pid==remote.pid);
    state.configure(config);state.update(trigger::remoteSample(),true);
    press(67,true);waitForInputTick();trigger::pollInputs();state.update(trigger::remoteSample(),true);
    assert(state.active && !trigger::live().keyHeld);
    remote={};waitForInputTick();trigger::pollInputs();state.update(trigger::remoteSample(),true);
    assert(!state.active);
    trigger::setRemoteInputNeeded(false);trigger::pollInputs();assert(!trigger::remoteSample().pid);

    trigger::cfg().key=67;trigger::cfg().keySource=trigger::InputSource::MainPc;
    trigger::cfg().secondary={68,0,trigger::InputSource::MainPc};
    trigger::setRemoteInputNeeded(true);remote.pid=125;
    waitForInputTick();trigger::pollInputs();state.configure(config);state.update(trigger::remoteSample(),true);
    press(67,true);press(68,true);waitForInputTick();trigger::pollInputs();state.update(trigger::remoteSample(),true);
    assert(state.active && trigger::live().boundHeld[0] && trigger::live().boundHeld[1]);
    press(67,false);waitForInputTick();trigger::pollInputs();state.update(trigger::remoteSample(),true);
    assert(!state.active && !trigger::live().boundHeld[0] && trigger::live().boundHeld[1]);
    state.beginBinding();trigger::startCapture();
    state.update(trigger::remoteSample(),true,trigger::capturing());assert(!state.binding);
    trigger::stopCapture();remote={};

    // A LAN tap can start and finish before the application samples it.
    trigger::resetRemoteInput();
    remote.pid=1;remote.transport=dma_keyboard::Transport::Lan;
    trigger::startCapture(0);trigger::pollInputs();
    remote.edges['E']={1,1};waitForInputTick();trigger::pollInputs();
    assert(!trigger::capturing() && trigger::binding(0).key=='E');
    assert(trigger::binding(0).source==trigger::InputSource::MainPc && !trigger::live().keyHeld);
    waitForInputTick();trigger::pollInputs(); // Arm the saved binding while released.
    press('E',true);++remote.edges['E'].pressed;waitForInputTick();trigger::pollInputs();
    assert(trigger::live().boundHeld[0]);
    press('E',false);++remote.edges['E'].released;waitForInputTick();trigger::pollInputs();
    assert(!trigger::live().boundHeld[0] && trigger::remoteSample().edges['E'].released==2);
    trigger::resetRemoteInput();remote={};

    // Clutch receives the LAN sender even if the shared Trigger snapshot is PCIe.
    trigger::setRemoteInputNeeded(false);
    const dma_keyboard::Sample pcie{123, {}, dma_keyboard::Transport::Pcie};
    dma_keyboard::Sample lan{1, {}, dma_keyboard::Transport::Lan};
    clutch::State lanClutch;
    lanClutch.configure({true, 'E', clutch::Activation::Toggle});
    lanClutch.update(clutch::mainPcInput(lan, pcie, true), true);
    lan.edges['E'] = {1, 1};
    lanClutch.update(clutch::mainPcInput(lan, pcie, true), true);
    assert(lanClutch.active && lanClutch.lastInput().transport == dma_keyboard::Transport::Lan);
    lanClutch.update(clutch::mainPcInput(lan, {}, false), true);
    assert(lanClutch.active); // Rereading the same input does not toggle again.
    lan.edges['E'] = {2, 2};
    lanClutch.update(clutch::mainPcInput(lan, {}, false), true);
    assert(!lanClutch.active);
    lan.edges['E'] = {3, 3};
    lanClutch.update(clutch::mainPcInput(lan, {}, false), true);
    assert(lanClutch.active);
    lanClutch.update(clutch::mainPcInput({}, lan, true), true);
    assert(!lanClutch.active && !lanClutch.ready);

    // With AllKeys, a held key must not stall the capture of a different key.
    clutch::State lanCapture;
    lanCapture.beginBinding();
    remote=lan;
    press('A',true);
    ++remote.edges['A'].pressed;
    lanCapture.update(remote,true);
    assert(lanCapture.binding && !lanCapture.boundKey);
    ++remote.edges['Y'].pressed;
    ++remote.edges['Y'].released;
    lanCapture.update(remote,true);
    assert(!lanCapture.binding && lanCapture.boundKey=='Y');
    remote={};

    // Real Windows INI round-trip through the production codecs, preserving other keys.
    char dir[MAX_PATH],path[MAX_PATH];GetTempPathA(MAX_PATH,dir);GetTempFileNameA(dir,"clu",0,path);
    WritePrivateProfileStringA("ESP","names","1",path);
    config.realBonesOnly=true;
    clutch::save(config,[&](const char* key,const std::string& value){ assert(WritePrivateProfileStringA("ESP",key,value.c_str(),path)); });
    const auto restored=clutch::load([&](const char* key,const char* fallback){char value[64];GetPrivateProfileStringA("ESP",key,fallback,value,sizeof value,path);return std::string(value);});
    assert(restored.enabled && restored.key==67 && restored.activation==clutch::Activation::Hold && restored.realBonesOnly);
    assert(GetPrivateProfileIntA("ESP","names",0,path)==1);
    WritePrivateProfileStringA("ESP","clutch_hotkey","27",path);
    assert(clutch::load([&](const char* key,const char* fallback){char value[64];GetPrivateProfileStringA("ESP",key,fallback,value,sizeof value,path);return std::string(value);}).key==0);
    DeleteFileA(path);
    clutch::State fresh;fresh.configure(restored);assert(!fresh.active);
    for (bool realOnly : {false,true}) {
        const clutch::Policy p{true,realOnly};
        assert(!p.extras() && p.skeleton(false) && p.skeleton(true));
        for (int kind=0;kind<=4;++kind) for (bool real : {false,true})
            assert(p.entity(kind,real)==(kind==1 && (!realOnly || real)));
    }
    const clutch::Policy normal{};
    assert(normal.extras() && !normal.skeleton(false) && normal.skeleton(true));
    for (int kind=0;kind<=4;++kind) assert(normal.entity(kind,false));
    puts("PASS clutch: toggle/hold, startup/reconnect, safe binding, shared input without trigger, INI and render policy");
}
