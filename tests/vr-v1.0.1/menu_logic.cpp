// Headless tests of the production menu routing/bindings. Widget and XR/GL
// boundaries are stubs; this does not substitute for Windows/headset tests.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <iostream>
using XrTime=long long;using Uint32=unsigned;using GLuint=unsigned;
struct SDL_Rect {int x,y,w,h;};
static unsigned makeColor(int,int,int,int){return ~0u;}
static int cacheBuilds=0;
struct Field {bool dirty=true,editable=false;std::string text;
 void setOwner(int){}void setSize(SDL_Rect){}void setFont(const char*){}
 void setText(const char* s){if(text!=s)dirty=true;text=s;}
 void setTextColor(unsigned){}void setEditable(bool b){editable=b;}
 void process(SDL_Rect,SDL_Rect,bool usable){assert(!usable && !editable);dirty=false;++cacheBuilds;}
};
struct Button {void setOwner(int){}void setSize(SDL_Rect){}void setFont(const char*){}void setText(const char*){}
 void setColor(unsigned){}void setTextColor(unsigned){}void setBackground(const char*){} };
struct Slider {enum{SLIDER_HORIZONTAL};float lo=0,hi=1,value=0;
 void setOwner(int){}void setOrientation(int){}void setRailSize(SDL_Rect){}void setHandleSize(SDL_Rect){}
 void setBorder(int){}void setMinValue(float f){lo=f;}void setMaxValue(float f){hi=f;}void setValue(float f){value=f;}
 void setColor(unsigned){}void setHighlightColor(unsigned){}void setHandleImage(const char*){}void setRailImage(const char*){}
};
struct Frame {inline static int virtualScreenX=1280,virtualScreenY=720;
 struct Image{bool tiled=false,ontop=false,disabled=false;SDL_Rect pos{};};
 std::vector<std::unique_ptr<Field>> fields;std::vector<std::unique_ptr<Button>> buttons;
 std::vector<std::unique_ptr<Slider>> sliders;std::vector<std::unique_ptr<Image>> images;
 explicit Frame(const char*){}void setOwner(int){}void setSize(SDL_Rect){}void setActualSize(SDL_Rect){}
 void setColor(int){}void setBorder(int){}void setScrollBarsEnabled(bool){}
 Field* addField(const char*,int){fields.emplace_back(new Field);return fields.back().get();}
 Button* addButton(const char*){buttons.emplace_back(new Button);return buttons.back().get();}
 Slider* addSlider(const char*){sliders.emplace_back(new Slider);return sliders.back().get();}
 Image* addImage(SDL_Rect r,unsigned,const char*,const char*){images.emplace_back(new Image);images.back()->pos=r;return images.back().get();}
 Image* findImage(const char*){return images.empty()?nullptr:images.back().get();}
 void drawForVr(){for(auto& f:fields)assert(!f->dirty);}
};
static int sprintf_s(char* dst,size_t n,const char* fmt,...){va_list v;va_start(v,fmt);int r=std::vsnprintf(dst,n,fmt,v);va_end(v);return r;}
struct XrVector3f{float x=0,y=0,z=0;};struct XrQuaternionf{float x=0,y=0,z=0,w=1;};
struct Pose{XrQuaternionf orientation;XrVector3f position;};
struct XrSpaceLocation{int type;unsigned locationFlags=3;Pose pose;};
static constexpr int XR_TYPE_SPACE_LOCATION=0,XR_SPACE_LOCATION_POSITION_VALID_BIT=1,XR_SPACE_LOCATION_ORIENTATION_VALID_BIT=2;
static constexpr int XR_NULL_HANDLE=0,XR_SESSION_STATE_FOCUSED=5;
#define XR_SUCCEEDED(x) ((x)==0)
static int g_xrRightAimSpace=1,g_xrGuiViewSpace=1,g_xrSessionState=5;
static Pose rayPose;
static int xrLocateSpace(int,int,XrTime,XrSpaceLocation* l){l->pose=rayPose;l->locationFlags=3;return 0;}
struct Controller {float trigger=0,squeeze=0;bool aimPoseValid=true;};
static Controller g_xrControllerHands[2];
static struct{Pose pose;struct{float width=1.2f,height=.675f;}size;}g_xrGuiLayer;
static float g_xrVrMenuPointerOffsetX=0;
static void vrLoadMenuPointerOffset(){}
static int clientnum=0;
struct Item{int type=0;const char* getName()const{return "Test item";}};
static constexpr int SHORTBOW=10;
struct Stat{Item* weapon=nullptr;Item* shield=nullptr;};static Stat stat;static Stat* stats[]={&stat};
struct Transform{float forward=0,up=0,side=0,yaw=0,pitch=0,roll=0;};
struct Hud:Transform{float scale=1;};struct Aim{float yaw=0,pitch=0;};
static Transform g_vrArmVisual[2],g_vrBowGrip[2];
static std::unordered_map<int,Transform>g_vrItemVisual[2];static std::unordered_map<int,Aim>g_vrRangedAimVisual;
static float g_vrLeftArmScaleFactor=1.65f,g_vrRightArmScaleFactor=1,g_vrBowFullDraw=.45f,g_vrBowZRotation[2]{};
static float g_vrFlailModelSize=1.5f,g_vrFlailChainLength=1.8f,g_vrFlailDamping=.45f,g_vrFlailAnchorForward=2.4f,g_vrFlailAnchorUp=.24f,g_vrFlailAnchorSide=0;
static Hud g_vrWearableHud[2],g_vrStatusEffectsHud,g_vrSpatialHud[6];static Aim g_vrSpellAimVisual;
static constexpr float VR_SPELL_AIM_MAX_YAW=.85f,VR_SPELL_AIM_MAX_PITCH=.35f;
static int g_vrArmCalibrationSelected=1,g_vrArmCalibrationSpatialPage=-1,g_vrLeftArmMeshMode=3,g_vrBowDrawHand=1;
static bool g_vrArmCalibrationActive=true,g_vrBodyCalibrationPage=false,g_vrArmCalibrationEditMelee=false;
static bool g_vrArmCalibrationEditItem=false,g_vrArmCalibrationEditAim=false,g_vrArmCalibrationEditSpellAim=false;
static bool g_vrArmCalibrationEditWearable=false,g_vrArmCalibrationEditStatusEffects=false,g_vrArmCalibrationEditBow=false,g_vrBowEditGrip=false,g_vrArmCalibrationEditFlail=false;
static bool g_vrArmCalibrationDirty=false,g_vrFlailSettingsDirty=false,g_vrMeleeTuningDirty=false;
static struct{float minTipSpeed=1.7f,minGripSpeed=.8f,rearmSpeed=.26f;}g_vrMeleeTuning;
static struct{bool drawing=false,armed=false;float draw=0;unsigned releases=0;}g_vrBowPreview;
static struct{bool enabled=true;float measuredReachMeters[2]{.6f,.6f};}g_vrBodyFit;
static bool g_vrBodyPoseLatched=false,g_vrBodyResetLatched=false;
static int equipmentSaves=0,modeSaves=0,meleeSaves=0,bodyResets=0;static float bowRotation[2][3]{};
static void vrSaveVisualArmCalibration(){++equipmentSaves;g_vrArmCalibrationDirty=false;}
static void vrSaveModeSettings(){++modeSaves;g_vrFlailSettingsDirty=false;}
static void vrSaveMeleeTuning(){++meleeSaves;g_vrMeleeTuningDirty=false;}
static void vrLoadBodyFit(){}static bool vrSaveBodyFit(){return true;}
static void vrResetBodyPoseSamples(){}
static void vrUpdateBodyPoseCalibration(XrTime){if(g_xrControllerHands[1].trigger>.9f && !g_vrBodyResetLatched)++bodyResets;}
static const Item* vrCalibrationEquippedItem(int h){return h?stat.weapon:stat.shield;}
static const Item* vrCalibrationRangedItem(){return g_vrArmCalibrationSelected==1?stat.weapon:nullptr;}
static bool vrBowEquipped(){return stat.weapon && stat.weapon->type==SHORTBOW;}
static void vrBowAdjustControllerRotation(int h,int a,float delta){bowRotation[h][a]+=delta;}
static unsigned g_xrGuiFramebuffer=1;bool drawingGui=false;
namespace Shader {static void unbind(){}}
namespace VrNativeHud{struct Surface{unsigned framebuffer=1;int width=1280,height=720;};struct GlState{};
 template<class F>bool render(Surface&,Surface&,SDL_Rect,int,int,F f){f();return true;}}
