// Actual receive-parser integration without a serial/socket handle.
#include "../src/trigger.cpp"
#include <cassert>

int main(){
    assert(!trigger::connected());
    trigger::gMotion.begin();assert(trigger::gMotion.queryDue(qpcUs()));
    trigger::gKeyHeld=false;
    const char pending[]="1\r\n";trigger::feedBytes(pending,int(sizeof(pending)-1));
    assert(trigger::gMotion.known && !trigger::gMotion.idle && !trigger::gKeyHeld);
    trigger::gMotion.cancel();trigger::feedBytes(pending,int(sizeof(pending)-1));
    assert(trigger::gKeyHeld); // Old legacy input parsing remains active outside a motion query.
    const char released[]="0\r\n";trigger::feedBytes(released,int(sizeof(released)-1));assert(!trigger::gKeyHeld);
    trigger::gMotion.begin();const auto now=qpcUs();assert(trigger::gMotion.queryDue(now));
    trigger::gMotion.queryDue(now+250001);assert(trigger::gMotion.queryDue(now+250002));
    const char identity[] = ">>> km.MAKCU\r\n";
    trigger::feedBytes(identity,int(sizeof(identity)-1)); // Artificial clock is ahead: rejected as out-of-window.
    assert(trigger::gMotion.waiting && !trigger::gMotion.verified);
    trigger::gMotion.cancel();
    assert(!trigger::connected() && !trigger::live().clicks);
    puts("PASS production input parser: pending replies cannot become key presses; legacy input unchanged; no device opened");
}
