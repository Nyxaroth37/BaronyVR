#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
using Uint32=uint32_t;
struct VrGripVector3{float x,y,z;};struct XrQuaternionf{float x=0,y=0,z=0,w=1;};struct XrVector3f{float x=0,y=0,z=0;};
struct Pose{XrQuaternionf orientation;XrVector3f position;};struct View{Pose pose;};View g_xrViews[2];
struct Hand{bool gripPoseValid=true;Pose gripPose;float squeeze=0,trigger=0;};Hand g_xrControllerHands[2];
struct Entity{Uint32 getUID(){return 55;}bool isMobile(){return true;}}actor;
struct Player{Entity*entity=&actor;bool shootmode=true;}p;Player*players[]{&p};
enum{POTION,OTHER,POTION_EMPTY=8,BROKEN=0};struct Item{Uint32 uid=12;int type=1,count=2,status=1;};
struct Stat{Item*weapon=nullptr;}s;Stat*stats[]{&s};
int itemCategory(Item*){return POTION;}enum {SINGLE=0}; int multiplayer=SINGLE;bool gamePaused=false,g_vrArmCalibrationActive=false;const int MAXPLAYERS=1;int clientnum=0,currentlevel=3;bool secretlevel=false;
bool allowed=true,g_xrControllerActionsAttached=true,g_xrVrBackpackInZone=false,g_xrVrBackpackGripConsumed=false,g_vrDrinkOwnsRightGrip=false;
namespace VrWorldGrab{static bool available(){return allowed;}}
bool vrWorldGrabOwnsGrip(int){return false;}
Uint32 now=100;Uint32 SDL_GetTicks(){return now;}void printlog(const char*,...){}
int calls=0;void useItem(Item*i,int){++calls;if(--i->count==0){delete i;s.weapon=nullptr;}}
static VrGripVector3 vrGripRotateXR(const XrQuaternionf& q, const VrGripVector3 v)
{
    // Quaternion vector rotation: v' = v + 2*w*(q.xyz x v)
    //                                   + 2*(q.xyz x (q.xyz x v)).
    const VrGripVector3 cross1{
        q.y * v.z - q.z * v.y,
        q.z * v.x - q.x * v.z,
        q.x * v.y - q.y * v.x
    };
    const VrGripVector3 cross2{
        q.y * cross1.z - q.z * cross1.y,
        q.z * cross1.x - q.x * cross1.z,
        q.x * cross1.y - q.y * cross1.x
    };
    return {
        v.x + 2.f * (q.w * cross1.x + cross2.x),
        v.y + 2.f * (q.w * cross1.y + cross2.y),
        v.z + 2.f * (q.w * cross1.z + cross2.z)
    };
}