static constexpr int GL_READ_FRAMEBUFFER=0,GL_DRAW_FRAMEBUFFER=1,GL_COLOR_ATTACHMENT0=2,GL_SCISSOR_TEST=3,GL_COLOR_BUFFER_BIT=4,GL_LINEAR=5;
static void glBindFramebuffer(int,unsigned){}static void glReadBuffer(int){}static void glDrawBuffer(int){}static void glDisable(int){}
static void glBlitFramebuffer(int,int,int,int,int,int,int,int,int,int){}
static bool g_vrRoomScaleEnabled=false;
static void vrRequestRoomCalibration(){}
namespace VrRoomScale{static void setEnabled(bool enabled){g_vrRoomScaleEnabled=enabled;}}
static bool g_vrVisibilityProbeFull=false,g_vrSpatialHudProbeHidden=false;
static void printlog(const char*,...){}
#include "../../src/vr_calibration_menu.hpp"
static void aim(float x,float y,float trigger){rayPose.position={(x/1280.f-.5f)*1.2f,(.5f-y/720.f)*.675f,0};g_xrControllerHands[1].trigger=trigger;VrCalibrationMenu::input(1);}
static void visit(int t,int s){VrCalibrationMenu::action(t);VrCalibrationMenu::action(10+s);VrCalibrationMenu::build();aim(0,0,0);}
int main(){using namespace VrCalibrationMenu;g_xrGuiLayer.pose.position.z=-1;
 open();action(30);input(1);assert(!g_vrArmCalibrationDirty);draw();assert(cacheBuilds>8);
 assert(edits.size()==7 && edits[0].data==&g_vrLeftArmScaleFactor);
 assert(std::string(section(2))=="Room-scale tracking" && section(3)==nullptr);
 action(31);input(1);build();assert(edits.size()==7 && edits[0].data==&g_vrRightArmScaleFactor);
 action(30);input(1);build();
 const int model=g_vrLeftArmMeshMode;action(50);assert(g_vrLeftArmMeshMode==model);aim(0,0,0);

 // Independent arm sizes and held-drag capture.
 const float oldRight=g_vrRightArmScaleFactor;
 aim(825,250,1);assert(g_vrLeftArmScaleFactor>2.f);assert(g_vrRightArmScaleFactor==oldRight);
 const float left=g_vrLeftArmScaleFactor;aim(900,290,1);
 assert(g_vrLeftArmScaleFactor>left);assert(g_vrRightArmScaleFactor==oldRight);
 g_xrControllerHands[1].aimPoseValid=false;input(1);const float cancelled=g_vrLeftArmScaleFactor;
 g_xrControllerHands[1].aimPoseValid=true;aim(700,250,1);assert(g_vrLeftArmScaleFactor==cancelled);
 aim(0,0,0);
 // Read-only fields are cached, and values contain units.
 draw();assert(edits[0].value->text.find("x")!=std::string::npos);
 visit(2,3);assert(g_vrArmCalibrationSpatialPage==0);aim(850,490,1);
 assert(g_vrSpatialHud[0].scale!=1);assert(g_vrSpatialHud[3].scale==1);
 aim(0,0,0);visit(2,6);assert(g_vrArmCalibrationSpatialPage==5);
 // Equipment profiles remain independent from arms and other items.
 Item weapon{4};stat.weapon=&weapon;visit(1,0);hand=1;frameTab=-1;build();aim(0,0,0);
 const float arm=g_vrArmVisual[1].forward;aim(850,250,1);
 assert(g_vrItemVisual[1][4].forward!=0);assert(g_vrArmVisual[1].forward==arm);
 aim(0,0,0);visit(1,1);assert(edits.size()==2);aim(850,250,1);assert(g_vrRangedAimVisual[4].yaw!=0);
 aim(0,0,0);visit(1,2);assert(g_vrArmCalibrationEditSpellAim);assert(edits.size()==2);
 // Bow alignment edits use controller-local deltas, with no reset of pose.
 weapon.type=SHORTBOW;visit(1,3);assert(edits.size()==7 && g_vrBowEditGrip);
 bowRotation[0][1]=.7f;aim(850,370,1);assert(bowRotation[0][1]>.7f);
 aim(0,0,0);visit(1,4);assert(!g_vrBowEditGrip && g_vrArmCalibrationEditBow);
 action(60);assert(g_vrBowDrawHand==0);assert(g_vrArmCalibrationDirty);
 // Removed pages are absent and cannot be selected; flail settings remain untouched.
 visit(1,0);assert(section(5)==nullptr);action(15);assert(sub==0);
 assert(!g_vrArmCalibrationEditFlail && !g_vrFlailSettingsDirty);
 aim(0,0,0);visit(3,2);aim(825,250,1);assert(g_vrMeleeTuningDirty);
 aim(0,0,0);visit(0,1);aim(825,250,1);assert(bodyResets==0);
 // All category pages can build/draw and every native text field has a cache.
 for(int t=0;t<4;++t){tab=t;for(int i=0;i<7 && section(i);++i){visit(t,i);draw();}}
 g_vrArmCalibrationDirty=true;g_vrFlailSettingsDirty=true;g_vrMeleeTuningDirty=true;
 const int a=equipmentSaves,b=modeSaves,c=meleeSaves;action(80);
 assert(!g_vrArmCalibrationActive && equipmentSaves==a+1 && modeSaves==b+1 && meleeSaves==c+1);
 tab=3;sub=5;frameTab=-1;build();assert(std::string(section(5))=="Visibility tests");
 action(93);assert(g_vrVisibilityProbeFull && !g_vrSpatialHudProbeHidden);
 action(94);assert(g_vrSpatialHudProbeHidden);
 action(95);assert(!g_vrVisibilityProbeFull && !g_vrSpatialHudProbeHidden);
 std::cout<<"PASS: all pages, native text-cache preparation, units, independent profiles, captured drag, tracking cancellation, HUD routing, bow quaternion deltas, save stores and protected body reset.\n";

 return 0;
}
