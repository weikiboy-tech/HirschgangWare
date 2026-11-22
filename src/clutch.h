#pragma once
#include "keyboard_bitmap.h"
#include <charconv>
#include <string>

namespace clutch {
enum class Activation { Toggle, Hold };
inline bool bindable(int key) {
    // Keyboard only; Escape cancels, End/F8 belong to application controls.
    return key >= 8 && key < 255 && key != 27 && key != 35 && key != 119 &&
        key != 16 && key != 17 && key != 18 && key != 91 && key != 92 &&
        !(key >= 160 && key <= 165);
}
struct Config {
    bool enabled = false;
    int key = 0;
    Activation activation = Activation::Toggle;
    bool realBonesOnly = false;
};
inline Config validated(Config value) {
    if (!bindable(value.key)) value.key = 0;
    if (value.activation != Activation::Hold) value.activation = Activation::Toggle;
    return value;
}
inline dma_keyboard::Sample mainPcInput(const dma_keyboard::Sample& lan,
                                       const dma_keyboard::Sample& shared, bool dma) {
    if (lan.pid) return lan;
    if (dma && shared.transport == dma_keyboard::Transport::Pcie) return shared;
    return {};
}
template<class Read> Config load(Read read) {
    Config config;
    config.enabled=read("clutch_enabled","0")=="1";
    const std::string key=read("clutch_hotkey","0");
    int value=0;
    const auto result=std::from_chars(key.data(),key.data()+key.size(),value);
    if (result.ec==std::errc{} && result.ptr==key.data()+key.size()) config.key=value;
    config.activation=read("clutch_activation","toggle")=="hold" ? Activation::Hold : Activation::Toggle;
    config.realBonesOnly=read("clutch_real_bones_only","0")=="1";
    return validated(config);
}
template<class Write> void save(Config config, Write write) {
    config=validated(config);
    write("clutch_enabled",config.enabled ? "1" : "0");
    write("clutch_hotkey",std::to_string(config.key));
    write("clutch_activation",config.activation==Activation::Hold ? "hold" : "toggle");
    write("clutch_real_bones_only",config.realBonesOnly ? "1" : "0");
}
// Runtime only: never persisted. All calls belong to the UI/render thread.
class State {
    Config config_;
    dma_keyboard::Sample previous_;
    bool armed_ = false, wasDown_ = false, captureArmed_ = false;
public:
    bool active = false, ready = false, binding = false, escapeConsumed = false;
    int boundKey = 0;
    const Config& config() const { return config_; }
    const dma_keyboard::Sample& lastInput() const { return previous_; }
    void reset() { active=false;ready=false;armed_=false;wasDown_=false;previous_={};captureArmed_=false; }
    void configure(Config config) {
        config=validated(config);
        if (config.enabled!=config_.enabled || config.key!=config_.key || config.activation!=config_.activation) {
            reset(); binding=false;
        }
        config_=config;
    }
    void beginBinding() { reset();binding=true;boundKey=0; }
    void cancelBinding() { binding=false;boundKey=0;reset(); }
    bool cancelEscape() {
        if (binding) { cancelBinding();escapeConsumed=true; }
        return escapeConsumed;
    }
    void update(const dma_keyboard::Sample& sample, bool running, bool triggerBinding=false, bool localEscape=false) {
        const bool escape=sample.pid && (dma_keyboard::down(sample.bits,27) ||
            dma_keyboard::edgesSince(previous_,sample,27).pressed);
        if (!escape && !localEscape) escapeConsumed=false;
        if (binding && (escape || localEscape)) { cancelEscape();return; }
        if (triggerBinding) { cancelBinding();return; }
        if (!running || !sample.pid) { reset();return; }
        if (!dma_keyboard::sameSource(previous_,sample)) reset();
        ready=true;
        if (binding) {
            if (!captureArmed_) {
                // Existing held keys are the baseline. Capture the next new press.
                previous_=sample;
                captureArmed_=true;
                return;
            }
            int key=0,count=0;
            for (int k=8;k<255;++k) {
                if (!bindable(k)) continue;
                const auto vk=unsigned(k);
                const auto pressed=sample.transport==dma_keyboard::Transport::Lan
                    ? dma_keyboard::edgesSince(previous_,sample,vk).pressed!=0
                    : dma_keyboard::down(sample.bits,vk) && !dma_keyboard::down(previous_.bits,vk);
                if (pressed) {
                    ++count;key=k;
                }
            }
            previous_=sample;
            if (count==1) {
                boundKey=key;binding=false;active=false;armed_=false;wasDown_=true;
            }
            return;
        }
        if (!config_.enabled || !bindable(config_.key)) { previous_=sample;active=false;armed_=false;return; }
        const bool down=dma_keyboard::down(sample.bits,unsigned(config_.key));
        const auto edges=dma_keyboard::edgesSince(previous_,sample,unsigned(config_.key));
        previous_=sample;
        const bool wasArmed=armed_;
        if (!down || edges.released) armed_=true;
        if (!armed_) { active=false;wasDown_=down;return; }
        if (config_.activation==Activation::Hold) active=down;
        else if (sample.transport==dma_keyboard::Transport::Lan) {
            if ((wasArmed || edges.released) && (edges.pressed & 1u)) active=!active;
        } else if (down && !wasDown_) active=!active;
        wasDown_=down;
    }
};
inline State& runtime() { static State state;return state; }
// Both GPU and GDI use these decisions through drawScene.
struct Policy {
    bool clutch = false, realOnly = false;
    bool extras() const { return !clutch; }
    bool entity(int kind, bool realBones) const { return !clutch || (kind==1 && (!realOnly || realBones)); }
    bool skeleton(bool normal) const { return clutch || normal; }
};
}
