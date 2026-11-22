// Headless execution of the actual production panel. ImGui events are synthetic;
// no window, DMA session, serial connection or physical mouse input is opened.
#include "../src/radar.cpp"
#include "../src/imgui/imgui_internal.h"
#include <cassert>
#include <map>

namespace {
std::map<ImGuiID,ImRect> testItemBounds;
std::map<std::string,ImRect> labels;
}
void ImGuiTestEngineHook_ItemAdd(ImGuiContext*,ImGuiID id,const ImRect& bb,const ImGuiLastItemData*){testItemBounds[id]=bb;}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*,ImGuiID id,const char* label,ImGuiItemStatusFlags){if(testItemBounds.count(id))labels[label]=testItemBounds[id];}
void ImGuiTestEngineHook_Log(ImGuiContext*,const char*,...){}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*,ImGuiID){return "test";}

int main(){
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize={1600,2400};io.DeltaTime=1.f/60.f;
    unsigned char* pixels=nullptr;int w=0,h=0;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    GImGui->TestEngineHookItems=true;
    radar::Config::Esp config;bool changed=false;
    const auto frame=[&]{
        labels.clear();testItemBounds.clear();
        ImGui::NewFrame();ImGui::SetNextWindowPos({20,20});ImGui::SetNextWindowSize({390,2100});
        ImGui::Begin("Aim assist panel",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize);
        radar::drawAimAssist(config,changed);ImGui::End();ImGui::Render();
        assert(ImGui::GetDrawData()->Valid);
    };
    const auto click=[&](const char* label){
        assert(labels.count(label));const auto p=labels.at(label).GetCenter();
        io.AddMousePosEvent(p.x,p.y);frame();io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();frame();
    };
    frame();frame();assert(labels.count("Gefuehrt einrichten"));
    assert(labels.count("##prediction-amount"));
    config.assist.motionPrediction=false;
    frame();assert(!labels.count("##prediction-amount"));
    changed=false;
    click("Scoped");
    assert(!changed && labels.count("##prediction-amount"));
    assert(!labels.count("##view-mode") && !labels.count("##calibration-profile"));
    click("##smoothing");click("##strength");click("Head");
    assert(changed && config.assist.scoped.smoothMs!=120 && config.assist.scoped.strength!=.35);
    assert(config.assist.scoped.point==0 && config.assist.point==2);
    assert(config.assist.smoothMs==120 && config.assist.strength==.35);
    click("Hipfire");assert(!labels.count("##prediction-amount"));
    config.assist.motionPrediction=true;frame();
    for(int i=0;i<3;++i){
        click("Gefuehrt einrichten");assert(labels.count("Einrichtung schliessen"));
        assert(labels.count("Messung starten...")); // Disabled while no live/device data exists.
        click("Einrichtung schliessen");assert(labels.count("Gefuehrt einrichten") && !labels.count("Messung starten..."));
    }
    click("Erweiterte Einstellungen");assert(labels.count("Erweitert schliessen"));
    click("Erweitert schliessen");assert(labels.count("Erweiterte Einstellungen"));
    const float setupY=labels.at("Gefuehrt einrichten").Min.y;
    aim_assist::runtime().outputStatus="A long changing diagnostic message must not move the setup controls under the pointer. Device status, targets and connection details can wrap across several lines.";
    frame();assert(std::abs(labels.at("Gefuehrt einrichten").Min.y-setupY)<.01f);
    aim_assist::runtime().outputStatus="no movement pending";
    click("Gefuehrt einrichten");click("Kalibrierung zuruecksetzen");assert(config.assistSetupReset);
    config.assistSetupReset=false;
    auto& runtime=aim_assist::runtime();aim_assist::Calibration::Input input;
    input.device=true;input.now=input.cameraAt=1000000;input.session=1;
    input.camera={{1,0,0},{0,1,0},{0,0,1},{0,0,0},1,.5625f};assert(runtime.calibration.start(input));
    frame();assert(labels.count("Abbrechen (Esc)"));click("Abbrechen (Esc)");assert(config.assistSetupCancel);
    config.assistSetupCancel=false;click("Einrichtung schliessen");assert(config.assistSetupCancel);
    assert(radar::wndProc(nullptr,WM_KEYDOWN,VK_ESCAPE,0)==0 && !runtime.calibration.running());
    assert(!trigger::connected() && !trigger::motionFeedback().verified && !trigger::live().clicks);
    ImGui::DestroyContext();puts("PASS production ImGui panel: reopen/close, disabled setup, reset/cancel requests, Escape; no physical output");
}
