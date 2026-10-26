#pragma once
// GA23: native Barony calibration widgets; private ray routing never injects
// inventory mouse events. Only our read-only Fields get process(usable=false)
// to build their native text caches; no global UI tree is processed here.
#include "ui/Button.hpp"
#include "ui/Field.hpp"
#include "ui/Slider.hpp"
extern bool drawingGui;
namespace VrCalibrationMenu {
static constexpr float degrees=57.2957795131f;
static bool legacy=false,held=false,armed=false,hit=false;
static int tab=0,sub=0,hand=0,drag=-1;
static float pointerX=0.f,pointerY=0.f;
static float bowRotationDelta[2][3]{};
static std::unique_ptr<Frame> frame;
static int frameWidth=0,frameHeight=0,frameTab=-1,frameSub=-1,frameHand=-1;
static int frameWeapon=-2,frameOffhand=-2,frameDrawHand=-1;
static Field* status=nullptr;
struct Click { Button* widget; SDL_Rect bounds; int action; bool selected; };
struct Edit { Slider* widget;Field* value;float* data;float lo,hi,display;
    const char* unit;const char* explanation;int kind,axis; };
static std::vector<Click> clicks;
static std::vector<Edit> edits;
static std::vector<Field*> fields;
static VrNativeHud::Surface scratch,target;
static bool visible() {
    return g_vrArmCalibrationActive && !legacy &&
        !g_vrBodyCalibrationPage && !g_vrArmCalibrationEditMelee;
}
static SDL_Rect rect(int x,int y,int w,int h) {
    return SDL_Rect{x*Frame::virtualScreenX/1280,y*Frame::virtualScreenY/720,
        std::max(1,w*Frame::virtualScreenX/1280),std::max(1,h*Frame::virtualScreenY/720)};
}
static bool inside(SDL_Rect r,float x,float y) {
    return x>=r.x && y>=r.y && x<r.x+r.w && y<r.y+r.h;
}
static Field* label(const char* name,const char* text,int x,int y,int w,int h=28) {
    auto* f=frame->addField(name,1024);f->setOwner(clientnum);
    f->setSize(rect(x,y,w,h));f->setFont("fonts/pixel_maz_multiline.ttf#18#2");
    f->setText(text);f->setTextColor(makeColor(246,230,200,255));
    f->setEditable(false);fields.push_back(f);return f;
}
static void button(const char* name,const char* text,SDL_Rect r,int action,bool selected=false) {
    auto* b=frame->addButton(name);b->setOwner(clientnum);b->setSize(rect(r.x,r.y,r.w,r.h));
    b->setFont("fonts/pixel_maz_multiline.ttf#18#2");b->setText(text);
    b->setColor(0xffffffff);b->setTextColor(0xffffffff);
    clicks.push_back(Click{b,r,action,selected});
}
// kind 0=equipment profile, 2=melee tuning, 3=incremental
// controller-local bow quaternion. Display conversions never change save units.
static void slider(const char* name,const char* text,float* data,float lo,float hi,
    int row,const char* explanation,const char* unit="units",float display=1.f,int kind=0,int axis=-1) {
    const int y=240+row*40;
    label((std::string(name)+"_label").c_str(),text,360,y,290,30);
    auto* s=frame->addSlider(name);s->setOwner(clientnum);
    s->setOrientation(Slider::SLIDER_HORIZONTAL);s->setRailSize(rect(660,y+5,330,24));
    s->setHandleSize(rect(0,0,40,36));s->setBorder(std::max(1,16*Frame::virtualScreenX/1280));
    s->setMinValue(lo);s->setMaxValue(hi);s->setValue(std::max(lo,std::min(hi,*data)));
    s->setColor(0xffffffff);s->setHighlightColor(0xffffffff);
    s->setHandleImage("*images/ui/Main Menus/Settings/Settings_ValueSlider_Slide00.png");
    s->setRailImage("*images/ui/Main Menus/Settings/Settings_ValueSlider_Backing00.png");
    auto* value=label((std::string(name)+"_value").c_str(),"",1010,y,160,30);
    edits.push_back(Edit{s,value,data,lo,hi,display,unit,explanation,kind,axis});
}
template<class Pose> static void pose(Pose& p,float limit,const char* unit,int first=0) {
    slider("forward","Depth offset",&p.forward,-limit,limit,first,
        "Moves this model toward / away from its tracked hand or HUD anchor.",unit);
    slider("up","Vertical offset",&p.up,-limit,limit,first+1,
        "Moves this model vertically relative to its existing anchor.",unit);
    slider("side","Sideways offset",&p.side,-limit,limit,first+2,
        "Moves this model sideways relative to its existing anchor.",unit);
    slider("yaw","Turn left / right",&p.yaw,-3.14159f,3.14159f,first+3,
        "Rotates the model horizontally. Does not change projectile aim.","deg",degrees);
    slider("pitch","Tilt up / down",&p.pitch,-3.14159f,3.14159f,first+4,
        "Tilts the model. Does not change projectile aim.","deg",degrees);
    slider("roll","Twist / roll",&p.roll,-3.14159f,3.14159f,first+5,
        "Rolls the model around its local axis.","deg",degrees);
}
static const char* section(int index) {
    static const char* names[4][7]={
        {"Arm placement","Body pose setup","Room-scale tracking",nullptr},
        {"Held item","Crosshair / shots","Spell aim","Bow grip","Bow draw",nullptr},
        {"HP / Mana wrist","Minimap wrist","Status icons","Belt hotbar","XP bar","Party health bars","Skill notifications"},
        {"Getting started","Bow handedness","Melee sensitivity","Backpack & items","D-pad & turning","Visibility tests",nullptr}
    };
    return index>=0 && index<7?names[tab][index]:nullptr;
}
static void syncContext() {
    g_vrArmCalibrationSelected=(tab==1 && sub==1)?1:hand;
    g_vrArmCalibrationEditItem=tab==1 && sub==0;
    g_vrArmCalibrationEditAim=tab==1 && sub==1;
    g_vrArmCalibrationEditSpellAim=tab==1 && sub==2;
    g_vrArmCalibrationEditBow=tab==1 && (sub==3 || sub==4);
    g_vrBowEditGrip=tab==1 && sub==3;
    g_vrArmCalibrationEditFlail=false; // Player calibration does not expose flail tuning.
    g_vrArmCalibrationEditWearable=tab==2 && sub<2;
    if(g_vrArmCalibrationEditWearable)g_vrArmCalibrationSelected=sub;
    g_vrArmCalibrationEditStatusEffects=tab==2 && sub==2;
    g_vrArmCalibrationSpatialPage=tab==2 && sub>=3 ? (sub==3?0:sub==4?3:sub==5?4:5):-1;
}
static void build() {
    if(Frame::virtualScreenX<=0 || Frame::virtualScreenY<=0)return;
    const int weapon=stats[clientnum] && stats[clientnum]->weapon?static_cast<int>(stats[clientnum]->weapon->type):-1;
    const int offhand=stats[clientnum] && stats[clientnum]->shield?static_cast<int>(stats[clientnum]->shield->type):-1;
    if(frame && frameWidth==Frame::virtualScreenX && frameHeight==Frame::virtualScreenY &&
        frameTab==tab && frameSub==sub && frameHand==hand && frameWeapon==weapon &&
        frameOffhand==offhand && frameDrawHand==g_vrBowDrawHand)return;
    frame.reset();clicks.clear();edits.clear();fields.clear();status=nullptr;drag=-1;
    frameWidth=Frame::virtualScreenX;frameHeight=Frame::virtualScreenY;
    frameTab=tab;frameSub=sub;frameHand=hand;frameWeapon=weapon;frameOffhand=offhand;
    frameDrawHand=g_vrBowDrawHand;
    frame.reset(new Frame("vr_calibration_ga23"));frame->setOwner(clientnum);
    frame->setSize(SDL_Rect{0,0,frameWidth,frameHeight});
    frame->setActualSize(SDL_Rect{0,0,frameWidth,frameHeight});
    frame->setColor(0);frame->setBorder(0);frame->setScrollBarsEnabled(false);
    frame->addImage(rect(60,35,1160,650),0xffffffff,
        "*images/ui/Main Menus/Settings/GenericWindow/UI_MM14_Window00.png","window");
    auto* rocks=frame->addImage(rect(94,145,1092,480),0xffffffff,
        "*images/ui/Main Menus/Settings/GenericWindow/UI_MM14_Rocks01.png","rocks");rocks->tiled=true;
    label("title","VR CALIBRATION",485,57,360);
    const char* tabs[]={"Body","Weapons & Items","HUD","Controls & Help"};
    for(int i=0;i<4;++i)button(("tab"+std::to_string(i)).c_str(),tabs[i],SDL_Rect{100+i*273,100,260,54},i,i==tab);
    for(int i=0;i<7 && section(i);++i)button(("section"+std::to_string(i)).c_str(),section(i),
        SDL_Rect{104,173+i*51,230,44},10+i,i==sub);
    label("section_title",section(sub),360,173,810,30);
    status=label("status","",360,552,810,36);
    if(tab==0 && sub==0) {
        button("left","Left arm",SDL_Rect{360,205,190,32},30,hand==0);
        button("right","Right arm",SDL_Rect{565,205,190,32},31,hand==1);
        float* size=hand==0?&g_vrLeftArmScaleFactor:&g_vrRightArmScaleFactor;
        slider("arm_size",hand==0?"Left arm size":"Right arm size",size,.70f,3.50f,0,
            "Scales only the selected arm. The other arm and equipment offsets stay independent.","x");
        pose(g_vrArmVisual[hand],8.f,"units",1);
    } else if(tab==0 && sub==1) {
        label("pose_help","Stand comfortably with both arms extended sideways.\nHold BOTH grips for two seconds to measure your reach.\nKeep your hands near shoulder height and look forward.",360,240,800,110);
        button("body_enabled",g_vrBodyFit.enabled?"Disable body fit":"Enable saved body fit",SDL_Rect{365,405,400,48},40);
        label("pose_note","Manual left / right arm size remains on Arm placement.\nPose measurement preserves your equipment grip profiles.",360,475,800,65);
    } else if(tab==0 && sub==2) {
        label("room_help","Offline single-player positional tracking test. Stand upright to enable.\nSteps use native collision; the belt follows your headset in either mode.\nBoth hands, aiming and grabbing share the same tracked view.\nStick forward follows your headset while room tracking is enabled.\nUse the existing seated mode to return to the original controls.",360,240,810,155);
        button("room_on",g_vrRoomScaleEnabled?"Tracking test: ON":"Enable standing tracking",SDL_Rect{365,420,440,45},90,g_vrRoomScaleEnabled);
        button("room_off","Disable tracking test",SDL_Rect{365,475,440,45},91,!g_vrRoomScaleEnabled);
        button("room_center","Recenter upright",SDL_Rect{825,420,330,45},92);
    } else if(tab==1 && sub==0) {
        button("offhand","Offhand slot",SDL_Rect{360,205,225,32},30,hand==0);
        button("mainhand","Weapon slot",SDL_Rect{600,205,225,32},31,hand==1);
        const Item* item=vrCalibrationEquippedItem(hand);
        if(item && item->type!=SHORTBOW) {
            pose(g_vrItemVisual[hand][static_cast<int>(item->type)],8.f,"units");
            label("item_name",item->getName(),360,490,810);
            label("item_size","This item's renderer has no independent size setting.",360,525,810);
        } else label("no_item",item?"Use Bow grip for the shortbow's physical hand placement.":
            "No supported item is equipped in this slot.",360,270,800,70);
    } else if(tab==1 && sub==1) {
        const Item* item=vrCalibrationRangedItem();
        if(item) {
            auto& aim=g_vrRangedAimVisual[static_cast<int>(item->type)];
            slider("aim_yaw","Aim left / right",&aim.yaw,-.35f,.35f,0,
                "Moves the weapon crosshair and launch ray together; does not rotate the held mesh.","deg",degrees);
            slider("aim_pitch","Aim up / down",&aim.pitch,-.35f,.35f,1,
                "Moves the weapon crosshair and launch ray vertically together.","deg",degrees);
            label("aim_item",item->getName(),360,365,800);
            label("aim_note","Aim correction is saved separately for each ranged / throwable item.\nPhysical shortbow aim follows the bow-and-string geometry.",360,410,800,80);
        } else label("aim_none","Equip a ranged or throwable weapon to adjust its aim.",360,270,800,65);
    } else if(tab==1 && sub==2) {
        slider("spell_yaw","Spell aim left / right",&g_vrSpellAimVisual.yaw,-VR_SPELL_AIM_MAX_YAW,VR_SPELL_AIM_MAX_YAW,0,
            "Adjusts the spell crosshair and spell projectile ray horizontally.","deg",degrees);
        slider("spell_pitch","Spell aim up / down",&g_vrSpellAimVisual.pitch,-VR_SPELL_AIM_MAX_PITCH,VR_SPELL_AIM_MAX_PITCH,1,
            "Adjusts the spell crosshair and spell projectile ray vertically.","deg",degrees);
        label("spell_note","Spell aim remains independent of weapon grip placement.\nWith a bow equipped, spells follow the existing bow hand policy.",360,365,800,90);
    } else if(tab==1 && sub==3) {
        if(vrBowEquipped()) {
            const int h=1-g_vrBowDrawHand;auto& g=g_vrBowGrip[h];
            slider("bow_depth","Bow depth offset",&g.forward,-.30f,.30f,0,"Moves the bow forward / backward relative to its controller.","m");
            slider("bow_height","Bow vertical offset",&g.up,-.30f,.30f,1,"Moves the bow up / down relative to its controller.","m");
            slider("bow_side","Bow sideways offset",&g.side,-.30f,.30f,2,"Moves the bow sideways relative to its controller.","m");
            slider("bow_turn","Turn from saved pose",&bowRotationDelta[h][1],-3.14159f,3.14159f,3,
                "Controller-local turn relative to this menu session's starting pose. Saved as a quaternion.","deg",degrees,3,1);
            slider("bow_tilt","Tilt from saved pose",&bowRotationDelta[h][2],-3.14159f,3.14159f,4,
                "Controller-local tilt relative to the starting pose; avoids legacy Euler gimbal lock.","deg",degrees,3,2);
            slider("bow_twist","Twist from saved pose",&bowRotationDelta[h][0],-3.14159f,3.14159f,5,
                "Twists around the controller's forward axis, relative to the starting pose.","deg",degrees,3,0);
            slider("bow_model_z","Model Z rotation",&g_vrBowZRotation[h],-3.14159f,3.14159f,6,
                "Additional model-local Z rotation, kept separate from controller-local alignment.","deg",degrees);
        } else label("bow_none","Equip a shortbow to adjust its physical grip.",360,270,800,65);
    } else if(tab==1 && sub==4) {
        slider("draw_length","Full draw distance",&g_vrBowFullDraw,.20f,.65f,0,
            "Real-world draw distance used for your full-draw calibration.","m");
        button("draw_left","Left draw / bow right",SDL_Rect{365,320,370,48},60,g_vrBowDrawHand==0);
        button("draw_right","Right draw / bow left",SDL_Rect{755,320,400,48},61,g_vrBowDrawHand==1);
        label("draw_help","Equip a shortbow. Hold the DRAW hand's grip near the string,\npull back, then release the grip. This calibration is a dry run:\nno arrows are fired and no ammunition is spent.",360,400,800,110);
    } else if(tab==2) {
        auto& h=sub<2?g_vrWearableHud[sub]:sub==2?g_vrStatusEffectsHud:g_vrSpatialHud[sub==3?0:sub==4?3:sub==5?4:5];
        pose(h,4.f,"units");
        slider("hud_size","Panel size",&h.scale,.35f,3.f,6,"Scales only this VR panel; original artwork and animation remain unchanged.","x");
    } else if(tab==3 && sub==0) {
        label("help_basic","Hold X+Y to open or close calibration.\nPoint with your RIGHT controller; hold its trigger to drag sliders.\nThe left column selects a page within the current category.\nValues show model units, metres, degrees or a size multiplier.\nHover a slider to read what it changes.\nEdits autosave; Save & Close also saves all calibration changes.",360,240,810,270);
    } else if(tab==3 && sub==1) {
        button("left_draw","Left draw / bow right",SDL_Rect{365,260,370,48},60,g_vrBowDrawHand==0);
        button("right_draw","Right draw / bow left",SDL_Rect{755,260,400,48},61,g_vrBowDrawHand==1);
        label("hand_help","Choose the hand that pulls the string. The bow uses the other hand.\nDraw with GRIP; release GRIP to shoot during normal gameplay.\nThe draw-hand trigger casts spells when not drawing.\nBow grip and full draw calibration are under Weapons & Items.",360,350,810,170);
    } else if(tab==3 && sub==2) {
        slider("tip_speed","Minimum blade speed",&g_vrMeleeTuning.minTipSpeed,.8f,4.5f,0,
            "Lower values recognize slower blade swings; higher values require faster swings.","m/s",1.f,2);
        slider("hand_speed","Minimum hand speed",&g_vrMeleeTuning.minGripSpeed,.2f,2.5f,1,
            "Requires your hand to move with the blade, reducing accidental attacks.","m/s",1.f,2);
        slider("rearm","Recovery speed threshold",&g_vrMeleeTuning.rearmSpeed,.08f,.70f,2,
            "Slow below this threshold to prepare the next recognized swing.","m/s",1.f,2);
        label("combat_note","Calibration uses dry-run swing recognition; native damage and reach\nremain unchanged. The existing training-prop test is still available\nthrough the original melee editor.",360,395,810,110);
        button("old_melee","Open training-prop editor",SDL_Rect{365,510,450,40},70);
    } else if(tab==3 && sub==3) {
        label("backpack_help","Single-player pilot: use an EMPTY hand and hold GRIP within 30 cm of a loose item.\nKeep holding GRIP to carry it; release GRIP to drop it.\nBow, equipped offhands and special quest items are excluded.\nPhysical throwing and storing held items in the backpack come later.\nEquipped potion: raise the RIGHT controller near your face,\nthen press GRIP to drink once. No tilt or hold time is needed.\nRelease and press GRIP again for another drink. Trigger still throws.",360,240,810,300);
    } else if(tab==3 && sub==5) {
        label("visibility_help","Temporary tests: change ONE switch, close calibration, then compare.\nFull-angle keeps wall occlusion; it may lower frame rate.\nHide floating HUD checks whether the overhead strip is a HUD panel.\nBoth tests reset when the game closes; Restore normal view undoes both.",360,240,810,100);
        button("visibility_full",g_vrVisibilityProbeFull?"Angular culling: BYPASSED":"Test full-angle visibility",SDL_Rect{365,355,750,45},93,g_vrVisibilityProbeFull);
        button("visibility_hud",g_vrSpatialHudProbeHidden?"Floating HUD: HIDDEN":"Test hiding floating HUD",SDL_Rect{365,415,750,45},94,g_vrSpatialHudProbeHidden);
        button("visibility_restore","Restore normal view",SDL_Rect{365,475,750,45},95);
    } else if(tab==3 && sub==4) {
        label("dpad_help","Touch the RIGHT thumb rest and move the LEFT stick for D-pad.\nLift your thumb to return the left stick to movement.\nPush the RIGHT stick DOWN for the existing Quick Turn animation.\nRight grip keeps its normal actions outside bow-string ownership.\nThe thumb-rest shortcut requires a supported controller touch input.\nCalibration keeps these gameplay shortcuts from editing values.",360,240,810,260);
    }
    button("save_close","Save & Close",SDL_Rect{820,590,340,46},80);
    label("footer","BARONY VR  |  Point with right hand. Trigger: click / drag.  X+Y: save / close.",165,647,1010);
    auto* cursor=frame->addImage(rect(0,0,24,30),0xffffffff,"*images/system/cursor_hand.png","pointer");cursor->ontop=true;
}
static void save() {
    if(g_vrArmCalibrationDirty)vrSaveVisualArmCalibration();
    if(g_vrFlailSettingsDirty)vrSaveModeSettings();
    if(g_vrMeleeTuningDirty)vrSaveMeleeTuning();
}
static void open() {
    legacy=false;held=false;armed=false;hit=false;drag=-1;tab=0;sub=0;
    hand=g_vrArmCalibrationSelected;frameTab=-1;
    g_vrBodyCalibrationPage=false;g_vrArmCalibrationEditMelee=false;
    for(int h=0;h<2;++h)for(int a=0;a<3;++a)bowRotationDelta[h][a]=0.f;
    g_vrBowPreview.drawing=false;g_vrBowPreview.armed=false;
    syncContext();
}
static void action(int id) {
    if(id<4){tab=id;sub=0;}
    else if(id>=10 && id<17 && section(id-10))sub=id-10;
    else if(id==30 || id==31)hand=id-30;
    else if(id==40) {
        const auto previous=g_vrBodyFit;g_vrBodyFit.enabled=!g_vrBodyFit.enabled;
        if(!vrSaveBodyFit())g_vrBodyFit=previous;
    } else if(id==60 || id==61) {
        if(g_vrBowDrawHand!=id-60){g_vrBowDrawHand=id-60;g_vrArmCalibrationDirty=true;}
    } else if(id==70) {
        save();legacy=true;
        // Keep the existing native training-prop/gesture editor unchanged.
        g_vrArmCalibrationSelected=1;g_vrArmCalibrationEditMelee=true;
        g_vrArmCalibrationEditBow=false;g_vrArmCalibrationEditFlail=false;
        g_vrArmCalibrationEditItem=false;g_vrArmCalibrationEditAim=false;
        g_vrArmCalibrationEditSpellAim=false;g_vrArmCalibrationEditWearable=false;
        g_vrArmCalibrationEditStatusEffects=false;g_vrArmCalibrationSpatialPage=-1;
    } else if(id==80){save();g_vrArmCalibrationActive=false;}
    else if(id==90)VrRoomScale::setEnabled(true);
    else if(id==91)VrRoomScale::setEnabled(false);
    else if(id==92)vrRequestRoomCalibration();
    else if(id==93){g_vrVisibilityProbeFull=!g_vrVisibilityProbeFull;printlog("[VR GA30] angular culling probe full=%d",g_vrVisibilityProbeFull?1:0);}
    else if(id==94){g_vrSpatialHudProbeHidden=!g_vrSpatialHudProbeHidden;printlog("[VR GA30] floating HUD probe hidden=%d",g_vrSpatialHudProbeHidden?1:0);}
    else if(id==95){g_vrVisibilityProbeFull=false;g_vrSpatialHudProbeHidden=false;printlog("[VR GA30] visibility probes reset");}
    drag=-1;armed=false;frameTab=-1;
    g_vrBowPreview.drawing=false;g_vrBowPreview.armed=false;
    if(!legacy)syncContext();
}
static void input(XrTime time) {
    hit=false;
    if(!visible()){held=false;armed=false;drag=-1;return;}
    syncContext();vrLoadMenuPointerOffset();vrLoadBodyFit();build();if(!frame)return;
    const auto& controller=g_xrControllerHands[1];
    const bool focused=g_xrSessionState==XR_SESSION_STATE_FOCUSED;
    const bool down=focused && controller.trigger>=(held?.35f:.65f);
    if(!focused || !controller.aimPoseValid){armed=false;held=down;drag=-1;return;}
    if(controller.trigger<.35f)armed=true;
    if(g_xrRightAimSpace!=XR_NULL_HANDLE && g_xrGuiViewSpace!=XR_NULL_HANDLE) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if(XR_SUCCEEDED(xrLocateSpace(g_xrRightAimSpace,g_xrGuiViewSpace,time,&location)) &&
            (location.locationFlags&XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (location.locationFlags&XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            const auto& p=location.pose;const auto& q=p.orientation;
            const float dx=-2.f*(q.x*q.z+q.y*q.w),dy=-2.f*(q.y*q.z-q.x*q.w);
            const float dz=-1.f+2.f*(q.x*q.x+q.y*q.y);
            if(std::fabs(dz)>.001f) {
                const float t=(g_xrGuiLayer.pose.position.z-p.position.z)/dz;
                if(t>.02f && t<6.f && g_xrGuiLayer.size.width>0 && g_xrGuiLayer.size.height>0) {
                    pointerX=((p.position.x+t*dx)/g_xrGuiLayer.size.width+.5f)*1280.f+g_xrVrMenuPointerOffsetX;
                    pointerY=(.5f-(p.position.y+t*dy)/g_xrGuiLayer.size.height)*720.f;
                    hit=pointerX>=0 && pointerX<1280 && pointerY>=0 && pointerY<720;
                }
            }
        }else {armed=false;drag=-1;}
    }
    if(down && !held && armed && hit) {
        for(const auto& click:clicks)if(inside(click.bounds,pointerX,pointerY)){action(click.action);break;}
        if(armed)for(int i=0;i<static_cast<int>(edits.size());++i)
            if(inside(SDL_Rect{640,236+i*40,370,40},pointerX,pointerY)){drag=i;break;}
    }
    if(down && armed && drag>=0 && hit) {
        auto& e=edits[drag];
        const float fraction=std::max(0.f,std::min(1.f,(pointerX-676.f)/298.f));
        const float next=e.lo+fraction*(e.hi-e.lo);
        if(*e.data!=next) {
            if(e.kind==3)vrBowAdjustControllerRotation(1-g_vrBowDrawHand,e.axis,next-*e.data);
            *e.data=next;
            if(e.kind==2)g_vrMeleeTuningDirty=true;
            else g_vrArmCalibrationDirty=true;
        }
    }
    if(!down)drag=-1;
    held=down;
    if(tab==0 && sub==1 && visible()) {
        // Clicks must never invoke the original held-trigger body reset.
        g_vrBodyResetLatched=true;vrUpdateBodyPoseCalibration(time);
    }else vrResetBodyPoseSamples();
}
static bool draw() {
    if(!visible())return false;
    build();if(!frame)return false;
    for(auto& click:clicks) {
        const bool hover=hit && inside(click.bounds,pointerX,pointerY);
        std::string path="*images/ui/Main Menus/Leaderboards/AA_Button_Subtitle_";
        path+=click.selected?"Selected":"Unselected";path+=hover?"High_00.png":"_00.png";
        click.widget->setBackground(path.c_str());
    }
    const char* hint="Point at a slider to see what it changes.";
    for(int i=0;i<static_cast<int>(edits.size());++i) {
        auto& e=edits[i];e.widget->setValue(std::max(e.lo,std::min(e.hi,*e.data)));
        char text[80];sprintf_s(text,sizeof(text),"%.2f %s",static_cast<double>(*e.data*e.display),e.unit);
        e.value->setText(text);
        if((hit && inside(SDL_Rect{350,236+i*40,820,40},pointerX,pointerY)) || drag==i)hint=e.explanation;
    }
    char current[256];
    if(tab==0 && sub==1) {
        sprintf_s(current,sizeof(current),"Body fit %s | Left %.2f m | Right %.2f m | %s",
            g_vrBodyFit.enabled?"ON":"OFF",static_cast<double>(g_vrBodyFit.measuredReachMeters[0]),
            static_cast<double>(g_vrBodyFit.measuredReachMeters[1]),g_vrBodyPoseLatched?"Measurement saved":"Hold both grips in T-pose");
        hint=current;
    }else if(tab==1 && sub==4) {
        sprintf_s(current,sizeof(current),"Draw %.2f m | Test releases %u | %s",
            static_cast<double>(g_vrBowPreview.draw),g_vrBowPreview.releases,
            g_vrBowPreview.drawing?"String held":"No shots fired in calibration");hint=current;
    }
    if(status)status->setText(hint);
    if(auto* cursor=frame->findImage("pointer")) {
        cursor->disabled=!hit;cursor->pos=rect(static_cast<int>(pointerX),static_cast<int>(pointerY),24,30);
    }
    VrNativeHud::GlState state;
    const bool success=VrNativeHud::render(scratch,target,SDL_Rect{0,0,frameWidth,frameHeight},frameWidth,frameHeight,[](){
        const bool wasDrawingGui=::drawingGui;::drawingGui=true;
        // Native Field::draw uses a cache populated by process(). These are
        // only our own read-only, inactive Fields, with no tick callbacks.
        // usable=false returns after cache preparation, before mouse handling.
        const SDL_Rect whole{0,0,frameWidth,frameHeight};
        for(auto* f:fields)if(f->dirty)f->process(whole,whole,false);
        frame->drawForVr();::drawingGui=wasDrawingGui;Shader::unbind();
    });
    if(!success)return false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER,target.framebuffer);glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,g_xrGuiFramebuffer);glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0,target.height,target.width,0,0,0,1280,720,GL_COLOR_BUFFER_BIT,GL_LINEAR);
    return true;
}
} // namespace VrCalibrationMenu
