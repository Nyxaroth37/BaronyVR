#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
using Uint32=uint32_t;
#include "drink_gesture.inc"
static bool tick(Gesture&g,Uint32 t,float d=.1f,float grip=1,float trigger=0,Uint32 uid=12){return g.sample(t,uid,55,1,2,3,false,d,grip,trigger);}
int main(){
 Gesture g;assert(!tick(g,100));assert(!g.pending); // must first observe release
 assert(!tick(g,110,.1f,0));assert(tick(g,120));assert(g.pending); // immediate press at face, no dwell
 for(int i=0;i<20;i++)assert(!tick(g,130+i*100)); // held grip cannot repeat
 g.pending=false;assert(!tick(g,2200,.1f,0));assert(tick(g,2210)); // release and repress without lowering
 g.reset();tick(g,100,.6f,0);assert(!tick(g,110,.6f));assert(!tick(g,120,.1f)); // pressing away then approaching held does not drink
 g.reset();tick(g,100,.1f,0);assert(!tick(g,110,.1f,1,1));assert(!tick(g,120,.1f,1,0)); // trigger suppresses this press
 g.reset();tick(g,100,.1f,0);assert(!tick(g,300)); // tracking gap requires fresh release
 g.reset();tick(g,100,.1f,0);assert(!tick(g,110,.1f,1,0,99)); // item changed
 g.reset();tick(g,100,.1f,0);assert(tick(g,110));tick(g,120,.1f,0);assert(g.pending); // quick tap survives release for safe application
 g.reset();tick(g,100,.1f,0);assert(!tick(g,110,NAN));assert(!g.pending);
 g.reset();tick(g,0xfffffff0u,.1f,0);assert(tick(g,20)); // tick wrap
 g.reset();tick(g,100,.25f,0);assert(tick(g,110,.25f)); // face tolerance inclusive
 g.reset();tick(g,100,.251f,0);assert(!tick(g,110,.251f));
 std::cout<<"PASS: press-at-face, immediate trigger, no repeat, near-face rearm, held approach exclusion, trigger conflict, item identity, tracking gap, quick tap, tolerance and clock wrap.\n";
}