// GA33: deliberate, controller-based drinking pilot. Distances are raw XR metres,
// shared by head and grip; neither world scale nor room/seated camera offsets enter.
namespace VrPotionDrink {
struct Gesture {
    Uint32 uid=0,owner=0,last=0,queuedAt=0;
    int type=-1,count=0,level=-1;bool secret=false;
    bool armed=false,latched=false,pending=false;
    void reset(){*this=Gesture{};}
    bool sample(Uint32 now,Uint32 item,Uint32 actor,int itemType,int itemCount,
        int mapLevel,bool mapSecret,float distance,float grip,float trigger) {
        if(uid!=item || owner!=actor || type!=itemType || level!=mapLevel || secret!=mapSecret) {
            reset();uid=item;owner=actor;type=itemType;level=mapLevel;secret=mapSecret;
        }
        if(last && now-last>150u){armed=false;pending=false;}
        last=now;
        if(!std::isfinite(distance)||!std::isfinite(grip)||!std::isfinite(trigger)) {
            reset();return false;
        }
        if(pending && now-queuedAt>150u)pending=false;
        // Observe a released grip even at the face. A grip already held while
        // approaching cannot drink; the press itself must occur in the zone.
        if(grip<.35f){armed=true;latched=false;return false;}
        if(grip<.65f || !armed)return false;
        armed=false;latched=true; // consume every press, including out of range
        if(distance>.25f || trigger>=.15f || pending)return false;
        pending=true;queuedAt=now;count=itemCount;
        return true;
    }
};
static Gesture state;
static bool context() {
    return VrWorldGrab::available() && g_xrControllerActionsAttached &&
        g_xrControllerHands[1].gripPoseValid && !vrWorldGrabOwnsGrip(1) &&
        !g_xrVrBackpackInZone && !g_xrVrBackpackGripConsumed &&
        players[clientnum]->entity->isMobile();
}
static Item* bottle() {
    if(!context())return nullptr;
    Item* item=stats[clientnum]->weapon;
    return item && itemCategory(item)==POTION && item->type!=POTION_EMPTY &&
        item->status!=BROKEN && item->count>0 ? item : nullptr;
}
static bool posture(float& distance) {
    const auto& q=g_xrViews[0].pose.orientation;
    const auto mouthOffset=vrGripRotateXR(q,{0.f,-.10f,-.08f});
    const auto& left=g_xrViews[0].pose.position;
    const auto& right=g_xrViews[1].pose.position;
    const auto& grip=g_xrControllerHands[1].gripPose;
    const float x=grip.position.x-(left.x+right.x)*.5f-mouthOffset.x;
    const float y=grip.position.y-(left.y+right.y)*.5f-mouthOffset.y;
    const float z=grip.position.z-(left.z+right.z)*.5f-mouthOffset.z;
    distance=std::sqrt(x*x+y*y+z*z);
    return std::isfinite(distance);
}
static void sample() {
    g_vrDrinkOwnsRightGrip=false;
    Item* item=bottle();float distance=0;
    if(!item || !posture(distance)) {
        static Uint32 blockedReport=0;
        const Uint32 now=SDL_GetTicks();
        if(clientnum>=0 && clientnum<MAXPLAYERS && players[clientnum] && stats[clientnum] &&
            stats[clientnum]->weapon && itemCategory(stats[clientnum]->weapon)==POTION &&
            g_xrControllerHands[1].squeeze>=.65f && (!blockedReport || now-blockedReport>=1500u)) {
            blockedReport=now;
            printlog("[VR GA33] drink blocked: single=%d paused=%d calibration=%d shoot=%d tracking=%d backpack=%d/%d",
                multiplayer==SINGLE?1:0,gamePaused?1:0,g_vrArmCalibrationActive?1:0,
                players[clientnum]->shootmode?1:0,g_xrControllerHands[1].gripPoseValid?1:0,
                g_xrVrBackpackInZone?1:0,g_xrVrBackpackGripConsumed?1:0);
        }
        state.reset();return;
    }
    const auto& hand=g_xrControllerHands[1];
    const bool queued=state.sample(SDL_GetTicks(),item->uid,players[clientnum]->entity->getUID(),
        static_cast<int>(item->type),item->count,currentlevel,secretlevel,
        distance,hand.squeeze,hand.trigger);
    // Reserve RB only near the mouth or until release after a drink. All other
    // grip interactions retain their existing priority and forwarding rules.
    g_vrDrinkOwnsRightGrip=hand.squeeze>=.35f &&
        (distance<=.30f || state.latched);
    static Uint32 reported=0;
    const Uint32 now=SDL_GetTicks();
    if(hand.squeeze>=.65f && (!reported || now-reported>=1500u)) {
        reported=now;
        printlog("[VR GA33] drink check distance=%.3fm grip=%.2f trigger=%.2f queued=%d; press GRIP at face",
            static_cast<double>(distance),static_cast<double>(hand.squeeze),static_cast<double>(hand.trigger),queued?1:0);
    }
    if(queued)printlog("[VR GA33] drink queued item=%u type=%d distance=%.3fm",state.uid,state.type,static_cast<double>(distance));
}
static void applyPending() {
    if(!state.pending)return;
    state.pending=false; // consume the request before any native call
    Item* item=bottle();float distance=0;
    const auto& hand=g_xrControllerHands[1];
    if(!item || SDL_GetTicks()-state.queuedAt>150u || SDL_GetTicks()-state.last>150u ||
        item->uid!=state.uid || static_cast<int>(item->type)!=state.type || item->count!=state.count ||
        players[clientnum]->entity->getUID()!=state.owner || currentlevel!=state.level || secretlevel!=state.secret ||
        !posture(distance) || distance>.25f || hand.trigger>=.15f) {
        printlog("[VR GA33] drink cancelled: item, pose, context or timing changed");return;
    }
    printlog("[VR GA33] native potion use requested item=%u type=%d count=%d",state.uid,state.type,state.count);
    useItem(item,clientnum); // native effects, consumption, recipe and bottle recovery
    // The native call can free item and clear stats.weapon. Never read item again.
}
} // namespace VrPotionDrink
void near(){g_xrControllerHands[1].gripPose.position={0,-.1f,-.08f};g_xrControllerHands[1].gripPose.orientation={0,0,0,1};}
void queue(){near();g_xrControllerHands[1].squeeze=0;VrPotionDrink::sample();now+=20;g_xrControllerHands[1].squeeze=1;VrPotionDrink::sample();assert(VrPotionDrink::state.pending);}
int main(){
 s.weapon=new Item;queue();VrPotionDrink::applyPending();assert(calls==1 && s.weapon->count==1);
 for(int i=0;i<20;i++){now+=100;VrPotionDrink::sample();VrPotionDrink::applyPending();}assert(calls==1);
 now+=100;queue();g_xrControllerHands[1].squeeze=0;VrPotionDrink::sample();VrPotionDrink::applyPending();assert(calls==2 && !s.weapon); // quick tap survives release, native call frees last bottle
 VrPotionDrink::sample();VrPotionDrink::applyPending();assert(calls==2);
 s.weapon=new Item;now+=100;queue();allowed=false;VrPotionDrink::applyPending();assert(calls==2);allowed=true;
 VrPotionDrink::state.reset();now+=100;queue();++s.weapon->uid;VrPotionDrink::applyPending();assert(calls==2);
 VrPotionDrink::state.reset();now+=100;queue();--s.weapon->count;VrPotionDrink::applyPending();assert(calls==2);
 VrPotionDrink::state.reset();now+=100;queue();now+=151;VrPotionDrink::applyPending();assert(calls==2);
 VrPotionDrink::state.reset();now+=100;queue();g_xrControllerHands[1].trigger=1;VrPotionDrink::applyPending();assert(calls==2);g_xrControllerHands[1].trigger=0;
 VrPotionDrink::state.reset();s.weapon->type=POTION_EMPTY;now+=100;near();g_xrControllerHands[1].squeeze=0;VrPotionDrink::sample();g_xrControllerHands[1].squeeze=1;VrPotionDrink::sample();VrPotionDrink::applyPending();assert(calls==2);
 delete s.weapon;s.weapon=nullptr;
 std::cout<<"PASS: exact production integration: grip-press native call, quick tap, last-bottle deletion, no repeat and stale/context/item/trigger exclusions.\n";
}
