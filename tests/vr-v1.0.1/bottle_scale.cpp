#include <cassert>
#include <cmath>
#include <iostream>
struct Item {int category;}; enum {POTION,OTHER}; int itemCategory(Item*i){return i->category;}
struct Entity {double scalex;}; struct Profile{float scaleMultiplier;};
float scale(Item*equipped,Entity*item,bool rightHand,bool vrWorldBook,Profile grip){
const float VR_SHIELD_VOXEL_SCALE=.3f,VR_ITEM_VOXEL_SCALE=.4f;
    const float visualScale = (equipped && itemCategory(equipped) == POTION ? 0.5f : 1.0f) * (vrWorldBook ? (0.5f * VR_SHIELD_VOXEL_SCALE) :
        (rightHand ? VR_ITEM_VOXEL_SCALE * grip.scaleMultiplier :
            VR_SHIELD_VOXEL_SCALE) *
        ((item->scalex > 0.01 && item->scalex < 4.0) ?
            static_cast<float>(item->scalex) : 1.0f));
return visualScale;}
int main(){Item potion{POTION},other{OTHER}; Profile grip{1.2f};
for(bool hand:{false,true})for(double n:{.5,1.,2.,0.,5.}){Entity e{n};float native=scale(&other,&e,hand,false,grip);float bottle=scale(&potion,&e,hand,false,grip);assert(std::fabs(bottle-native*.5f)<1e-7f);for(int i=0;i<100;i++)assert(scale(&potion,&e,hand,false,grip)==bottle);assert(e.scalex==n);}
Entity e{1};assert(std::fabs(scale(&other,&e,false,true,grip)-.15f)<1e-7f);assert(scale(nullptr,&e,true,false,grip)==scale(&other,&e,true,false,grip));std::cout<<"PASS: exact production scale expression: potion-only 50%, both hands, native relative scale, book and repeat stability.\n";}
