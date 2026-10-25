#define WIN32_LEAN_AND_MEAN
#define XR_USE_PLATFORM_WIN32
#include <windows.h>
#include <unknwn.h>
#include <GL/glew.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <vector>
#include <utility> // STEP638CZ: store previous HUD flame visibility per eye.
#include <cmath>
#include <openxr/openxr.h>
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr_platform.h>
#include <cstdio>
#include <cstring>
#include <cstdlib> // STEP638FP: optional desktop-only launch environment variable.
#include "main.hpp"
#include "game.hpp" // intro, gamePaused, clientnum and gameplay UI state.
#include "net.hpp" // Declares messagePlayer() used by the arm-calibration HUD.
#include "messages.hpp" // Show calibration mode/values in the in-headset HUD.
#include "player.hpp" // Use Barony's native learned-spells UI (gamepad Spell List is normally unbound).
#include "interface/interface.hpp" // Detect native shrine and interaction overlays (GenericGUI).
#include "stat.hpp" // Offhand equipment check for crouch-to-sneak safety.
#include "items.hpp" // Use the actual item category when selecting tracked equipment.
#include "entity.hpp" // Render the existing first-person hand entity as tracked 3D geometry.
#include "mod_tools.hpp" // Read native breakable-collider definitions for training barrel.
#include "magic/magic.hpp" // Barony magic projectile XYZ motion and casting state.
#include "draw.hpp"
#include "ui/Image.hpp"
#include "shader.hpp"
#include "vr_native_hud.hpp"
#include "ui/Frame.hpp" // STEP638EI: real HP/MP and minimap frame rectangles.
#include "ui/GameUI.hpp" // STEP638FB: original StatusEffectQueue icon artwork and layout.
#include "ui/MainMenu.hpp" // STEP 624: Detect the real character-creation lobby.
// Barony's existing game pause state.
extern bool gamePaused;
// VR-specific viewport control defined in opengl.cpp.
struct view_t;
extern bool vrCameraViewportEnabled;
// Controls headset tracking for VR camera rendering.
extern bool vrCameraTrackingEnabled;
// STEP 616: Barony's local player appends several HUD entities to the
// bodyparts vector BEFORE the actual torso/legs/arms. Therefore indices 4
// and 5 are NOT the world right/left arms. Locate real limbs by their
// actPlayerLimb behavior, then use the creation order from actPlayer():
// torso=1, right leg=2, left leg=3, RIGHT ARM=4, LEFT ARM=5.
// This remains correct when HUD entries or the player's race/equipment change.
// STEP638FI: Ordinals are world-model parts, NOT indices in bodyparts.
// Retain all original limb entities for co-op replication; VR only borrows
// their current race/armor sprite and transform during the eye draw.
static Entity* vrFindNativePlayerLimb(Entity* playerEntity, int wantedLimb)
{
    if (!playerEntity || wantedLimb < 1 || wantedLimb > 5)
        return nullptr;
    int limbOrdinal = 0;
    for (Entity* part : playerEntity->bodyparts)
        if (part && part->behavior == &actPlayerLimb &&
            ++limbOrdinal == wantedLimb)
            return part;
    return nullptr;
}
static Entity* vrFindNativePlayerArm(Entity* playerEntity, bool leftHand)
{
    return vrFindNativePlayerLimb(playerEntity, leftHand ? 5 : 4);
}


// OpenXR per-eye projection override, defined in opengl.cpp.
extern bool vrEyeProjectionEnabled;
extern float vrEyeProjectionMatrix[16];
// Build an OpenGL projection matrix from an OpenXR eye's field of view.
// Output is column-major, matching Barony's shader matrices.
static void vrBuildEyeProjection(
    const XrFovf& fov,
    float nearPlane,
    float farPlane,
    float projection[16])
{
    const float left = tanf(fov.angleLeft);
    const float right = tanf(fov.angleRight);
    const float bottom = tanf(fov.angleDown);
    const float top = tanf(fov.angleUp);

    const float width = right - left;
    const float height = top - bottom;
    const float depth = farPlane - nearPlane;

    for (int i = 0; i < 16; ++i)
    {
        projection[i] = 0.0f;
    }

    projection[0] = 2.0f / width;
    projection[5] = 2.0f / height;
    projection[8] = (right + left) / width;
    projection[9] = (top + bottom) / height;
    projection[10] = -(farPlane + nearPlane) / depth;
    projection[11] = -1.0f;
    projection[14] = -(2.0f * farPlane * nearPlane) / depth;
}
// Convert an OpenXR orientation quaternion into a column-major
// OpenGL view rotation matrix (inverse headset orientation).
static void vrBuildEyeViewRotation(
    const XrQuaternionf& orientation,
    float view[16])
{
    const float x = orientation.x;
    const float y = orientation.y;
    const float z = orientation.z;
    const float w = orientation.w;

    const float xx = x * x;
    const float yy = y * y;
    const float zz = z * z;
    const float xy = x * y;
    const float xz = x * z;
    const float yz = y * z;
    const float wx = w * x;
    const float wy = w * y;
    const float wz = w * z;

    // Transpose the headset rotation to obtain its inverse.
    view[0] = 1.0f - 2.0f * (yy + zz);
    view[1] = 2.0f * (xy - wz);
    view[2] = 2.0f * (xz + wy);
    view[3] = 0.0f;

    view[4] = 2.0f * (xy + wz);
    view[5] = 1.0f - 2.0f * (xx + zz);
    view[6] = 2.0f * (yz - wx);
    view[7] = 0.0f;

    view[8] = 2.0f * (xz - wy);
    view[9] = 2.0f * (yz + wx);
    view[10] = 1.0f - 2.0f * (xx + yy);
    view[11] = 0.0f;

    view[12] = 0.0f;
    view[13] = 0.0f;
    view[14] = 0.0f;
    view[15] = 1.0f;
}
// Apply the inverse of an OpenXR eye position to a
// column-major OpenGL view rotation matrix.
static void vrApplyEyeViewTranslation(
    const XrVector3f& position,
    float view[16])
{
    const float x = position.x;
    const float y = position.y;
    const float z = position.z;

    view[12] = -(view[0] * x + view[4] * y + view[8] * z);
    view[13] = -(view[1] * x + view[5] * y + view[9] * z);
    view[14] = -(view[2] * x + view[6] * y + view[10] * z);
    view[15] = 1.0f;
}

// Persistent OpenXR handles
static XrInstance g_xrInstance = XR_NULL_HANDLE;
static XrSystemId g_xrSystemId = XR_NULL_SYSTEM_ID;
static XrSession g_xrSession = XR_NULL_HANDLE;
// Reference space used for headset tracking and stereo rendering.
static XrSpace g_xrReferenceSpace = XR_NULL_HANDLE;
// Headset-relative space for the VR interface.
static XrSpace g_xrGuiViewSpace = XR_NULL_HANDLE;
// OpenXR action set for Quest 3 controller input.
static XrActionSet g_xrControllerActionSet = XR_NULL_HANDLE;
// OpenXR paths identifying the player's left and right hands.
static XrPath g_xrLeftHandPath = XR_NULL_PATH;
static XrPath g_xrRightHandPath = XR_NULL_PATH;
// OpenXR action for tracking the left controller's 6DoF pose.
static XrAction g_xrLeftHandPoseAction = XR_NULL_HANDLE;
// OpenXR action for tracking the right controller's 6DoF pose.
static XrAction g_xrRightHandPoseAction = XR_NULL_HANDLE;
// OpenXR analog thumbstick input for both Quest 3 controllers.
static XrAction g_xrThumbstickAction = XR_NULL_HANDLE;
// OpenXR analog trigger input for both Quest 3 controllers.
static XrAction g_xrTriggerAction = XR_NULL_HANDLE;
// Analog grip/squeeze input, tracked independently for both hands.
static XrAction g_xrGripAction = XR_NULL_HANDLE;
// Additional Quest Touch input actions. Each face button belongs to one hand.
static XrAction g_xrXButtonAction = XR_NULL_HANDLE;
static XrAction g_xrYButtonAction = XR_NULL_HANDLE;
static XrAction g_xrAButtonAction = XR_NULL_HANDLE;
static XrAction g_xrBButtonAction = XR_NULL_HANDLE;
static XrAction g_xrLeftMenuAction = XR_NULL_HANDLE;
static XrAction g_xrThumbstickClickAction = XR_NULL_HANDLE;
static XrAction g_xrRightThumbrestAction = XR_NULL_HANDLE;
// Independent aim poses for aiming bows, spells, pointers, and ranged weapons.
static XrAction g_xrLeftAimPoseAction = XR_NULL_HANDLE;
static XrAction g_xrRightAimPoseAction = XR_NULL_HANDLE;
// Haptic output is defined now so future hand-specific rumble needs no new action.
static XrAction g_xrHapticAction = XR_NULL_HANDLE;
static bool g_xrControllerBindingsSuggested = false;
static bool g_xrControllerActionsAttached = false;

struct VrControllerHandState
{
    XrVector2f thumbstick{};
    float trigger = 0.0f;
    float squeeze = 0.0f;
    bool thumbstickClick = false;
    bool thumbrestAvailable = false, thumbrestTouch = false;
    bool primaryButton = false;   // Left: X, Right: A.
    bool secondaryButton = false; // Left: Y, Right: B.
    bool menuButton = false;      // Left hand only; right system button is reserved.
    bool gripPoseValid = false;
    bool aimPoseValid = false;
    XrPosef gripPose{};
    XrPosef aimPose{};
};
static VrControllerHandState g_xrControllerHands[2]{};
// Oculus Touch interaction profile for Quest controllers.
// OpenXR action spaces for independent 6DoF hand tracking.
// These will be created after the VR session is initialized.
static XrSpace g_xrLeftHandSpace = XR_NULL_HANDLE;
static XrSpace g_xrRightHandSpace = XR_NULL_HANDLE;
static XrSpace g_xrLeftAimSpace = XR_NULL_HANDLE;
static XrSpace g_xrRightAimSpace = XR_NULL_HANDLE;
static XrPath g_xrTouchControllerProfilePath = XR_NULL_PATH;
// Whether both eyes have valid tracking for the current frame.
static bool g_xrEyeViewsValid = false;
// Whether both eye textures were rendered successfully this frame.
static bool g_xrStereoFrameReady = false;
// Headset tracking information for the left and right eyes.
static XrView g_xrViews[2] = {
    { XR_TYPE_VIEW },
    { XR_TYPE_VIEW }
};
// Projection information for the left and right eyes.
static XrCompositionLayerProjectionView g_xrProjectionViews[2] = {
    { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
    { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }
};
// Projection layer that will contain both stereo eye views.
static XrCompositionLayerProjection g_xrProjectionLayer = {
    XR_TYPE_COMPOSITION_LAYER_PROJECTION
};
static XrSwapchain g_xrSwapchain = XR_NULL_HANDLE;
// Separate OpenXR swapchain for Barony's VR interface.
static XrSwapchain g_xrGuiSwapchain = XR_NULL_HANDLE;
// OpenGL textures belonging to the separate GUI swapchain.
static std::vector<XrSwapchainImageOpenGLKHR> g_xrGuiSwapchainImages;
// OpenXR quad layer for displaying Barony's GUI in VR.
static XrCompositionLayerQuad g_xrGuiLayer{
    XR_TYPE_COMPOSITION_LAYER_QUAD
};
// OpenGL framebuffer used to render into VR swapchain textures.
static GLuint g_xrFramebuffer = 0;
// Separate framebuffer for rendering the VR GUI.
static GLuint g_xrGuiFramebuffer = 0;
// Barony's original mouse cursor artwork.
// This will be drawn into the floating VR GUI texture.
static const char* g_xrMenuCursorImagePath =
"images/system/cursor_hand.png";
// Barony's loaded mouse cursor image for the VR interface.
static Image* g_xrMenuCursorImage = nullptr;
// OpenGL depth-stencil renderbuffer for VR 3D rendering.
static GLuint g_xrDepthRenderbuffer = 0;
// Function pointer for creating OpenGL framebuffer objects.
static PFNGLGENFRAMEBUFFERSPROC g_glGenFramebuffers = nullptr;
// Function pointer for binding an OpenGL framebuffer.
static PFNGLBINDFRAMEBUFFERPROC g_glBindFramebuffer = nullptr;
// Function pointer for attaching a VR eye texture layer to a framebuffer.
static PFNGLFRAMEBUFFERTEXTURELAYERPROC g_glFramebufferTextureLayer = nullptr;
// Function pointer for checking whether the VR framebuffer is complete.
static PFNGLCHECKFRAMEBUFFERSTATUSPROC g_glCheckFramebufferStatus = nullptr;
// Function pointer for creating OpenGL renderbuffers.
static PFNGLGENRENDERBUFFERSPROC g_glGenRenderbuffers = nullptr;

// Function pointer for binding an OpenGL renderbuffer.
static PFNGLBINDRENDERBUFFERPROC g_glBindRenderbuffer = nullptr;

// Function pointer for allocating depth-stencil renderbuffer storage.
static PFNGLRENDERBUFFERSTORAGEPROC g_glRenderbufferStorage = nullptr;

// Function pointer for attaching a renderbuffer to a framebuffer.
static PFNGLFRAMEBUFFERRENDERBUFFERPROC g_glFramebufferRenderbuffer = nullptr;
static std::vector<XrSwapchainImageOpenGLKHR> g_xrSwapchainImages;
static int64_t g_xrSwapchainFormat = 35907; // GL_SRGB8_ALPHA8
static uint32_t g_xrEyeWidth = 3760;
static uint32_t g_xrEyeHeight = 3996;
void vrCheckSwapchainFormats();
void vrCheckViewConfiguration();
void vrCreateSwapchain();
void vrGetSwapchainImages();
void vrTestAcquireSwapchainImage();
void vrRenderStereoTestFrame(view_t* camera);
// Retrieve the tracked positions and orientations of both eyes.
void vrLocateEyeViews(XrTime predictedDisplayTime);
static XrSessionState g_xrSessionState = XR_SESSION_STATE_UNKNOWN;
static bool g_xrSessionRunning = false;
// STEP 630: Free-swing melee. Barony's user-selected crosshair is now the
// charge HUD only; the normal controller-aim reticle is still used for ranged
// weapons, throwable items, and casting. No new artwork is required.
static bool g_vrMeleeChargeVisible = false;
static float g_vrMeleeChargeFraction = 0.0f;
// STEP 638DV: Keep the right-hand weapon and left-hand spell guides separate.
// Both can be visible when a ranged weapon and projectile spell are readied.
static bool g_vrWeaponAimReticleAvailable = false;
static int g_vrWeaponAimReticleX = 640, g_vrWeaponAimReticleY = 360;
static bool g_vrSpellAimReticleAvailable = false;
static int g_vrSpellAimReticleX = 640, g_vrSpellAimReticleY = 360;
// STEP 638DD: Cosmetic timed-block feedback. No combat or input changes.
static bool g_vrTimedBlockFlashActive = false;
static Uint32 g_vrTimedBlockFlashTick = 0;

bool vrCustomCrosshairModeActive()
{
    return g_xrSessionRunning && g_xrSessionState == XR_SESSION_STATE_FOCUSED;
}

// This is called only when the vanilla damage path records a zero-damage
// defended hit. The game's own defendTicks defines the 1/3-second window.
// It does NOT change incoming damage, shield durability, or skill rolls.
void vrNotifyTimedBlock(int playerIndex)
{
    if (!vrCustomCrosshairModeActive() || playerIndex != clientnum ||
        playerIndex < 0 || playerIndex >= MAXPLAYERS ||
        !players[playerIndex] || !stats[playerIndex] ||
        !stats[playerIndex]->defending || !stats[playerIndex]->shield)
        return;
    const Uint32 raisedAt = players[playerIndex]->mechanics.defendTicks;
    if (!raisedAt || (::ticks - raisedAt) >= (TICKS_PER_SECOND / 3))
        return;
    g_vrTimedBlockFlashTick = ::ticks;
    g_vrTimedBlockFlashActive = true;
}

// Kept as a linkage point for Step 629's Entity::attack() hooks. Melee no
// longer shows hit confirmation: the selected crosshair is ONLY charge HUD.
void vrNotifyMeleeHit(int, bool) {}

// Render the original selected crosshair texture twice: a dim whole texture,
// then a bottom-up colored fill, clipped to the artwork's actual alpha pixels.
// This deliberately does not alter Barony's crosshair asset or any settings.
static void vrDrawSelectedCrosshairOnGui()
{
    if (!vrCustomCrosshairModeActive() || intro || gamePaused ||
        clientnum < 0 || clientnum >= MAXPLAYERS ||
        !players[clientnum] || !players[clientnum]->shootmode)
        return;

    // STEP 638ED: Aim guides render only in the stereo world. Preserve this
    // GUI pass exclusively for the existing blocking and melee-charge cues.
    const bool blockVisible = stats[clientnum] &&
        stats[clientnum]->defending && stats[clientnum]->shield;
    const bool chargeVisible = !blockVisible && g_vrMeleeChargeVisible;
    if (!blockVisible && !chargeVisible) return;
    const bool timedBlockFlash = blockVisible && g_vrTimedBlockFlashActive &&
        (::ticks - g_vrTimedBlockFlashTick) < (TICKS_PER_SECOND / 4);

    Image* img = Image::get(players[clientnum]->hud.getCrosshairPath());
    if (!img || img->getWidth() == 0 || img->getHeight() == 0) return;

    const int sizeX = static_cast<int>(img->getWidth());
    const int sizeY = static_cast<int>(img->getHeight());
    const int x = 640;
    const int y = 360;
    const int setting = playerSettings[multiplayer ? 0 : clientnum].shootmodeCrosshairOpacity;
    const int alpha = std::max(0, std::min(255, setting * 255 / 100));
    if (alpha <= 0) return;

    GLint oldViewport[4] = {}, oldScissorBox[4] = {};
    GLint oldProgram = 0, oldVAO = 0, oldArrayBuffer = 0;
    GLint oldActiveTexture = 0, oldTexture = 0;
    GLint oldBlendSrcRGB = 0, oldBlendDstRGB = 0;
    GLint oldBlendSrcAlpha = 0, oldBlendDstAlpha = 0;
    GLboolean oldColorMask[4] = {};
    glGetIntegerv(GL_VIEWPORT, oldViewport);
    glGetIntegerv(GL_SCISSOR_BOX, oldScissorBox);
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVAO);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldArrayBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
    glGetIntegerv(GL_BLEND_SRC_RGB, &oldBlendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &oldBlendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &oldBlendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &oldBlendDstAlpha);
    glGetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);
    const GLboolean oldBlend = glIsEnabled(GL_BLEND);
    const GLboolean oldDepth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean oldScissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean oldCull = glIsEnabled(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);

    glViewport(0, 0, 1280, 720);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    const SDL_Rect position{ x-sizeX/2, y-sizeY/2, sizeX, sizeY };
    const SDL_Rect screen{ 0, 0, 1280, 720 };
    if (blockVisible)
    {
        img->drawColor(nullptr, position, screen,
            timedBlockFlash ? makeColor(255, 218, 50, alpha) :
                makeColor(45, 165, 255, alpha));
    }
    else if (chargeVisible)
    {
        img->drawColor(nullptr, position, screen,
            makeColor(108, 116, 132, alpha));
        const float fraction = std::max(0.f, std::min(1.f, g_vrMeleeChargeFraction));
        const int fillHeight = std::min(sizeY,
            static_cast<int>(ceilf(fraction * static_cast<float>(sizeY))));
        if (fillHeight > 0)
        {
            // OpenGL glScissor has bottom-left origin, whereas this GUI's
            // rectangles have top-left origin; fill from bottom upwards.
            const int scissorX = std::max(0, position.x);
            const int scissorY = std::max(0, 720-(position.y+sizeY));
            const int scissorW = std::max(0, std::min(1280, position.x+sizeX)-scissorX);
            const int scissorH = std::max(0, std::min(fillHeight, 720-scissorY));
            if (scissorW > 0 && scissorH > 0)
            {
                glEnable(GL_SCISSOR_TEST);
                glScissor(scissorX, scissorY, scissorW, scissorH);
                const Uint32 color = fraction >= 1.f ?
                    makeColor(105, 245, 151, alpha) : makeColor(255, 190, 75, alpha);
                img->drawColor(nullptr, position, screen, color);
                glDisable(GL_SCISSOR_TEST);
            }
        }
    }

    glUseProgram(static_cast<GLuint>(oldProgram));
    glBindVertexArray(static_cast<GLuint>(oldVAO));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(oldArrayBuffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture));
    glActiveTexture(static_cast<GLenum>(oldActiveTexture));
    glBlendFuncSeparate(oldBlendSrcRGB, oldBlendDstRGB,
        oldBlendSrcAlpha, oldBlendDstAlpha);
    glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
    glScissor(oldScissorBox[0], oldScissorBox[1], oldScissorBox[2], oldScissorBox[3]);
    glColorMask(oldColorMask[0], oldColorMask[1], oldColorMask[2], oldColorMask[3]);
    if (oldBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (oldDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (oldScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (oldCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
}
static unsigned long long g_xrFrameCount = 0;

// VR gamepad compatibility bridge. The OpenXR hand states remain independent;
// this only exposes their conventional controls to Barony's existing SDL path.
// Disable this single flag to run VR using keyboard/mouse only.
static constexpr bool g_xrEnableVirtualSDLGamepad = true;
static int g_xrVirtualSDLDeviceIndex = -1;
static SDL_Joystick* g_xrVirtualSDLJoystick = nullptr;

// STEP 624: Only the character-creation lobby opts back into native gamepad
// selection. The other menus must remain mouse-driven (Step 623).
static bool vrCharacterCreationLobbyActive()
{
    return intro && MainMenu::main_menu_frame &&
        MainMenu::main_menu_frame->findFrame("lobby") != nullptr;
}

// Barony deliberately unassigns controllers when entering its lobby. Bind the
// VR virtual gamepad to the local slot only after Barony has opened it and
// only when that slot does not already own a controller. Do not touch other
// gamepads or take a controller away from another splitscreen player.
static void vrBindVirtualControllerInLobby()
{
    if (!vrCharacterCreationLobbyActive() || !g_xrVirtualSDLJoystick ||
        clientnum < 0 || clientnum >= MAXPLAYERS ||
        inputs.hasController(clientnum))
        return;

    const SDL_JoystickID instance = SDL_JoystickInstanceID(g_xrVirtualSDLJoystick);
    if (instance < 0)
        return;
    for (int p = 0; p < MAXPLAYERS; ++p)
    {
        if (p != clientnum && inputs.getControllerID(p) == instance)
            return;
    }
    for (auto& controller : game_controllers)
    {
        if (controller.getID() == instance && controller.getControllerDevice())
        {
            inputs.setControllerID(clientnum, instance);
            // Non-local multiplayer routes controller ownership through
            // player 0 in Barony's Inputs class.
            const int inputPlayer = multiplayer == SINGLE ? clientnum : 0;
            Input::inputs[inputPlayer].refresh();
            std::printf("OpenXR VR: STEP624 QUEST GAMEPAD bound to lobby player %d (device=%d)\n",
                clientnum, static_cast<int>(instance));
            return;
        }
    }
}


// STEP 592: Locomotion profiles are independent of the standard SDL gamepad.
// Keep the saved setting beside Barony's normal working-directory files.
// The first run defaults to SEATED: physical movement is opt-in.
enum class VrPlayMode { Seated, Standing };
static VrPlayMode g_xrPlayMode = VrPlayMode::Seated;
static bool g_xrModeSettingsLoaded = false;
static bool g_xrRoomRecalibrate = true;
static bool g_xrRoomCalibrated = false;
static bool g_xrRoomWasInDungeon = false;
static bool g_xrRoomHavePreviousPose = false;
static bool g_xrPhysicalSneaking = false;
static bool g_xrRoomSneakLatched = false;
static bool g_xrRoomSneakLogged = false;
static float g_xrRoomCalibratedHeight = 0.0f;
static float g_xrRoomBaselineForwardX = 0.0f;
static float g_xrRoomBaselineForwardZ = -1.0f;
static float g_xrRoomBaselinePlayerYaw = 0.0f;
static float g_xrRoomStickX = 0.0f;
static float g_xrRoomStickY = 0.0f;
static XrVector3f g_xrRoomPreviousHead{};
static XrTime g_xrRoomPreviousTime = 0;
// STEP638FX: The vanilla Hermit Fowl Spirit owns the dungeon camera during
// projection. Remember the transition out so VR can use the original body
// viewpoint while Barony's ordinary camera catches up; never edit game state.
static bool g_vrHermitReturnCameraPending = false;
// STEP638FY: Vanilla sets the Project Spirit body's pitch to PI/4 while
// resting, but does not restore that pitch when the spirit expires. Preserve
// the Hermit's PRE-SPELL look pitch and restore only on the spell's exit.
// VR-only local-character state; Shaman transformations are not affected.
static bool g_vrHermitPreSpellPitchValid = false;
static double g_vrHermitPreSpellPitch = 0.0;
static bool g_xrVrMenuChordActive = false;
static bool g_xrVrMenuChordHeldAction = false;
static Uint32 g_xrVrMenuChordStartMs = 0;

static const char* vrPlayModeName()
{
    return g_xrPlayMode == VrPlayMode::Standing ? "STANDING" : "SEATED";
}

// STEP638FO: Explicit desktop opt-out and independent visual flail preview.
// Defaults retain VR auto-detection and enable the cosmetic preview.
static bool g_vrUserEnabled = true;
static bool g_vrFlailVisualEnabled = true;
// Cosmetic-only flail pivot and chain tuning in Barony world units.
static float g_vrFlailAnchorForward = 2.4f;
static float g_vrFlailAnchorUp = 0.240f; // STEP638FP: headset-tested starting pivot.
static float g_vrFlailChainLength = 1.8f;
static float g_vrFlailAnchorSide = 0.f;
static float g_vrFlailDamping = 0.45f;
// STEP638FV: Temporary in-headset visual-only original voxel size control.
// 1.5 = Step638FU's tested 150% rendered size; 1.0 = original Step638FT.
// glDrawVoxel applies voxel scale twice; the renderer takes sqrt of this.
static float g_vrFlailModelSize = 1.5f;
static bool g_vrFlailSettingsDirty = false;
static bool g_vrFeatureSettingsRead = false;
static void vrReadFeatureSettings()
{
    if (g_vrFeatureSettingsRead) return;
    g_vrFeatureSettingsRead = true;
    FILE* file = std::fopen("vr_settings.cfg", "r");
    if (!file) return;
    char line[128]{};
    while (std::fgets(line, sizeof(line), file))
    {
        if (std::strncmp(line, "vr_enabled=0", 12) == 0)
            g_vrUserEnabled = false;
        else if (std::strncmp(line, "vr_enabled=1", 12) == 0)
            g_vrUserEnabled = true;
        else if (std::strncmp(line, "flail_visual=0", 14) == 0)
            g_vrFlailVisualEnabled = false;
        else if (std::strncmp(line, "flail_visual=1", 14) == 0)
            g_vrFlailVisualEnabled = true;
        else
        {
            float value = 0.f;
            if (std::sscanf(line, "flail_anchor_forward=%f", &value) == 1)
                g_vrFlailAnchorForward = std::max(0.f, std::min(8.f, value));
            else if (std::sscanf(line, "flail_anchor_up=%f", &value) == 1)
                g_vrFlailAnchorUp = std::max(-4.f, std::min(4.f, value));
            else if (std::sscanf(line, "flail_chain_length=%f", &value) == 1)
                g_vrFlailChainLength = std::max(0.5f, std::min(5.f, value));
            else if (std::sscanf(line, "flail_anchor_side=%f", &value) == 1)
                g_vrFlailAnchorSide = std::max(-4.f, std::min(4.f, value));
            else if (std::sscanf(line, "flail_damping=%f", &value) == 1)
                g_vrFlailDamping = std::max(0.1f, std::min(5.f, value));
            else if (std::sscanf(line, "flail_model_size=%f", &value) == 1 &&
                std::isfinite(value))
                g_vrFlailModelSize = std::max(0.6f, std::min(3.f, value));
        }
    }
    std::fclose(file);
}

static void vrSaveModeSettings()
{
    vrReadFeatureSettings();
    FILE* file = std::fopen("vr_settings.cfg", "w");
    if (file)
    {
        std::fprintf(file,
            "# Barony VR: vr_enabled=0 forces desktop mode; 1 detects HMD.\n"
            "# flail_visual=0 disables only the experimental cosmetic chain.\n"
            "vr_enabled=%d\nflail_visual=%d\n"
            "flail_anchor_forward=%.3f\nflail_anchor_up=%.3f\n"
            "flail_chain_length=%.3f\nflail_anchor_side=%.3f\n"
            "flail_damping=%.3f\nflail_model_size=%.3f\nmode=%s\n",
            g_vrUserEnabled ? 1 : 0, g_vrFlailVisualEnabled ? 1 : 0,
            g_vrFlailAnchorForward, g_vrFlailAnchorUp,
            g_vrFlailChainLength, g_vrFlailAnchorSide, g_vrFlailDamping,
            g_vrFlailModelSize,
            g_xrPlayMode == VrPlayMode::Standing ? "standing" : "seated");
        std::fclose(file);
        g_vrFlailSettingsDirty = false;
    }
    else
        std::printf("OpenXR VR: Could not write vr_settings.cfg; mode will not persist.\n");
}

static void vrLoadModeSettings()
{
    if (g_xrModeSettingsLoaded) return;
    g_xrModeSettingsLoaded = true;
    FILE* file = std::fopen("vr_settings.cfg", "r");
    if (file)
    {
        char line[128]{};
        while (std::fgets(line, sizeof(line), file))
        {
            if (std::strncmp(line, "mode=standing", 13) == 0)
                g_xrPlayMode = VrPlayMode::Standing;
            else if (std::strncmp(line, "mode=seated", 11) == 0)
                g_xrPlayMode = VrPlayMode::Seated;
        }
        std::fclose(file);
    }
    else vrSaveModeSettings();
    std::printf("OpenXR VR: PLAY MODE = %s (vr_settings.cfg). Hold BOTH stick clicks 1.5s to switch; hold with RIGHT grip to calibrate.\n",
        vrPlayModeName());
}

static void vrRequestRoomCalibration()
{
    g_xrRoomRecalibrate = true;
    g_xrRoomCalibrated = false;
    g_xrRoomHavePreviousPose = false;
    g_xrPhysicalSneaking = false;
    g_xrRoomSneakLatched = false;
    g_xrRoomStickX = g_xrRoomStickY = 0.0f;
    std::printf("OpenXR VR: ROOM HEIGHT + MOTION RECALIBRATION requested.\n");
}

static void vrTogglePlayMode()
{
    g_xrPlayMode = g_xrPlayMode == VrPlayMode::Seated ?
        VrPlayMode::Standing : VrPlayMode::Seated;
    vrSaveModeSettings();
    vrRequestRoomCalibration();
    std::printf("OpenXR VR: PLAY MODE -> %s. %s\n", vrPlayModeName(),
        g_xrPlayMode == VrPlayMode::Standing ?
        "Walk physically + use left stick." : "Left-stick movement only.");
}

// Samples position-only tracking once per frame. The generated movement axes
// feed Barony's NORMAL controller movement and thus its existing collision
// and multiplayer movement path; never teleport/mutate Entity::x/y here.
static void vrSampleHeadsetForLocomotion(XrTime predictedTime)
{
    g_xrRoomStickX = g_xrRoomStickY = 0.0f;
    g_xrPhysicalSneaking = false;
    if (!g_xrEyeViewsValid ||
        g_xrSessionState != XR_SESSION_STATE_FOCUSED)
    {
        g_xrRoomHavePreviousPose = false;
        g_xrRoomSneakLatched = false;
        return;
    }

    const XrVector3f head{
        (g_xrViews[0].pose.position.x + g_xrViews[1].pose.position.x) * 0.5f,
        (g_xrViews[0].pose.position.y + g_xrViews[1].pose.position.y) * 0.5f,
        (g_xrViews[0].pose.position.z + g_xrViews[1].pose.position.z) * 0.5f
    };
    const bool inDungeon = !intro && !gamePaused &&
        clientnum >= 0 && clientnum < MAXPLAYERS &&
        players[clientnum] && players[clientnum]->entity &&
        players[clientnum]->shootmode;

    // STEP638FO: Shaman form and Hermit projected-spirit transitions can
    // invalidate the prior room-to-avatar baseline. Rebase ONCE per enter/
    // exit, including the return to the original character. This never
    // teleports or modifies the character, camera entity or gameplay state.
    static bool hadFormState = false;
    static bool hadSpiritState = false;
    static bool trackedCharacterState = false;
    const bool formState = inDungeon && stats[clientnum] &&
        (stats[clientnum]->getEffectActive(EFF_SHAPESHIFT) ||
         players[clientnum]->entity->effectShapeshift != NOTHING);
    const bool spiritState = inDungeon && stats[clientnum] &&
        stats[clientnum]->getEffectActive(EFF_PROJECT_SPIRIT);
    if (inDungeon && trackedCharacterState &&
        (formState != hadFormState || spiritState != hadSpiritState))
    {
        vrRequestRoomCalibration();
        std::printf("OpenXR VR STEP638FO: form/spirit transition, tracking baseline reset (form=%d spirit=%d).\n",
            formState ? 1 : 0, spiritState ? 1 : 0);
    }
    // STEP638FY: Save the original body's pitch each active-dungeon frame
    // BEFORE Project Spirit is cast. The vanilla body-rest pose overwrites
    // body->pitch with PI/4, and leaving Project Spirit does not restore it.
    // Never sample the resting pose as a replacement for the saved angle.
    if (!inDungeon)
    {
        g_vrHermitPreSpellPitchValid = false;
    }
    else if (!spiritState && !hadSpiritState)
    {
        g_vrHermitPreSpellPitch = players[clientnum]->entity->pitch;
        g_vrHermitPreSpellPitchValid = true;
    }
    else if (hadSpiritState && !spiritState &&
        g_vrHermitPreSpellPitchValid &&
        !stats[clientnum]->getEffectActive(EFF_ASLEEP))
    {
        Entity* const body = players[clientnum]->entity;
        const double restingPitch = body->pitch;
        body->pitch = g_vrHermitPreSpellPitch;
        std::printf("OpenXR VR STEP638FY: Hermit Fowl Spirit exit; body pitch %.3f -> pre-spell %.3f rad.\n",
            restingPitch, body->pitch);
    }
    // STEP638FX: When the Hermit's Fowl Spirit expires, the old ghost camera
    // may remain the last camera sampled for a frame or longer. Track only
    // this spell's exit; Shaman form transitions keep their existing path.
    if (!inDungeon || spiritState)
        g_vrHermitReturnCameraPending = false;
    else if (trackedCharacterState && hadSpiritState)
    {
        g_vrHermitReturnCameraPending = true;
        std::printf("OpenXR VR STEP638FX: Hermit Project Spirit ended; re-anchoring VR camera to body.\n");
    }
    trackedCharacterState = inDungeon;
    hadFormState = formState;
    hadSpiritState = spiritState;

    if (g_xrRoomRecalibrate || !g_xrRoomCalibrated ||
        (inDungeon && !g_xrRoomWasInDungeon))
    {
        g_xrRoomCalibratedHeight = head.y;
        const XrQuaternionf& baselineQ = g_xrViews[0].pose.orientation;
        float fbx = -2.f * (baselineQ.x * baselineQ.z +
            baselineQ.y * baselineQ.w);
        float fbz = -(1.f - 2.f *
            (baselineQ.x * baselineQ.x + baselineQ.y * baselineQ.y));
        const float fbLength = std::sqrt(fbx * fbx + fbz * fbz);
        if (fbLength > 0.5f)
        {
            g_xrRoomBaselineForwardX = fbx / fbLength;
            g_xrRoomBaselineForwardZ = fbz / fbLength;
        }
        g_xrRoomBaselinePlayerYaw = (clientnum >= 0 &&
            clientnum < MAXPLAYERS && players[clientnum] &&
            players[clientnum]->entity) ?
            static_cast<float>(players[clientnum]->entity->yaw) : 0.0f;
        g_xrRoomCalibrated = true;
        g_xrRoomRecalibrate = false;
        g_xrRoomHavePreviousPose = false;
        std::printf("OpenXR VR: HEIGHT calibrated = %.3f m, mode=%s.\n",
            g_xrRoomCalibratedHeight, vrPlayModeName());
    }
    g_xrRoomWasInDungeon = inDungeon;

    if (!inDungeon || g_xrPlayMode != VrPlayMode::Standing)
    {
        g_xrRoomHavePreviousPose = false;
        g_xrRoomSneakLatched = false;
        return;
    }

    // Hysteresis avoids rapidly toggling the Sneak button near threshold.
    // Barony binds Sneak/Block/Offhand to the SAME action. Only inject it when
    // the offhand equipment slot is empty; never force shield blocking.
    const float lowered = g_xrRoomCalibratedHeight - head.y;
    if (lowered >= 0.30f)
        g_xrPhysicalSneaking = true;
    else if (lowered <= 0.24f)
        g_xrPhysicalSneaking = false;
    else
        g_xrPhysicalSneaking = g_xrRoomSneakLatched;
    g_xrRoomSneakLatched = g_xrPhysicalSneaking;
    if (!stats[clientnum] || stats[clientnum]->shield)
        g_xrPhysicalSneaking = false;
    if (g_xrPhysicalSneaking != g_xrRoomSneakLogged)
    {
        g_xrRoomSneakLogged = g_xrPhysicalSneaking;
        std::printf("OpenXR VR: PHYSICAL CROUCH -> %s\n",
            g_xrPhysicalSneaking ? "SNEAK (empty offhand)" : "RELEASE");
    }

    if (g_xrRoomHavePreviousPose)
    {
        const double dt = static_cast<double>(predictedTime - g_xrRoomPreviousTime) / 1.0e9;
        const float dx = head.x - g_xrRoomPreviousHead.x;
        const float dz = head.z - g_xrRoomPreviousHead.z;
        const float displacement = std::sqrt(dx * dx + dz * dz);
        // Bad frame timing / tracking jump: discard rather than launch player.
        if (dt >= 0.004 && dt <= 0.060 && displacement < 0.13f)
        {
            // Translate room movement into the PLAYER's current facing frame.
            // Baseline HMD room direction is captured at calibration. Turning
            // your head alone must not rotate physical locomotion sideways;
            // character yaw changes (thumbstick turning) are accounted for.
            const float fx = g_xrRoomBaselineForwardX;
            const float fz = g_xrRoomBaselineForwardZ;
            const float worldForward = (dx * fx + dz * fz) /
                static_cast<float>(dt);
            const float worldRight = (dx * (-fz) + dz * fx) /
                static_cast<float>(dt);
            const float currentYaw = static_cast<float>(
                players[clientnum]->entity->yaw);
            const float yawDelta = currentYaw - g_xrRoomBaselinePlayerYaw;
            const float cs = std::cos(yawDelta);
            const float sn = std::sin(yawDelta);
            const float forward = worldForward * cs + worldRight * sn;
            const float sideways = worldRight * cs - worldForward * sn;
            // Experimental conversion: 1 m/s physical walking -> full stick.
            // This uses Barony's movement physics, not teleportation. Precise
            // metric room-scale calibration will require an engine hook.
            const float sensitivity = 1.0f;
            auto clamp = [](float v) { return v < -1.f ? -1.f : (v > 1.f ? 1.f : v); };
            g_xrRoomStickX = clamp(sideways * sensitivity);
            g_xrRoomStickY = clamp(forward * sensitivity);
            if ((std::fabs(g_xrRoomStickX) > 0.4f ||
                std::fabs(g_xrRoomStickY) > 0.4f))
            {
                static Uint32 lastMovementLog = 0;
                const Uint32 now = SDL_GetTicks();
                if (now - lastMovementLog > 1500)
                {
                    lastMovementLog = now;
                    std::printf("OpenXR VR: ROOM MOTION x=%.2f forward=%.2f -> left-stick input.\n",
                        g_xrRoomStickX, g_xrRoomStickY);
                }
            }
        }
    }
    g_xrRoomPreviousHead = head;
    g_xrRoomPreviousTime = predictedTime;
    g_xrRoomHavePreviousPose = true;
}

// VR-only shortcuts. Button pulses last long enough to be sampled by Barony's
// slower game input polling, but never repeat while a gesture remains held.
// STEP638GA3: right-stick-down owns the vanilla Quick Turn input in the dungeon.
// Keep the simulated button pressed until the stick returns near neutral:
// Barony consumes Quick Turn as a one-shot input, so this prevents repetition.
static bool g_xrVrQuickTurnStickDownHeld = false;
static bool g_xrVrMenuChordLatched = false;
static bool g_xrVrBackpackGripWasDown = false;
static bool g_xrVrBackpackGripConsumed = false;
static bool g_xrVrBackpackInZone = false;
// STEP 611: The left-shoulder spell-opening gesture has been removed.
// Left grip is again available for the game's ordinary left-shoulder input.
static bool g_xrVrUiXWasDown = false;
static bool g_xrVrUiYWasDown = false;
static Uint32 g_xrVrMenuPulseUntilMs = 0;
static Uint32 g_xrVrBackpackPulseUntilMs = 0;

// UI laser pointer. 1=right controller (the default); later this can become a
// player option allowing the left hand, or either hand.
static constexpr int g_xrVrPointerHand = 1;
static bool g_xrVrPointerActive = false;
static bool g_xrVrPointerTriggerWasDown = false;
static bool g_xrVrPointerMouseDown = false;
static bool g_xrVrPointerRightMouseDown = false;
static bool g_xrVrPointerTriggerConsumed = false;
static bool g_xrVrPhysicalMouseOwns = false;
// STEP 620: Point-and-click is the ONLY cursor owner in menus. No simulated
// arrow-key navigation, no synthetic native gamepad buttons while UI is open.
static bool g_vrArmCalibrationActive = false;
static bool g_xrVrMenuEscapeWasDown = false;
static int g_xrVrMenuScrollHeld = 0;
static Uint32 g_xrVrMenuScrollRepeatAt = 0;
// STEP 622: A real UI is not always the same thing as "not shootmode".
// Shrines, shops, chests and auxiliary interaction windows may remain active
// while the player still owns a gameplay camera. Recognize those windows,
// rather than letting the laser drop out when a native overlay is opened.
static bool vrMenuPointerMode()
{
    if (intro || gamePaused)
        return true;
    if (clientnum < 0 || clientnum >= MAXPLAYERS || !players[clientnum])
        return false;
    Player* const p = players[clientnum];
    // STEP638FZ6: A deathcam may retain shootmode while gameover is open.
    // Treat the native gameover window as a pointer-driven modal GUI.
    return p->GUI.isGameoverActive() || !p->shootmode ||
        p->gui_mode != GUI_MODE_NONE || p->GUI.isDropdownActive() ||
        GenericGUI[clientnum].isGUIOpen() ||
        p->inventoryUI.chestGUI.bOpen || p->shopGUI.bOpen;
}

// STEP 621: An explicit bridge used by Barony's original input dispatcher.
// The native UI is mouse-driven even if a virtual SDL gamepad is attached.
// Never affect combat, calibration or another local player's controls.
bool vrMenuLaserOwnsMouse(int player)
{
    return player == clientnum && !g_xrVrPhysicalMouseOwns &&
        g_xrVrPointerActive && !g_vrArmCalibrationActive &&
        vrMenuPointerMode() &&
        g_xrSessionState == XR_SESSION_STATE_FOCUSED;
}

static void vrSendMenuScroll(int direction)
{
    extern SDL_Window* screen;
    if (!screen || !direction) return;
    SDL_Event event{};
    event.type = SDL_MOUSEWHEEL;
    event.wheel.windowID = SDL_GetWindowID(screen);
    event.wheel.y = direction;
    event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    SDL_PushEvent(&event);
}

// STEP 620: All pointer-driven menus use Barony's ordinary SDL mouse events.
// The VR right trigger is a held left mouse button, including dragging.
// B emits Escape, X/Y retain the explicit inventory tab actions below, and
// right thumbstick Y emits wheel scroll. Left thumbstick intentionally does
// NOT generate keys: otherwise it competes with pointer-based focus.
static void vrSendMenuKey(SDL_Keycode key, bool down)
{
    extern SDL_Window* screen;
    if (!screen) return;
    SDL_Event event{};
    event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.windowID = SDL_GetWindowID(screen);
    event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.key.repeat = 0;
    event.key.keysym.sym = key;
    event.key.keysym.scancode = SDL_GetScancodeFromKey(key);
    event.key.keysym.mod = KMOD_NONE;
    SDL_PushEvent(&event);
}

static void vrUpdateMenuNavigation(bool validInput = true)
{
    const bool menuPresent = validInput && vrMenuPointerMode() &&
        !g_vrArmCalibrationActive &&
        g_xrSessionState == XR_SESSION_STATE_FOCUSED &&
        g_xrControllerActionsAttached;
    const VrControllerHandState& right = g_xrControllerHands[1];
    const Uint32 now = SDL_GetTicks();

    if (!menuPresent)
    {
        if (g_xrVrMenuEscapeWasDown)
            vrSendMenuKey(SDLK_ESCAPE, false);
        g_xrVrMenuEscapeWasDown = false;
        g_xrVrMenuScrollHeld = 0;
        return;
    }

    if (right.secondaryButton != g_xrVrMenuEscapeWasDown)
    {
        vrSendMenuKey(SDLK_ESCAPE, right.secondaryButton);
        g_xrVrMenuEscapeWasDown = right.secondaryButton;
        std::printf("OpenXR VR: STEP620 MENU ESCAPE %s\n",
            right.secondaryButton ? "DOWN" : "UP");
    }

    // Left grip temporarily turns the right stick into pointer alignment
    // adjustment; do not scroll a menu during this calibration gesture.
    const int scroll = g_xrControllerHands[0].squeeze >= 0.75f ? 0 :
        std::fabs(right.thumbstick.y) >= 0.7f ?
        (right.thumbstick.y > 0.f ? 1 : -1) : 0;
    if (scroll != g_xrVrMenuScrollHeld)
    {
        g_xrVrMenuScrollHeld = scroll;
        if (scroll)
        {
            vrSendMenuScroll(scroll);
            g_xrVrMenuScrollRepeatAt = now + 300;
        }
    }
    else if (scroll &&
        static_cast<Sint32>(now - g_xrVrMenuScrollRepeatAt) >= 0)
    {
        vrSendMenuScroll(scroll);
        g_xrVrMenuScrollRepeatAt = now + 115;
    }
}
// Smooth minor tracking jitter, but snap to the real ray on a click so the
// cursor and SDL mouse event land on the same button.
static bool g_xrVrPointerSmoothValid = false;
static float g_xrVrPointerSmoothU = 0.5f;
static float g_xrVrPointerSmoothV = 0.5f;
static int g_xrVrPointerGuiX = 640;
static int g_xrVrPointerGuiY = 360;
static int g_xrVrPointerWindowX = 0;
static int g_xrVrPointerWindowY = 0;
// STEP 627: Horizontal alignment correction, measured in GUI texture pixels
// (1280 wide). Positive moves BOTH the visible cursor and the mouse target
// right. Fine-tune inside any menu: hold LEFT GRIP, push RIGHT STICK left/right.
// Releasing left grip saves the adjustment; no recompile is required.
static float g_xrVrMenuPointerOffsetX = 8.0f;
static bool g_xrVrMenuPointerOffsetLoaded = false;
static bool g_xrVrMenuPointerOffsetDirty = false;
static bool g_xrVrMenuPointerAdjustWasHeld = false;
static Uint32 g_xrVrMenuPointerAdjustNextTick = 0;
static const char* g_xrVrMenuPointerOffsetConfig =
    "vr_menu_pointer_alignment.cfg";

static void vrLoadMenuPointerOffset()
{
    if (g_xrVrMenuPointerOffsetLoaded) return;
    g_xrVrMenuPointerOffsetLoaded = true;
    if (FILE* file = std::fopen(g_xrVrMenuPointerOffsetConfig, "r"))
    {
        float saved = 0.f;
        if (std::fscanf(file, "%f", &saved) == 1 &&
            std::isfinite(saved) && saved >= -80.f && saved <= 80.f)
        {
            g_xrVrMenuPointerOffsetX = saved;
        }
        std::fclose(file);
    }
    std::printf("OpenXR VR: STEP627 MENU POINTER X OFFSET=%.1f px\n",
        g_xrVrMenuPointerOffsetX);
}

static void vrSaveMenuPointerOffset()
{
    if (!g_xrVrMenuPointerOffsetDirty) return;
    if (FILE* file = std::fopen(g_xrVrMenuPointerOffsetConfig, "w"))
    {
        if (std::fprintf(file, "%.1f\n", g_xrVrMenuPointerOffsetX) > 0)
        {
            g_xrVrMenuPointerOffsetDirty = false;
            std::printf("OpenXR VR: STEP627 SAVED MENU POINTER X OFFSET=%.1f px\n",
                g_xrVrMenuPointerOffsetX);
        }
        std::fclose(file);
    }
    else
    {
        std::printf("OpenXR VR: STEP627 WARNING cannot save %s\n",
            g_xrVrMenuPointerOffsetConfig);
    }
}
// Physical PC mouse can take ownership by moving or clicking. The VR laser
// automatically resumes after the PC mouse is idle; pressing the VR trigger
// explicitly reclaims control. Ownership cannot change during a held VR drag.
static bool g_xrVrLastWarpValid = false;
static int g_xrVrLastWarpX = 0;
static int g_xrVrLastWarpY = 0;
// STEP 621: Never switch between VR and desktop cursor just because SDL
// reports movement. SDL_WarpMouseInWindow itself generates mouse movement.
// Right-stick click in UI explicitly toggles the owner; VR trigger reclaims VR.
static bool g_xrVrPointerSwitchWasDown = false;
static bool g_xrVrPointerRayHit = false;

// STEP 623: The native inventory, buttons and Assist Shrine consult
// virtualMouse.draw_cursor to decide whether they are being operated with
// a mouse or a gamepad. SDL mouse events alone do NOT set that UI mode while
// our virtual Quest gamepad stays connected. Borrow mouse UI mode only while
// the VR laser owns a menu, then restore the previous setting on exit.
static bool g_xrVrMenuMouseModeForced = false;
static bool g_xrVrMenuMouseModePrevious = false;
static int g_xrVrMenuMouseModePlayer = -1;

static void vrSetNativeMenuMouseMode(bool useVrMouse)
{
    if (g_xrVrMenuMouseModeForced &&
        (!useVrMouse || g_xrVrMenuMouseModePlayer != clientnum))
    {
        if (g_xrVrMenuMouseModePlayer >= 0 &&
            g_xrVrMenuMouseModePlayer < MAXPLAYERS)
        {
            if (auto* mouse = inputs.getVirtualMouse(g_xrVrMenuMouseModePlayer))
                mouse->draw_cursor = g_xrVrMenuMouseModePrevious;
        }
        g_xrVrMenuMouseModeForced = false;
        g_xrVrMenuMouseModePlayer = -1;
    }
    if (!useVrMouse || clientnum < 0 || clientnum >= MAXPLAYERS ||
        !players[clientnum])
        return;

    if (auto* mouse = inputs.getVirtualMouse(clientnum))
    {
        if (!g_xrVrMenuMouseModeForced)
        {
            g_xrVrMenuMouseModePrevious = mouse->draw_cursor;
            g_xrVrMenuMouseModePlayer = clientnum;
            g_xrVrMenuMouseModeForced = true;
        }
        mouse->draw_cursor = true;
        mouse->lastMovementFromController = false;
    }
}

static void vrSendPointerMouseClick(Uint8 button, bool down)
{
    extern SDL_Window* screen;
    if (!screen) return;
    SDL_Event event{};
    event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    event.button.windowID = SDL_GetWindowID(screen);
    event.button.button = button;
    event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.button.clicks = 1;
    event.button.x = g_xrVrPointerWindowX;
    event.button.y = g_xrVrPointerWindowY;
    SDL_PushEvent(&event);
}

static void vrResetPointer()
{
    vrSaveMenuPointerOffset();
    g_xrVrMenuPointerAdjustWasHeld = false;
    vrSetNativeMenuMouseMode(false);
    if (g_xrVrPointerMouseDown)
        vrSendPointerMouseClick(SDL_BUTTON_LEFT, false);
    if (g_xrVrPointerRightMouseDown)
        vrSendPointerMouseClick(SDL_BUTTON_RIGHT, false);
    g_xrVrPointerMouseDown = false;
    g_xrVrPointerRightMouseDown = false;
    g_xrVrPointerActive = false;
    g_xrVrPointerTriggerWasDown = false;
    g_xrVrPointerTriggerConsumed = false;
    g_xrVrLastWarpValid = false;
    g_xrVrPointerRayHit = false;
    g_xrVrPointerSwitchWasDown = false;
    g_xrVrPhysicalMouseOwns = false;
}

static bool vrShortcutPulseActive(Uint32 untilMs)
{
    return untilMs != 0 &&
        static_cast<Sint32>(untilMs - SDL_GetTicks()) > 0;
}

// STEP 605: Adjustable arm-only visuals. Weapon, shield and aim transforms
// do not read these values; Step 604's hardcoded +/-90 yaw is removed below.
struct VrArmVisualCalibration
{
    float forward = 0.f, up = 0.f, side = 0.f;
    float yaw = 0.f, pitch = 0.f, roll = 0.f; // radians
};
// STEP606: each equipped ItemType has an independent calibration per hand.
// Saved offsets apply to the model only, never the controller's aim pose.
static VrArmVisualCalibration g_vrArmVisual[2]{};
// STEP638FZ8: Separate, reversible body-fit profile. Existing arm/item
// calibrations remain independent and are NEVER rewritten by body sampling.
struct VrBodyFitProfile
{
    bool enabled = false;
    float measuredReachMeters[2]{ 0.60f, 0.60f };
    float handScale[2]{ 1.f, 1.f };
    // Controller-local shift, in METERS: x=forward, y=up, z=right.
    float handOffsetMeters[2]{ 0.f, 0.f }; // local-down wrist anchor only
};
static VrBodyFitProfile g_vrBodyFit{};
static bool g_vrBodyFitLoaded = false;
static bool g_vrBodyCalibrationPage = false;
static bool g_vrBodyToggleLatched = false;
static Uint32 g_vrBodyToggleStarted = 0;
static bool g_vrBodyPoseLatched = false;
static Uint32 g_vrBodyPoseStarted = 0;
static double g_vrBodyReachTotal[2]{};
static int g_vrBodyPoseSamples = 0;
static Uint32 g_vrBodyResetStarted = 0;
static bool g_vrBodyResetLatched = false;
static const char* const VR_BODY_PROFILE_FILE = "vr_body_calibration.cfg";
static void vrLoadBodyFit()
{
    if (g_vrBodyFitLoaded) return;
    g_vrBodyFitLoaded = true;
    FILE* f = std::fopen(VR_BODY_PROFILE_FILE, "r");
    if (!f) return;
    int enabled = 0;
    float l = 0.f, r = 0.f;
    // Versioned single line, strict ranges; corrupt profiles stay disabled.
    char profileLine[160]{};
    bool parsed = false;
    while (std::fgets(profileLine, sizeof(profileLine), f))
    {
        if (std::sscanf(profileLine, "body_fit_v1 %d %f %f", &enabled, &l, &r) == 3)
        { parsed = true; break; }
    }
    if (parsed &&
        (enabled == 0 || enabled == 1) &&
        std::isfinite(l) && std::isfinite(r) &&
        l >= 0.30f && l <= 0.95f && r >= 0.30f && r <= 0.95f)
    {
        g_vrBodyFit.enabled = enabled != 0;
        g_vrBodyFit.measuredReachMeters[0] = l;
        g_vrBodyFit.measuredReachMeters[1] = r;
        for (int h = 0; h < 2; ++h)
        {
            // Keep first body-fit preview conservative: fit the original
            // first-person hand voxel, not world limbs or gameplay reach.
            const float ratio = (l + r) * .5f / 0.60f;
            g_vrBodyFit.handScale[h] = std::max(0.85f,
                std::min(1.15f, ratio));
            // Measured wrist reach changes the visual controller->hand anchor.
            // No offsets are baked into individual saved weapon profiles.
            g_vrBodyFit.handOffsetMeters[h] =
                -0.030f * g_vrBodyFit.handScale[h];
        }
    }
    std::fclose(f);
}
static bool vrSaveBodyFit()
{
    const char* temp = "vr_body_calibration.cfg.tmp";
    FILE* f = std::fopen(temp, "w");
    if (!f) return false;
    const int wrote = std::fprintf(f,
        "# Step638FZ8 visual body-fit; preserves vr_equipment_calibration.cfg\n"
        "body_fit_v1 %d %.6f %.6f\n", g_vrBodyFit.enabled ? 1 : 0,
        static_cast<double>(g_vrBodyFit.measuredReachMeters[0]),
        static_cast<double>(g_vrBodyFit.measuredReachMeters[1]));
    const int closed = std::fclose(f);
    if (wrote <= 0 || closed != 0) { std::remove(temp); return false; }
    if (!MoveFileExA(temp, VR_BODY_PROFILE_FILE,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        std::remove(temp);
        return false;
    }
    return true;
}
static void vrResetBodyPoseSamples()
{
    g_vrBodyPoseStarted = 0;
    g_vrBodyReachTotal[0] = g_vrBodyReachTotal[1] = 0.0;
    g_vrBodyPoseSamples = 0;
}
// Pose capture is always local-only and cosmetic. Quest/SteamVR owns Meta
// recentering; no attempt is made to intercept its reserved system button.
static void vrUpdateBodyPoseCalibration(XrTime predictedDisplayTime)
{
    const Uint32 now = SDL_GetTicks();
    const auto& left = g_xrControllerHands[0];
    const auto& right = g_xrControllerHands[1];
    const bool both = left.squeeze >= 0.73f && right.squeeze >= 0.73f;
    if (!both)
    {
        if (left.squeeze < 0.40f && right.squeeze < 0.40f)
            g_vrBodyPoseLatched = false;
        vrResetBodyPoseSamples();
    }
    else if (!g_vrBodyPoseLatched && left.gripPoseValid && right.gripPoseValid &&
        g_xrGuiViewSpace != XR_NULL_HANDLE &&
        g_xrReferenceSpace != XR_NULL_HANDLE)
    {
        XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
        const XrResult result = xrLocateSpace(g_xrGuiViewSpace,
            g_xrReferenceSpace, predictedDisplayTime, &loc);
        const XrSpaceLocationFlags needed =
            XR_SPACE_LOCATION_POSITION_VALID_BIT |
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_SUCCEEDED(result) && (loc.locationFlags & needed) == needed)
        {
            const XrPosef& head = loc.pose;
            const auto& q = head.orientation;
            float fx = -2.f * (q.x * q.z + q.y * q.w);
            float fz = -(1.f - 2.f * (q.x*q.x + q.y*q.y));
            const float heading = std::sqrt(fx*fx + fz*fz);
            if (heading > 0.40f)
            {
                fx /= heading; fz /= heading;
                float reach[2]{};
                bool tPose = true;
                for (int h = 0; h < 2; ++h)
                {
                    const auto& hand = g_xrControllerHands[h].gripPose.position;
                    const float dx = hand.x - head.position.x;
                    const float dz = hand.z - head.position.z;
                    const float side = dx*(-fz) + dz*fx;
                    const float forward = dx*fx + dz*fz;
                    const float vertical = hand.y - head.position.y;
                    const float signedShoulder = h == 0 ? -0.17f : 0.17f;
                    const float armSide = side - signedShoulder;
                    // Hands extended sideways at approximately shoulder height;
                    // reject crossed arms, lowered hands or forward reaches.
                    if ((h == 0 ? side > -0.43f : side < 0.43f) ||
                        std::fabs(vertical + 0.24f) > 0.18f ||
                        std::fabs(forward) > 0.33f)
                        tPose = false;
                    reach[h] = std::sqrt(armSide*armSide + forward*forward +
                        (vertical + 0.24f)*(vertical + 0.24f));
                    if (reach[h] < 0.30f || reach[h] > 0.95f) tPose = false;
                }
                if (tPose)
                {
                    if (!g_vrBodyPoseStarted) g_vrBodyPoseStarted = now;
                    for (int h = 0; h < 2; ++h)
                        g_vrBodyReachTotal[h] += reach[h];
                    ++g_vrBodyPoseSamples;
                    if (now - g_vrBodyPoseStarted >= 2000u &&
                        g_vrBodyPoseSamples >= 35)
                    {
                        VrBodyFitProfile candidate = g_vrBodyFit;
                        candidate.enabled = true;
                        for (int h = 0; h < 2; ++h)
                        {
                            candidate.measuredReachMeters[h] = static_cast<float>(
                                g_vrBodyReachTotal[h] / g_vrBodyPoseSamples);
                            candidate.handScale[h] = std::max(0.85f,
                                std::min(1.15f,
                                    candidate.measuredReachMeters[h] / 0.60f));
                            candidate.handOffsetMeters[h] =
                                -0.030f * candidate.handScale[h];
                        }
                        // Preserve measured reaches, but use a shared visual
                        // size for both hands. Per-arm manual size stays independent.
                        const float sharedScale=std::max(.85f,std::min(1.15f,
                            (candidate.measuredReachMeters[0]+candidate.measuredReachMeters[1])*.5f/.60f));
                        for(int h=0;h<2;++h) {
                            candidate.handScale[h]=sharedScale;
                            candidate.handOffsetMeters[h]=-.030f*sharedScale;
                        }
                        const VrBodyFitProfile previous = g_vrBodyFit;
                        g_vrBodyFit = candidate;
                        if (!vrSaveBodyFit())
                        {
                            g_vrBodyFit = previous; // no unsaved profile
                            messagePlayer(clientnum, MESSAGE_HINT,
                                "VR BODY FIT save failed - profile unchanged.");
                        }
                        else
                        {
                            messagePlayer(clientnum, MESSAGE_HINT,
                                "VR BODY FIT saved! Left %.2fm Right %.2fm. Weapons follow hand anchor.",
                                static_cast<double>(candidate.measuredReachMeters[0]),
                                static_cast<double>(candidate.measuredReachMeters[1]));
                            std::printf("OpenXR VR STEP638FZ8: body fit saved left=%.3f right=%.3f, samples=%d\n",
                                candidate.measuredReachMeters[0],
                                candidate.measuredReachMeters[1], g_vrBodyPoseSamples);
                        }
                        g_vrBodyPoseLatched = true;
                        vrResetBodyPoseSamples();
                    }
                    return;
                }
            }
        }
        vrResetBodyPoseSamples(); // only consecutive valid T-pose frames
    }
    else if (both) vrResetBodyPoseSamples();
    // Hold RIGHT TRIGGER to disable body fit without touching old item offsets.
    if (!both && right.trigger >= 0.90f && !g_vrBodyResetLatched)
    {
        if (!g_vrBodyResetStarted) g_vrBodyResetStarted = now;
        if (now - g_vrBodyResetStarted >= 1500u)
        {
            const VrBodyFitProfile previous = g_vrBodyFit;
            g_vrBodyFit.enabled = false;
            if (vrSaveBodyFit())
                messagePlayer(clientnum, MESSAGE_HINT,
                    "VR BODY FIT disabled. Original weapon grips preserved.");
            else
            {
                g_vrBodyFit = previous;
                messagePlayer(clientnum, MESSAGE_HINT,
                    "VR BODY FIT reset save failed. Profile unchanged.");
            }
            g_vrBodyResetLatched = true;
        }
    }
    else if (right.trigger < 0.5f)
    {
        g_vrBodyResetStarted = 0;
        g_vrBodyResetLatched = false;
    }
}

// STEP 617: First-person HUD right arm and third-person native left arm have
// different intrinsic voxel dimensions. The native left arm gets an independent
// VISUAL-ONLY scale factor; no collision, item, or projectile changes.
// 1.65x is a starting point, not a claim that the models match perfectly.
static constexpr float VR_DEFAULT_LEFT_ARM_SCALE_FACTOR = 1.65f;
static float g_vrLeftArmScaleFactor = VR_DEFAULT_LEFT_ARM_SCALE_FACTOR;
static float g_vrRightArmScaleFactor = 1.f;
// STEP619: Barony has a proper first-person LEFT hand! actLeftHandMagic()
// assigns it the right race/appearance/glove sprite (human 656/657/658,
// equipment 659+). Use players[clientnum]->hud.magicLeftHand->sprite as the
// new default, even when the magic-hand HUD entity is invisible between casts.
// 0=native third-person left; 1=right HUD mirrored Y; 2=right HUD mirrored X;
// 3=native FIRST-PERSON left from actLeftHandMagic().
static int g_vrLeftArmMeshMode = 3;
static bool g_vrArmCalibrationRightClickPrevious = false;
static const char* vrLeftArmMeshName()
{
    switch (g_vrLeftArmMeshMode)
    {
        case 0: return "NATIVE WORLD LEFT";
        case 1: return "HUD MIRROR Y";
        case 2: return "HUD MIRROR X";
        case 3: return "NATIVE FIRST-PERSON LEFT";
        default: return "UNKNOWN";
    }
}
static std::unordered_map<int, VrArmVisualCalibration> g_vrItemVisual[2];
// STEP608: Independent aim angle per ranged ItemType. These values change
// BOTH the visual five-dot reticle and the launch ray, never the held mesh.
struct VrRangedAimCalibration { float yaw = 0.f, pitch = 0.f; };
static std::unordered_map<int, VrRangedAimCalibration> g_vrRangedAimVisual;
// STEP638DP: Spell missiles use LEFT controller and independent persistent
// yaw/pitch correction. Do not couple these values to any weapon/item profile.
// STEP 638DY: The left spell guide needs more horizontal travel than the
// original +/-20-degree weapon tuning; vertical remains on its existing range.
// These limits are shared by edit, load, GUI projection and projectile rays.
static constexpr float VR_SPELL_AIM_MAX_YAW = 0.85f;   // approx +/-49 degrees
static constexpr float VR_SPELL_AIM_MAX_PITCH = 0.35f; // existing +/-20 degrees
static VrRangedAimCalibration g_vrSpellAimVisual{};
// STEP638EI: One independent transform for each wearable original-HUD panel.
// Units match the existing arm/equipment visual calibration (Barony units,
// radians). This never changes the underlying vanilla UI or stat values.
struct VrWearableHudTransform
{
    float forward, up, side, yaw, pitch, roll, scale;
};
static VrWearableHudTransform g_vrWearableHud[2] = {
    { -1.15f, .28f, -.18f, 0.f, 0.f, 0.f, 1.f },
    { -.64f, .26f, .02f, 0.f, 0.f, 0.f, 1.f }
};
static const VrWearableHudTransform g_vrWearableHudDefaults[2] = {
    { -1.15f, .28f, -.18f, 0.f, 0.f, 0.f, 1.f },
    { -.64f, .26f, .02f, 0.f, 0.f, 0.f, 1.f }
};
// STEP638FD: Independent LOCAL adjustment to the status-effect strip, relative
// to the existing HP/MP plane. No change to the HP/MP or wrist calibration.
// Zero position/rotation and 1x scale reproduces the Step638FB placement.
static VrWearableHudTransform g_vrStatusEffectsHud{ 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f };
static const VrWearableHudTransform g_vrStatusEffectsHudDefault{ 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f };
// STEP638FZ4: Restore all three previously calibrated VR HUD surfaces:
// waist hotbar, forward interaction prompts, and dungeon messages.
// Stored OFFSETS relative to safe head/body-facing initial placements.
// The original UI source remains untouched so desktop/menu interactions work.
// STEP638FZ2: Page 3 is the independently calibrated XP bar attached to
// the waist belt. Existing pages 0..2 and their persisted values are intact.
// GA7: Index 4 is an independent, persistent native party-bar transform.
// Existing saved indices 0..3 remain backward-compatible; 1 and 2 are
// retained for old config files but hidden from the calibrator sequence.
// GA10: Append skill-up calibration as index 5; saved indices 0..4 are unchanged.
static constexpr int VR_SPATIAL_HUD_COUNT = 6;
static VrWearableHudTransform g_vrSpatialHud[VR_SPATIAL_HUD_COUNT] = {
    { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f },
    { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f },
    { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f },
    { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f },
    { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f },
    { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f }
};
static int g_vrArmCalibrationSpatialPage = -1; // -1 means no spatial page.
// STEP638FP: Dedicated cosmetic flail page; never edits combat registration.
static bool g_vrArmCalibrationEditFlail = false;
// GA13: calibration-only shortbow geometry/draw pilot. No attack calls.
static bool g_vrArmCalibrationEditBow = false;
static int g_vrBowDrawHand = 1; // 1: right-handed archer; bow held on left.
static float g_vrBowFullDraw = .45f;
struct VrBowGripCalibration {
    float forward=0.f, up=0.f, side=0.f, yaw=0.f, pitch=0.f, roll=0.f;
};
static VrBowGripCalibration g_vrBowGrip[2];
static float g_vrBowZRotation[2]{};
// GA19 correction axes are controller-local forward/up/right, applied AFTER
// the legacy model alignment and BEFORE grip translation. Identity preserves
// every existing saved pose; edits never decompose back to Euler angles.
static XrQuaternionf g_vrBowControllerRotation[2]{{0,0,0,1},{0,0,0,1}};
static void vrBowAdjustControllerRotation(int hand, int axis, float radians)
{
    const float sn=sinf(radians*.5f), cs=cosf(radians*.5f);
    const float x=axis==0?sn:0.f, y=axis==1?sn:0.f, z=axis==2?sn:0.f;
    const auto q=g_vrBowControllerRotation[hand];
    XrQuaternionf next{
        cs*q.x+x*q.w+y*q.z-z*q.y,
        cs*q.y-x*q.z+y*q.w+z*q.x,
        cs*q.z+x*q.y-y*q.x+z*q.w,
        cs*q.w-x*q.x-y*q.y-z*q.z};
    const float length=sqrtf(next.x*next.x+next.y*next.y+next.z*next.z+next.w*next.w);
    if(std::isfinite(length) && length>.0001f) {
        next.x/=length;next.y/=length;next.z/=length;next.w/=length;
        g_vrBowControllerRotation[hand]=next;
    }
}
static bool g_vrBowEditGrip = false;

struct VrBowPreviewState {
    bool drawing = false, armed = false;
    float draw = 0.f;
    unsigned releases = 0;
    int hand = -1;
    Uint32 lastSample = 0;
    bool gameplay = false;
    Uint32 bowUid = 0, playerUid = 0;
};
static VrBowPreviewState g_vrBowPreview;
struct VrBowReleaseState {
    bool pending = false, nativeScope = false, projectileSeen = false;
    Uint32 atMs = 0, nativeTick = 0, bowUid = 0, playerUid = 0;
    float draw = 0.f;
    XrVector3f origin{}, forward{};
};
static VrBowReleaseState g_vrBowRelease;
// GA15: one equipment/hand policy shared by gameplay and calibration.
// This pilot still targets SHORTBOW only; other weapons retain their paths.
static bool vrBowEquipped()
{
    return clientnum >= 0 && clientnum < MAXPLAYERS && stats[clientnum] &&
        stats[clientnum]->weapon && stats[clientnum]->weapon->type == SHORTBOW;
}
static bool vrBowPreviewSelected()
{
    return vrBowEquipped() && g_vrArmCalibrationActive && g_vrArmCalibrationEditBow;
}
static bool vrBowDisplaySelected()
{
    return vrBowEquipped(); // GA20: display ownership persists on every calibration page.
}
static bool vrBowGameplayActive()
{
    return vrBowEquipped() && !g_vrArmCalibrationActive && !intro && !gamePaused &&
        players[clientnum] && players[clientnum]->entity && players[clientnum]->shootmode;
}
// GA17: physical bow/spell input yields to the native face hotbar.
static bool vrBowHotbarSelecting()
{
    return vrBowEquipped() && players[clientnum] && players[clientnum]->shootmode &&
        players[clientnum]->hotbar.useHotbarFaceMenu &&
        (g_xrControllerHands[0].primaryButton || g_xrControllerHands[0].secondaryButton ||
         g_xrControllerHands[1].secondaryButton ||
         players[clientnum]->hotbar.faceMenuButtonHeld != Player::Hotbar_t::GROUP_NONE);
}
static int vrSpellControllerHand()
{
    return vrBowEquipped() ? g_vrBowDrawHand : 0;
}
// Ownership survives pause/focus loss so the native bow cannot complete a
// previously held draw on its vanilla automatic-fire path.
bool vrPhysicalBowOwnsAttack(int playerIndex)
{
    return playerIndex == clientnum && vrBowEquipped() &&
        multiplayer != CLIENT && g_xrSession != XR_NULL_HANDLE;
}
static void vrUpdateBowPreview();

static const char* vrSpatialHudName(int index)
{
    switch (index) {
    case 0: return "BELT HOTBAR";
    case 1: return "FORWARD ACTION PROMPTS";
    case 2: return "DUNGEON MESSAGES";
    case 3: return "BELT XP BAR";
    case 4: return "NATIVE COMPANION / CO-OP BARS";
    case 5: return "SKILL-UP NOTIFICATIONS";
    default: return "NONE";
    }
}
static bool g_vrArmCalibrationEditStatusEffects = false;
static bool g_vrArmCalibrationEditWearable = false;
static bool g_vrArmCalibrationEditSpellAim = false;
static bool g_vrArmCalibrationEditAim = false;
static bool g_vrArmCalibrationEditItem = false;
// STEP632: Separate melee gesture page. Enter from existing calibration by
// holding BOTH physical controller GRIPS for 3s. Hold again (after release)
// in melee calibration to save and EXIT DIRECTLY TO GAME.
static bool g_vrArmCalibrationEditMelee = false;
static Uint32 g_vrMeleeGripChordStartedAt = 0;
static bool g_vrMeleeGripChordLatched = false;
static bool g_vrMeleeExitGripsSuppressed = false;
struct VrMeleeTuning
{
    // Require the HAND and the BLADE to move together before striking.
    // This removes Step630's permissive tip-speed-only OR path.
    float minTipSpeed = 1.70f;   // meters/second, weapon tip relative to HMD
    float minGripSpeed = 0.80f;  // meters/second, hand relative to HMD
    float rearmSpeed = 0.26f;    // meters/second, slowdown threshold floor
};
static VrMeleeTuning g_vrMeleeTuning{};
static bool g_vrMeleeTuningLoaded = false;
static bool g_vrMeleeTuningDirty = false;
static Uint32 g_vrMeleeLastSave = 0;
static float g_vrMeleeLiveTipSpeed = 0.f;
static float g_vrMeleeLiveGripSpeed = 0.f;
// STEP633: Expose recovery state in the dry-run calibration overlay.
static bool g_vrMeleeLiveReady = true;
static float g_vrMeleeLiveReverseMeters = 0.f;
static Uint32 g_vrMeleeLastPreviewAt = 0;
static void vrLoadMeleeTuning()
{
    if (g_vrMeleeTuningLoaded) return;
    g_vrMeleeTuningLoaded = true;
    FILE* file = std::fopen("vr_melee_calibration.cfg", "r");
    if (!file) return; // Safe defaults on first launch.
    char line[256]{};
    while (std::fgets(line, sizeof(line), file))
    {
        VrMeleeTuning v{};
        float unusedLegacyPadding = 0.f;
        // Accept both Step631's four-value settings and Step632's
        // three-value gesture-only format. Ignore old collision padding.
        const int fields = std::sscanf(line, "melee %f %f %f %f",
            &v.minTipSpeed, &v.minGripSpeed, &v.rearmSpeed,
            &unusedLegacyPadding);
        if (fields >= 3 &&
            std::isfinite(v.minTipSpeed) && std::isfinite(v.minGripSpeed) &&
            std::isfinite(v.rearmSpeed) &&
            v.minTipSpeed >= 0.8f && v.minTipSpeed <= 4.5f &&
            v.minGripSpeed >= 0.2f && v.minGripSpeed <= 2.5f &&
            v.rearmSpeed >= 0.08f && v.rearmSpeed <= 0.7f)
            g_vrMeleeTuning = v;
    }
    std::fclose(file);
    std::printf("OpenXR VR STEP631: melee tuning loaded.\n");
}
static void vrSaveMeleeTuning()
{
    FILE* file = std::fopen("vr_melee_calibration.cfg", "w");
    if (!file)
    {
        std::printf("OpenXR VR STEP631: melee calibration SAVE FAILED\n");
        if (clientnum >= 0 && clientnum < MAXPLAYERS && players[clientnum])
            messagePlayer(clientnum, MESSAGE_HINT,
                "VR melee calibration save FAILED (folder permissions)");
        return;
    }
    std::fprintf(file, "# Barony VR Step632: melee min_tip_mps min_grip_mps rearm_mps\n");
    std::fprintf(file, "melee %.4f %.4f %.4f\n",
        static_cast<double>(g_vrMeleeTuning.minTipSpeed),
        static_cast<double>(g_vrMeleeTuning.minGripSpeed),
        static_cast<double>(g_vrMeleeTuning.rearmSpeed));
    if (std::fclose(file) == 0)
    {
        g_vrMeleeTuningDirty = false;
        g_vrMeleeLastSave = SDL_GetTicks();
        std::printf("OpenXR VR STEP631: melee tuning saved.\n");
    }
}
static bool g_vrArmCalibrationLeftClickPrevious = false;
static bool g_vrArmCalibrationLoaded = false;

static bool g_vrArmCalibrationDirty = false;
static int g_vrArmCalibrationSelected = 1;
static bool g_vrArmCalibrationChordLatched = false;
static Uint32 g_vrArmCalibrationChordStart = 0;
static Uint32 g_vrArmCalibrationLastSample = 0;
static Uint32 g_vrArmCalibrationLastMessage = 0;
static Uint32 g_vrArmCalibrationLastSave = 0;
static Uint32 g_vrArmCalibrationResetStart = 0;
static bool g_vrArmCalibrationResetLatched = false;
static bool g_vrArmCalibrationAPrevious = false;
static bool g_vrArmCalibrationBPrevious = false;

static void vrLoadVisualArmCalibration()
{
    if (g_vrArmCalibrationLoaded) return;
    g_vrArmCalibrationLoaded = true;
    FILE* file = std::fopen("vr_equipment_calibration.cfg", "r");
    if (!file) { std::printf("OpenXR VR: STEP605 arm offsets initialized to zero.\n"); return; }
    char line[256]{};
    while (std::fgets(line, sizeof(line), file))
    {
        int rotationHand=-1;XrQuaternionf correction{};
        if(std::sscanf(line,"bow_controller_rotation%d %f %f %f %f",&rotationHand,
            &correction.x,&correction.y,&correction.z,&correction.w)==5) {
            const float length=sqrtf(correction.x*correction.x+correction.y*correction.y+
                correction.z*correction.z+correction.w*correction.w);
            if(rotationHand>=0 && rotationHand<2 && std::isfinite(length) && length>=.5f && length<=1.5f) {
                correction.x/=length;correction.y/=length;correction.z/=length;correction.w/=length;
                g_vrBowControllerRotation[rotationHand]=correction;
            }
            continue;
        }
        int zHand=-1; float zAngle=0.f;
        if(std::sscanf(line,"bow_zrotation%d %f",&zHand,&zAngle)==2) {
            if(zHand>=0 && zHand<2 && std::isfinite(zAngle) && std::fabs(zAngle)<=3.142f)
                g_vrBowZRotation[zHand]=zAngle;
            continue;
        }
        float rightScale=0.f;
        if(std::sscanf(line,"arm_scale1 %f",&rightScale)==1) {
            if(std::isfinite(rightScale) && rightScale>=.70f && rightScale<=3.50f)
                g_vrRightArmScaleFactor=rightScale;
            continue;
        }
        int gripHand=-1; VrBowGripCalibration grip{};
        if(std::sscanf(line,"bow_grip%d %f %f %f %f %f %f",&gripHand,
            &grip.forward,&grip.up,&grip.side,&grip.yaw,&grip.pitch,&grip.roll)==7) {
            bool valid=gripHand>=0 && gripHand<2;
            for(float value:{grip.forward,grip.up,grip.side})
                if(!std::isfinite(value)||std::fabs(value)>.30f)valid=false;
            for(float value:{grip.yaw,grip.pitch,grip.roll})
                if(!std::isfinite(value)||std::fabs(value)>3.142f)valid=false;
            if(valid)g_vrBowGrip[gripHand]=grip;
            continue;
        }
        int bowHand=-1; float bowDraw=0.f;
        if (std::sscanf(line,"bow_draw_hand %d",&bowHand)==1) {
            if(bowHand==0 || bowHand==1)g_vrBowDrawHand=bowHand;
            continue;
        }
        if (std::sscanf(line,"bow_full_draw %f",&bowDraw)==1) {
            if(std::isfinite(bowDraw) && bowDraw>=.20f && bowDraw<=.65f)g_vrBowFullDraw=bowDraw;
            continue;
        }
        // STEP618: save/load model choice independently of visual offsets.
        int meshMode = -1;
        if (std::sscanf(line, "arm_mesh0 %d", &meshMode) == 1)
        {
            if (meshMode >= 0 && meshMode <= 3)
                g_vrLeftArmMeshMode = meshMode;
            continue;
        }
        // STEP 617: Optional, backwards-compatible visual size setting.
        // Old calibration files without this line retain the 1.65x default.
        float leftScale = 0.f;
        if (std::sscanf(line, "arm_scale0 %f", &leftScale) == 1)
        {
            if (std::isfinite(leftScale) && leftScale >= .70f && leftScale <= 3.50f)
                g_vrLeftArmScaleFactor = leftScale;
            continue;
        }
        // STEP638EI: Older settings files simply keep the above defaults.
        int wearableHand = -1;
        VrWearableHudTransform wearable{};
        if (std::sscanf(line, "wearable%d %f %f %f %f %f %f %f",
            &wearableHand, &wearable.forward, &wearable.up, &wearable.side,
            &wearable.yaw, &wearable.pitch, &wearable.roll, &wearable.scale) == 8)
        {
            const float values[] = { wearable.forward, wearable.up, wearable.side,
                wearable.yaw, wearable.pitch, wearable.roll, wearable.scale };
            bool valid = wearableHand >= 0 && wearableHand < 2 &&
                wearable.scale >= .35f && wearable.scale <= 3.0f;
            for (float v : values)
                if (!std::isfinite(v) || std::fabs(v) > 6.f) valid = false;
            if (valid) g_vrWearableHud[wearableHand] = wearable;
            continue;
        }
        // STEP638FD: Independent status-effect position relative to HP/MP.
        // This optional entry is ignored by old game builds and defaults to
        // the already-tested above-HP/MP placement when absent.
        VrWearableHudTransform status{};
        if (std::sscanf(line, "status_effect_hud %f %f %f %f %f %f %f",
            &status.forward, &status.up, &status.side,
            &status.yaw, &status.pitch, &status.roll, &status.scale) == 7)
        {
            const float values[] = { status.forward, status.up, status.side,
                status.yaw, status.pitch, status.roll, status.scale };
            bool valid = status.scale >= .35f && status.scale <= 3.f;
            for (float value : values)
                if (!std::isfinite(value) || std::fabs(value) > 6.f) valid = false;
            if (valid) g_vrStatusEffectsHud = status;
            continue;
        }
        // STEP638FE: Optional new per-panel offsets; older files remain valid.
        int spatialIndex = -1;
        VrWearableHudTransform spatial{};
        if (std::sscanf(line, "spatial_hud%d %f %f %f %f %f %f %f",
            &spatialIndex, &spatial.forward, &spatial.up, &spatial.side,
            &spatial.yaw, &spatial.pitch, &spatial.roll, &spatial.scale) == 8)
        {
            const float values[] = { spatial.forward, spatial.up, spatial.side,
                spatial.yaw, spatial.pitch, spatial.roll, spatial.scale };
            bool valid = spatialIndex >= 0 && spatialIndex < VR_SPATIAL_HUD_COUNT &&
                spatial.scale >= .35f && spatial.scale <= 3.f;
            for (float value : values)
                if (!std::isfinite(value) || std::fabs(value) > 6.f) valid = false;
            if (valid) g_vrSpatialHud[spatialIndex] = spatial;
            continue;
        }
        // Independent spell aim; older calibration files simply use zeros.
        VrRangedAimCalibration spellAim{};
        if (std::sscanf(line, "spell_aim %f %f",
            &spellAim.yaw, &spellAim.pitch) == 2)
        {
            if (std::isfinite(spellAim.yaw) && std::isfinite(spellAim.pitch) &&
                std::fabs(spellAim.yaw) <= VR_SPELL_AIM_MAX_YAW + .001f &&
                std::fabs(spellAim.pitch) <= VR_SPELL_AIM_MAX_PITCH + .001f)
                g_vrSpellAimVisual = spellAim;
            continue;
        }
        // Additive file format: Step606 arm/item lines are still accepted.
        int aimType = -1;
        VrRangedAimCalibration aim{};
        if (std::sscanf(line, "aim %d %f %f", &aimType,
            &aim.yaw, &aim.pitch) == 3)
        {
            if (aimType >= 0 && aimType < NUMITEMS &&
                std::isfinite(aim.yaw) && std::isfinite(aim.pitch) &&
                std::fabs(aim.yaw) <= 0.45f && std::fabs(aim.pitch) <= 0.45f)
                g_vrRangedAimVisual[aimType] = aim;
            continue;
        }
        int hand = -1;
        VrArmVisualCalibration a{};
        int itemType = -1;
        const bool armEntry = std::sscanf(line, "arm%d %f %f %f %f %f %f", &hand,
            &a.forward, &a.up, &a.side, &a.yaw, &a.pitch, &a.roll) == 7;
        if (!armEntry)
        {
            if (std::sscanf(line, "item%d %d %f %f %f %f %f %f", &hand, &itemType,
                &a.forward, &a.up, &a.side, &a.yaw, &a.pitch, &a.roll) != 8)
                continue;
        }
        if (hand < 0 || hand > 1) continue;
        const float values[] = { a.forward, a.up, a.side, a.yaw, a.pitch, a.roll };
        bool valid = true;
        for (float v : values) if (!std::isfinite(v) || std::fabs(v) > 12.f) valid = false;
        if (!valid) continue;
        if (armEntry) g_vrArmVisual[hand] = a;
        else if (itemType >= 0 && itemType < NUMITEMS)
            g_vrItemVisual[hand][itemType] = a;
    }
    std::fclose(file);
    std::printf("OpenXR VR: STEP606 arm and equipment offsets loaded.\n");
}
static void vrSaveVisualArmCalibration()
{
    FILE* file = std::fopen("vr_equipment_calibration.cfg", "w");
    if (!file)
    {
        std::printf("OpenXR VR: STEP605 ERROR cannot save vr_equipment_calibration.cfg\n");
        if (clientnum >= 0 && clientnum < MAXPLAYERS && players[clientnum])
            messagePlayer(clientnum, MESSAGE_HINT, "VR arm save FAILED (folder permission)");
        return;
    }
    std::fprintf(file, "# Barony VR Step618: arm{hand} or item{hand} {ItemType}, forward up side yaw pitch roll (radians)\n");
    for(int hand=0;hand<2;++hand) {
        const auto& g=g_vrBowGrip[hand];
        std::fprintf(file,"bow_grip%d %.6f %.6f %.6f %.7f %.7f %.7f\n",hand,
            g.forward,g.up,g.side,g.yaw,g.pitch,g.roll);
    }
    std::fprintf(file,"bow_draw_hand %d\n",g_vrBowDrawHand);
    std::fprintf(file,"bow_full_draw %.4f\n",g_vrBowFullDraw);
    // Model trial and size are visual only; old calibration files still load.
    std::fprintf(file, "arm_mesh0 %d\n", g_vrLeftArmMeshMode);
    std::fprintf(file, "arm_scale0 %.5f\n", g_vrLeftArmScaleFactor);
    std::fprintf(file, "arm_scale1 %.5f\n", g_vrRightArmScaleFactor);
    for(int h=0;h<2;++h) {
        std::fprintf(file,"bow_zrotation%d %.7f\n",h,g_vrBowZRotation[h]);
        const auto& q=g_vrBowControllerRotation[h];
        std::fprintf(file,"bow_controller_rotation%d %.9f %.9f %.9f %.9f\n",h,q.x,q.y,q.z,q.w);
    }
    for (int hand = 0; hand < 2; ++hand)
    {
        const auto& a = g_vrArmVisual[hand];
        std::fprintf(file, "arm%d %.6f %.6f %.6f %.7f %.7f %.7f\n", hand,
            a.forward, a.up, a.side, a.yaw, a.pitch, a.roll);
        for (const auto& entry : g_vrItemVisual[hand])
        {
            const auto& v = entry.second;
            std::fprintf(file, "item%d %d %.6f %.6f %.6f %.7f %.7f %.7f\n", hand,
                entry.first, v.forward, v.up, v.side, v.yaw, v.pitch, v.roll);
        }
    }
    for (const auto& entry : g_vrRangedAimVisual)
        std::fprintf(file, "aim %d %.7f %.7f\n",
            entry.first, entry.second.yaw, entry.second.pitch);
    std::fprintf(file, "spell_aim %.7f %.7f\n",
        g_vrSpellAimVisual.yaw, g_vrSpellAimVisual.pitch);
    for (int hand = 0; hand < 2; ++hand)
    {
        const auto& v = g_vrWearableHud[hand];
        std::fprintf(file, "wearable%d %.6f %.6f %.6f %.7f %.7f %.7f %.6f\n",
            hand, v.forward, v.up, v.side, v.yaw, v.pitch, v.roll, v.scale);
    }
    // Status strip offsets are local to the HP/MP panel, not replacements
    // for wearable0. Keeping both entries guarantees backward compatibility.
    std::fprintf(file, "status_effect_hud %.6f %.6f %.6f %.7f %.7f %.7f %.6f\n",
        g_vrStatusEffectsHud.forward, g_vrStatusEffectsHud.up,
        g_vrStatusEffectsHud.side, g_vrStatusEffectsHud.yaw,
        g_vrStatusEffectsHud.pitch, g_vrStatusEffectsHud.roll,
        g_vrStatusEffectsHud.scale);
    // GA10: Append the skill transform to the same calibration file.
    // All existing panel entries retain their original indices and values.
    for (int i = 0; i < VR_SPATIAL_HUD_COUNT; ++i)
    {
        const auto& h = g_vrSpatialHud[i];
        std::fprintf(file, "spatial_hud%d %.6f %.6f %.6f %.7f %.7f %.7f %.6f\n",
            i, h.forward, h.up, h.side, h.yaw, h.pitch, h.roll, h.scale);
    }
    if (std::fclose(file) == 0)
    {
        g_vrArmCalibrationDirty = false;
        g_vrArmCalibrationLastSave = SDL_GetTicks();
        std::printf("OpenXR VR: STEP606 CAL_SAVED vr_equipment_calibration.cfg\n");
    }
}
// Current equipment comes from the real inventory, not whichever HUD entity
// happens to exist this frame. An empty hand cannot be calibrated as an item.
static const Item* vrCalibrationEquippedItem(int hand)
{
    if (hand < 0 || hand > 1 || clientnum < 0 || clientnum >= MAXPLAYERS ||
        !stats[clientnum]) return nullptr;
    const Item* item = hand == 1 ? stats[clientnum]->weapon : stats[clientnum]->shield;
    // Vanilla first-person SPELLBOOKs do not have a rendered weapon voxel.
    if (hand == 1 && item && itemCategory(item) == SPELLBOOK) return nullptr;
    return item;
}
// STEP612: Barony's thrown category is not isRangedWeapon(): it includes
// chakrams, shurikens, tomahawks, and more. Other thrown paths include
// gems/rocks, potions/bottles, pies, tinkering throwables and rubber ducks.
// Keep ONE item filter shared by controller aim, reticle, and calibration.
static bool vrIsThrowableItem(const Item& item)
{
    const Category category = itemCategory(&item);
    return category == THROWN || category == GEM || category == POTION ||
        item.type == FOOD_CREAMPIE || item.type == TOOL_DUCK ||
        itemIsThrowableTinkerTool(&item);
}
static bool vrIsAimedProjectileItem(const Item& item)
{
    return isRangedWeapon(item) || vrIsThrowableItem(item);
}
// Melee page is available only for an equipped right-hand physical weapon.
static const Item* vrCalibrationMeleeItem()
{
    if (g_vrArmCalibrationSelected != 1) return nullptr;
    const Item* equipped = vrCalibrationEquippedItem(1);
    if (!equipped) return nullptr;
    return (isMeleeWeapon(*equipped) || equipped->type == TOOL_PICKAXE ||
        equipped->type == TOOL_WHIP) ? equipped : nullptr;
}
static const Item* vrCalibrationRangedItem()
{
    if (g_vrArmCalibrationSelected != 1) return nullptr;
    const Item* equipped = vrCalibrationEquippedItem(1);
    return (equipped && vrIsAimedProjectileItem(*equipped)) ? equipped : nullptr;
}
static VrRangedAimCalibration vrCalibrationAimValues()
{
    const Item* equipped = vrCalibrationRangedItem();
    if (!equipped) return {};
    const auto it = g_vrRangedAimVisual.find(static_cast<int>(equipped->type));
    return it != g_vrRangedAimVisual.end() ? it->second : VrRangedAimCalibration{};
}
static const VrArmVisualCalibration& vrCalibrationSelectedValues()
{
    const Item* item = g_vrArmCalibrationEditItem ?
        vrCalibrationEquippedItem(g_vrArmCalibrationSelected) : nullptr;
    if (item)
    {
        const auto& offsets = g_vrItemVisual[g_vrArmCalibrationSelected];
        const auto it = offsets.find(static_cast<int>(item->type));
        if (it != offsets.end()) return it->second;
        static const VrArmVisualCalibration defaults{};
        return defaults;
    }
    return g_vrArmVisual[g_vrArmCalibrationSelected];
}
static VrArmVisualCalibration* vrCalibrationWritableSelection()
{
    if (g_vrArmCalibrationEditMelee || g_vrArmCalibrationEditSpellAim ||
        g_vrArmCalibrationEditWearable || g_vrArmCalibrationEditStatusEffects ||
        g_vrArmCalibrationSpatialPage >= 0 || g_vrArmCalibrationEditFlail)
        return nullptr;
    if (!g_vrArmCalibrationEditItem) return &g_vrArmVisual[g_vrArmCalibrationSelected];
    const Item* item = vrCalibrationEquippedItem(g_vrArmCalibrationSelected);
    if (!item) return nullptr;
    return &g_vrItemVisual[g_vrArmCalibrationSelected][static_cast<int>(item->type)];
}
static void vrShowVisualArmStatus(bool controls)
{
    if (clientnum < 0 || clientnum >= MAXPLAYERS || !players[clientnum]) return;
    const auto& a = vrCalibrationSelectedValues();
    if (g_vrArmCalibrationEditMelee && !controls)
    {
        messagePlayer(clientnum, MESSAGE_HINT,
            "VR MELEE DRY RUN: TIP %.2f  HAND %.2f  REARM %.2f m/s",
            static_cast<double>(g_vrMeleeTuning.minTipSpeed),
            static_cast<double>(g_vrMeleeTuning.minGripSpeed),
            static_cast<double>(g_vrMeleeTuning.rearmSpeed));
        return;
    }
    if (g_vrArmCalibrationEditFlail && !controls)
    {
        messagePlayer(clientnum, MESSAGE_HINT,
            "VR FLAIL: fwd %+.3f up %+.3f side %+.3f len %.3f model %.0f%%",
            static_cast<double>(g_vrFlailAnchorForward),
            static_cast<double>(g_vrFlailAnchorUp),
            static_cast<double>(g_vrFlailAnchorSide),
            static_cast<double>(g_vrFlailChainLength),
            static_cast<double>(g_vrFlailModelSize * 100.f));
        return;
    }
    if (g_vrArmCalibrationSpatialPage >= 0 && !controls)
    {
        const auto& h = g_vrSpatialHud[g_vrArmCalibrationSpatialPage];
        messagePlayer(clientnum, MESSAGE_HINT,
            "VR %s: side %+.2f up %+.2f depth %+.2f size %.2f",
            vrSpatialHudName(g_vrArmCalibrationSpatialPage),
            static_cast<double>(h.side), static_cast<double>(h.up),
            static_cast<double>(h.forward), static_cast<double>(h.scale));
        return;
    }
    if (g_vrArmCalibrationEditStatusEffects && !controls)
    {
        const auto& w = g_vrStatusEffectsHud;
        messagePlayer(clientnum, MESSAGE_HINT,
            "VR STATUS ICONS (relative to HP/MP): side %+.2f up %+.2f depth %+.2f size %.2f",
            static_cast<double>(w.side), static_cast<double>(w.up),
            static_cast<double>(w.forward), static_cast<double>(w.scale));
        return;
    }
    if (g_vrArmCalibrationEditWearable && !controls)
    {
        const auto& w = g_vrWearableHud[g_vrArmCalibrationSelected];
        messagePlayer(clientnum, MESSAGE_HINT,
            "VR %s WEARABLE: side %+.2f up %+.2f depth %+.2f size %.2f",
            g_vrArmCalibrationSelected ? "MAP WATCH" : "HP/MP FOREARM",
            static_cast<double>(w.side), static_cast<double>(w.up),
            static_cast<double>(w.forward), static_cast<double>(w.scale));
        return;
    }
    if (g_vrArmCalibrationEditSpellAim && !controls)
    {
        messagePlayer(clientnum, MESSAGE_HINT,
            "VR LEFT SPELL AIM: yaw %+.1f pitch %+.1f deg. LEFT stick to adjust.",
            static_cast<double>(g_vrSpellAimVisual.yaw * 180.f / static_cast<float>(PI)),
            static_cast<double>(g_vrSpellAimVisual.pitch * 180.f / static_cast<float>(PI)));
        return;
    }
    if (g_vrArmCalibrationEditAim && !controls)
    {
        const auto aim = vrCalibrationAimValues();
        messagePlayer(clientnum, MESSAGE_HINT,
            "VR RIGHT AIM: yaw %+.1f pitch %+.1f deg. LEFT stick to adjust.",
            static_cast<double>(aim.yaw * 180.f / static_cast<float>(PI)),
            static_cast<double>(aim.pitch * 180.f / static_cast<float>(PI)));
        return;
    }
    if (controls)
        messagePlayer(clientnum, MESSAGE_HINT,
            "VR CAL: A=right B=left; CLICK LEFT STICK=arm/item/aim; X+Y=exit");
    else
        messagePlayer(clientnum, MESSAGE_HINT,
            "VR %s %s: side %.2f up %.2f depth %.2f; yaw %.0f pitch %.0f roll %.0f",
            g_vrArmCalibrationSelected ? "RIGHT" : "LEFT",
            g_vrArmCalibrationEditItem ? "EQUIPMENT" : "ARM",
            static_cast<double>(a.side), static_cast<double>(a.up),
            static_cast<double>(a.forward),
            static_cast<double>(a.yaw * 180.f / static_cast<float>(PI)),
            static_cast<double>(a.pitch * 180.f / static_cast<float>(PI)),
            static_cast<double>(a.roll * 180.f / static_cast<float>(PI)));
}
// Run after controller pose polling and before forwarding virtual gamepad.
// STEP634: Spawn a real Barony barrel, not a dummy VR hitbox. Use the
// runtime's named collider definition, melee vulnerability, art and HP.
// Training props are available only in offline single-player to avoid
// affecting another person's dungeon or requiring a network protocol.
static Uint32 g_vrCalibrationBarrelUid = 0;
static void vrSpawnCalibrationBarrel()
{
    if (multiplayer != SINGLE || splitscreen ||
        clientnum < 0 || clientnum >= MAXPLAYERS || !players[clientnum] ||
        !players[clientnum]->entity || !map.entities)
    {
        messagePlayer(clientnum, MESSAGE_HINT,
            "VR training prop: only available in offline single player.");
        return;
    }
    if (g_vrCalibrationBarrelUid && uidToEntity(g_vrCalibrationBarrelUid))
    {
        messagePlayer(clientnum, MESSAGE_HINT,
            "VR training prop already exists. Break it before spawning another.");
        return;
    }

    // STEP636: The Steam game's native collider definitions can differ
    // between versions. We prefer a genuine damageable barrel/cask/keg,
    // but fall back to other suitable *native* breakables, never a dummy
    // sprite, fabricated collider ID or modified weapon damage/reach.
    // Entity::isDamageableCollider requires colliderMaxHP > 0; native
    // colliderAssignProperties obtains that from the JSON hp override.
    int trainingType = 0;
    int bestPriority = 1000;
    std::string trainingName;
    int barrelsFound = 0;
    int nativeMeleeBreakables = 0;
    for (const auto& pair : EditorEntityData_t::colliderData)
    {
        std::string name = pair.second.name;
        std::string hpName = pair.second.hpbarLookupName;
        for (char& c : name)
        {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        for (char& c : hpName)
        {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        const auto contains = [&](const char* term) -> bool {
            return name.find(term) != std::string::npos ||
                hpName.find(term) != std::string::npos;
        };
        const bool namedBarrel = contains("barrel") ||
            contains("cask") || contains("keg");
        if (namedBarrel) ++barrelsFound;
        const auto damage = EditorEntityData_t::colliderDmgTypes.find(
            pair.second.damageCalculationType);
        const auto hp = pair.second.overrideProperties.find("hp");
        if (damage == EditorEntityData_t::colliderDmgTypes.end() ||
            !damage->second.meleeAffects ||
            hp == pair.second.overrideProperties.end() || hp->second <= 0)
            continue;
        ++nativeMeleeBreakables;

        // Only known harmless training scenery, not walls/traps/fire.
        // A lower rank wins. Map key is the actual runtime collider ID.
        int priority = 1000;
        if (namedBarrel) priority = 0;
        else if (contains("crate")) priority = 10;
        else if (contains("sack") || contains("bag")) priority = 20;
        else if (contains("box") || contains("chest")) priority = 30;
        else if (contains("pot") || contains("vase")) priority = 40;
        else if (contains("chair") || contains("table")) priority = 50;
        if (priority < bestPriority)
        {
            bestPriority = priority;
            trainingType = pair.first;
            trainingName = pair.second.name;
        }
    }
    if (!trainingType)
    {
        printlog("OpenXR VR STEP636: no suitable training prop; collider definitions=%d, named barrel/cask/keg=%d, melee breakables=%d",
            static_cast<int>(EditorEntityData_t::colliderData.size()),
            barrelsFound, nativeMeleeBreakables);
        messagePlayer(clientnum, MESSAGE_HINT,
            "VR test prop unavailable: %d collider types, %d barrel names, %d melee breakables. See log.",
            static_cast<int>(EditorEntityData_t::colliderData.size()),
            barrelsFound, nativeMeleeBreakables);
        return;
    }

    Entity* const player = players[clientnum]->entity;
    // One nearby tile in front of the player: inside normal Barony
    // STRIKERANGE when the existing spawn helper finds valid floor space.
    const real_t spawnX = player->x + cos(player->yaw)*16.0;
    const real_t spawnY = player->y + sin(player->yaw)*16.0;
    Entity* const barrel = Entity::createBreakableCollider(
        trainingType, spawnX, spawnY, nullptr);
    if (!barrel)
    {
        messagePlayer(clientnum, MESSAGE_HINT,
            "VR test prop: no safe spawn tile nearby. Move to an open room.");
        return;
    }
    g_vrCalibrationBarrelUid = barrel->getUID();
    const char* kind = (bestPriority == 0) ? "BARREL" : "FALLBACK";
    printlog("OpenXR VR STEP636: spawned %s prop %s (collider ID %d, HP %d)",
        kind, trainingName.c_str(), trainingType, barrel->colliderMaxHP);
    messagePlayer(clientnum, MESSAGE_HINT,
        "VR test: spawned %s '%s'. Hold BOTH grips 3s to exit and strike it.",
        kind, trainingName.c_str());
}

// GA12: One page list drives both navigation and the visible page indicator.
enum class VrCalibrationPage { Arm, Item, Aim, Spell, Wrist, Status, Belt, Xp, Party, Skill, Flail, Bow };
static std::vector<VrCalibrationPage> vrCalibrationPages()
{
    std::vector<VrCalibrationPage> pages{VrCalibrationPage::Arm};
    if (vrCalibrationEquippedItem(g_vrArmCalibrationSelected))
        pages.push_back(VrCalibrationPage::Item);
    if (g_vrArmCalibrationSelected == 0)
        pages.push_back(VrCalibrationPage::Spell);
    else if (vrCalibrationRangedItem())
        pages.push_back(VrCalibrationPage::Aim);
    pages.push_back(VrCalibrationPage::Wrist);
    if (g_vrArmCalibrationSelected == 0)
        for (auto page : {VrCalibrationPage::Status, VrCalibrationPage::Belt,
            VrCalibrationPage::Xp, VrCalibrationPage::Party,
            VrCalibrationPage::Skill, VrCalibrationPage::Flail}) pages.push_back(page);
    pages.push_back(VrCalibrationPage::Bow);
    return pages;
}
static VrCalibrationPage vrCalibrationCurrentPage()
{
    if (g_vrArmCalibrationEditBow) return VrCalibrationPage::Bow;
    if (g_vrArmCalibrationEditFlail) return VrCalibrationPage::Flail;
    switch (g_vrArmCalibrationSpatialPage) {
    case 0: return VrCalibrationPage::Belt;
    case 3: return VrCalibrationPage::Xp;
    case 4: return VrCalibrationPage::Party;
    case 5: return VrCalibrationPage::Skill;
    default: break;
    }
    if (g_vrArmCalibrationEditStatusEffects) return VrCalibrationPage::Status;
    if (g_vrArmCalibrationEditWearable) return VrCalibrationPage::Wrist;
    if (g_vrArmCalibrationEditSpellAim) return VrCalibrationPage::Spell;
    if (g_vrArmCalibrationEditAim) return VrCalibrationPage::Aim;
    if (g_vrArmCalibrationEditItem) return VrCalibrationPage::Item;
    return VrCalibrationPage::Arm;
}
static const char* vrCalibrationPageName(VrCalibrationPage page)
{
    switch (page) {
    case VrCalibrationPage::Bow: return "Bow test";
    case VrCalibrationPage::Arm: return "Arm";
    case VrCalibrationPage::Item: return "Equipment";
    case VrCalibrationPage::Aim: return "Weapon aim";
    case VrCalibrationPage::Spell: return "Spell aim";
    case VrCalibrationPage::Wrist: return "Wrist HUD";
    case VrCalibrationPage::Status: return "Status icons";
    case VrCalibrationPage::Belt: return "Belt HUD";
    case VrCalibrationPage::Xp: return "XP bar";
    case VrCalibrationPage::Party: return "Party bars";
    case VrCalibrationPage::Skill: return "Skill-ups";
    case VrCalibrationPage::Flail: return "Flail";
    }
    return "Arm";
}
static void vrCalibrationStepPage(bool backwards)
{
    const auto pages = vrCalibrationPages();
    const auto current = vrCalibrationCurrentPage();
    const auto found = std::find(pages.begin(), pages.end(), current);
    const int count = static_cast<int>(pages.size());
    const int index = found == pages.end() ? -1 : static_cast<int>(found - pages.begin());
    const auto next = pages[index < 0 ? (backwards ? count - 1 : 0) :
        (index + (backwards ? count - 1 : 1)) % count];
    if (g_vrArmCalibrationEditFlail && g_vrFlailSettingsDirty) vrSaveModeSettings();
    g_vrArmCalibrationEditItem = next == VrCalibrationPage::Item;
    g_vrArmCalibrationEditAim = next == VrCalibrationPage::Aim;
    g_vrArmCalibrationEditSpellAim = next == VrCalibrationPage::Spell;
    g_vrArmCalibrationEditWearable = next == VrCalibrationPage::Wrist;
    g_vrArmCalibrationEditStatusEffects = next == VrCalibrationPage::Status;
    g_vrArmCalibrationEditFlail = next == VrCalibrationPage::Flail;
    g_vrArmCalibrationEditBow = next == VrCalibrationPage::Bow;
    g_vrArmCalibrationSpatialPage = next == VrCalibrationPage::Belt ? 0 :
        next == VrCalibrationPage::Xp ? 3 : next == VrCalibrationPage::Party ? 4 :
        next == VrCalibrationPage::Skill ? 5 : -1;
}

static void vrUpdateVisualArmCalibration(XrTime predictedDisplayTime)
{
    vrLoadVisualArmCalibration();
    vrLoadMeleeTuning();
    const Uint32 now = SDL_GetTicks();
    const auto& left = g_xrControllerHands[0];
    const auto& right = g_xrControllerHands[1];
    const bool available = g_xrSessionState == XR_SESSION_STATE_FOCUSED &&
        !intro && !gamePaused && clientnum >= 0 && clientnum < MAXPLAYERS &&
        players[clientnum] && players[clientnum]->entity &&
        players[clientnum]->shootmode;
    if (!available)
    {
        if (g_vrArmCalibrationActive && g_vrArmCalibrationDirty)
            vrSaveVisualArmCalibration();
        if (g_vrFlailSettingsDirty) vrSaveModeSettings();
        if (g_vrMeleeTuningDirty) vrSaveMeleeTuning();
        g_vrArmCalibrationActive = false;
        g_vrBodyCalibrationPage = false;
        vrResetBodyPoseSamples();
        g_vrArmCalibrationEditMelee = false;
        g_vrArmCalibrationEditSpellAim = false;
        g_vrArmCalibrationEditWearable = false;
        g_vrArmCalibrationEditStatusEffects = false;
        g_vrArmCalibrationSpatialPage = -1;
        g_vrArmCalibrationEditFlail = false;
        g_vrArmCalibrationEditBow = false;
        g_vrMeleeGripChordStartedAt = 0;
        g_vrMeleeGripChordLatched = false;
        g_vrMeleeExitGripsSuppressed = false;
        g_vrArmCalibrationChordLatched = false;
        g_vrArmCalibrationChordStart = 0;
        g_vrArmCalibrationLastSample = now;
        return;
    }
    // Both analog GRIP buttons pressed simultaneously for 3000ms.
    // Require release between transitions, so entry cannot instantly exit.
    const bool bothGrips = left.squeeze >= 0.72f && right.squeeze >= 0.72f;
    if (left.squeeze < 0.48f && right.squeeze < 0.48f)
        g_vrMeleeExitGripsSuppressed = false;
    if (!g_vrArmCalibrationActive || !bothGrips || g_vrBodyCalibrationPage || g_vrArmCalibrationEditBow)
    {
        g_vrMeleeGripChordStartedAt = 0;
        g_vrMeleeGripChordLatched = false;
    }
    else
    {
        if (!g_vrMeleeGripChordStartedAt) g_vrMeleeGripChordStartedAt = now;
        if (!g_vrMeleeGripChordLatched &&
            now - g_vrMeleeGripChordStartedAt >= 3000u)
        {
            g_vrMeleeGripChordLatched = true;
            g_vrArmCalibrationLastSample = now;
            g_vrArmCalibrationResetStart = 0;
            g_vrArmCalibrationResetLatched = false;
            if (g_vrArmCalibrationEditMelee)
            {
                // Save current melee settings and leave BOTH calibration
                // screens, returning straight to ordinary gameplay.
                if (g_vrMeleeTuningDirty) vrSaveMeleeTuning();
                if (g_vrArmCalibrationDirty) vrSaveVisualArmCalibration();
                g_vrArmCalibrationEditMelee = false;
                g_vrArmCalibrationActive = false;
                g_vrMeleeExitGripsSuppressed = true;
                // Prevent a backpack/use edge in this exit frame.
                g_xrVrBackpackGripWasDown = true;
                g_xrVrBackpackGripConsumed = true;
                messagePlayer(clientnum, MESSAGE_HINT,
                    "VR MELEE CAL SAVED - back to game. Release BOTH grips.");
                std::printf("OpenXR VR: STEP632 melee calibration saved / exited\n");
            }
            else if (stats[clientnum] && stats[clientnum]->weapon &&
                (isMeleeWeapon(*stats[clientnum]->weapon) ||
                 stats[clientnum]->weapon->type == TOOL_PICKAXE ||
                 stats[clientnum]->weapon->type == TOOL_WHIP))
            {
                g_vrArmCalibrationSelected = 1;
                g_vrArmCalibrationEditItem = false;
                g_vrArmCalibrationEditAim = false;
                g_vrArmCalibrationEditSpellAim = false;
                g_vrArmCalibrationEditWearable = false;
                g_vrArmCalibrationEditStatusEffects = false;
                g_vrArmCalibrationSpatialPage = -1;
                g_vrArmCalibrationEditFlail = false;
        g_vrArmCalibrationEditBow = false;
                g_vrArmCalibrationEditMelee = true;
                messagePlayer(clientnum, MESSAGE_HINT,
                    "VR MELEE CAL: gesture-only dry run; hold BOTH grips 3s to save and exit game.");
                vrShowVisualArmStatus(false);
                std::printf("OpenXR VR: STEP632 melee calibration entered\n");
            }
            else
                messagePlayer(clientnum, MESSAGE_HINT,
                    "Equip a RIGHT-hand melee weapon, then hold BOTH grips 3s.");
        }
    }
    // X+Y continues to open/close the original visual editor, but cannot
    // accidentally exit the melee editor (which has its own grip shortcut).
    const bool chord = left.primaryButton && left.secondaryButton;
    if (!chord)
    {
        g_vrArmCalibrationChordStart = 0;
        g_vrArmCalibrationChordLatched = false;
    }
    else
    {
        if (!g_vrArmCalibrationChordStart) g_vrArmCalibrationChordStart = now;
        if (!g_vrArmCalibrationEditMelee &&
            !g_vrArmCalibrationChordLatched &&
            now - g_vrArmCalibrationChordStart >= 700u)
        {
            g_vrArmCalibrationChordLatched = true;
            g_vrArmCalibrationActive = !g_vrArmCalibrationActive;
            g_vrArmCalibrationLastSample = now;
            g_vrArmCalibrationLastMessage = now;
            g_vrArmCalibrationLeftClickPrevious = left.thumbstickClick;
            g_vrArmCalibrationRightClickPrevious = right.thumbstickClick;
            if (g_vrArmCalibrationActive)
            {
                messagePlayer(clientnum, MESSAGE_HINT,
                    "VR CAL ON. Hold BOTH grips 3s for MELEE calibration.");
                vrShowVisualArmStatus(true);
                vrShowVisualArmStatus(false);
            }
            else
            {
                g_vrArmCalibrationEditMelee = false;
                g_vrBodyCalibrationPage = false;
                vrResetBodyPoseSamples();
                g_vrArmCalibrationEditSpellAim = false;
                g_vrArmCalibrationEditWearable = false;
                g_vrArmCalibrationEditStatusEffects = false;
                g_vrArmCalibrationSpatialPage = -1;
                g_vrArmCalibrationEditFlail = false;
        g_vrArmCalibrationEditBow = false;
                if (g_vrFlailSettingsDirty) vrSaveModeSettings();
                if (g_vrArmCalibrationDirty) vrSaveVisualArmCalibration();
                if (g_vrMeleeTuningDirty) vrSaveMeleeTuning();
                messagePlayer(clientnum, MESSAGE_HINT,
                    "VR CAL OFF: saved. Hold X+Y to adjust again.");
            }
            std::printf("OpenXR VR: STEP606 CAL_%s\n",
                g_vrArmCalibrationActive ? "ON" : "OFF");
        }
    }
    const bool aPressed = right.primaryButton;
    const bool bPressed = right.secondaryButton;
    const bool barrelSpawnPressed = g_vrArmCalibrationEditMelee &&
        aPressed && !g_vrArmCalibrationAPrevious;
    if (!g_vrArmCalibrationActive)
    {
        g_vrArmCalibrationAPrevious = aPressed;
        g_vrArmCalibrationBPrevious = bPressed;
        g_vrArmCalibrationRightClickPrevious = right.thumbstickClick;
        g_vrArmCalibrationLastSample = now;
        return;
    }
    // STEP638FZ8: Hold A+B for 0.7s while editing to toggle BODY FIT.
    // It never steals the existing X+Y calibration chord or either grip.
    const bool bodyChord = aPressed && bPressed;
    if (!bodyChord) { g_vrBodyToggleStarted = 0; g_vrBodyToggleLatched = false; }
    else if (!g_vrArmCalibrationEditMelee)
    {
        if (!g_vrBodyToggleStarted) g_vrBodyToggleStarted = now;
        if (!g_vrBodyToggleLatched && now - g_vrBodyToggleStarted >= 700u)
        {
            g_vrBodyToggleLatched = true;
            g_vrBodyCalibrationPage = !g_vrBodyCalibrationPage;
            vrResetBodyPoseSamples();
            g_vrBodyPoseLatched = false;
            g_vrBodyResetStarted = 0;
            g_vrBodyResetLatched = false;
            vrLoadBodyFit();
            if (g_vrBodyCalibrationPage)
            {
                g_vrArmCalibrationEditItem = false;
                g_vrArmCalibrationEditAim = false;
                g_vrArmCalibrationEditSpellAim = false;
                g_vrArmCalibrationEditWearable = false;
                g_vrArmCalibrationEditStatusEffects = false;
                g_vrArmCalibrationSpatialPage = -1;
                g_vrArmCalibrationEditFlail = false;
        g_vrArmCalibrationEditBow = false;
            }
            messagePlayer(clientnum, MESSAGE_HINT,
                g_vrBodyCalibrationPage ?
                "VR BODY FIT: T-pose, hold both GRIPS for 2s. A+B to exit." :
                "VR BODY FIT editor closed. Existing equipment grips preserved.");
        }
    }
    if (g_vrBodyCalibrationPage)
    {
        vrUpdateBodyPoseCalibration(predictedDisplayTime);
        g_vrArmCalibrationAPrevious = aPressed;
        g_vrArmCalibrationBPrevious = bPressed;
        g_vrArmCalibrationLeftClickPrevious = left.thumbstickClick;
        g_vrArmCalibrationRightClickPrevious = right.thumbstickClick;
        g_vrArmCalibrationLastSample = now;
        return;
    }
    if (!g_vrArmCalibrationEditMelee)
    {
    // STEP618: in LEFT ARM calibration mode, click only the RIGHT stick
    // to preview each left-hand mesh. Avoid the both-sticks menu chord.
    if (right.thumbstickClick && !g_vrArmCalibrationRightClickPrevious &&
        !left.thumbstickClick && g_vrArmCalibrationSelected == 0 &&
        !g_vrArmCalibrationEditBow &&
        !g_vrArmCalibrationEditItem && !g_vrArmCalibrationEditAim &&
        !g_vrArmCalibrationEditSpellAim &&
        !g_vrArmCalibrationEditStatusEffects &&
        g_vrArmCalibrationSpatialPage < 0 &&
        !g_vrArmCalibrationEditWearable &&
        !g_vrArmCalibrationEditMelee && !chord)
    {
        g_vrLeftArmMeshMode = (g_vrLeftArmMeshMode + 1) % 4;
        g_vrArmCalibrationDirty = true;
        messagePlayer(clientnum, MESSAGE_HINT,
            "VR LEFT ARM MODEL: %s (right stick click to compare)",
            vrLeftArmMeshName());
    }
    if(g_vrArmCalibrationEditBow && right.thumbstickClick &&
        !g_vrArmCalibrationRightClickPrevious && !left.thumbstickClick && !chord) {
        g_vrBowEditGrip=!g_vrBowEditGrip;
        g_vrBowPreview.drawing=false;g_vrBowPreview.armed=false;
        g_vrArmCalibrationResetStart=0;g_vrArmCalibrationResetLatched=false;
    }
    // STEP638FQ: quick access for the right-hand flail visual pivot.
    // The original page cycle reached the flail page only through the
    // left-hand HUD pages; right-hand users had no discoverable path.
    if (right.thumbstickClick && !g_vrArmCalibrationRightClickPrevious &&
        !left.thumbstickClick && !chord &&
        g_vrArmCalibrationSelected == 1 && !g_vrArmCalibrationEditMelee)
    {
        const Item* equipped = vrCalibrationEquippedItem(1);
        if (equipped && equipped->type == STEEL_FLAIL)
        {
            const bool openFlail = !g_vrArmCalibrationEditFlail;
            g_vrArmCalibrationEditItem = false;
            g_vrArmCalibrationEditAim = false;
            g_vrArmCalibrationEditSpellAim = false;
            g_vrArmCalibrationEditWearable = false;
            g_vrArmCalibrationEditStatusEffects = false;
            g_vrArmCalibrationSpatialPage = -1;
            g_vrArmCalibrationEditFlail = openFlail;
            g_vrArmCalibrationEditBow = false;
            g_vrArmCalibrationResetStart = 0;
            g_vrArmCalibrationResetLatched = false;
            if (!openFlail && g_vrFlailSettingsDirty) vrSaveModeSettings();
            messagePlayer(clientnum, MESSAGE_HINT,
                openFlail ? "VR FLAIL PIVOT: live anchor editing ON" :
                            "VR FLAIL PIVOT: saved; right arm editing ON");
            vrShowVisualArmStatus(false);
        }
    }
    g_vrArmCalibrationRightClickPrevious = right.thumbstickClick;
    if (aPressed && !bPressed && !g_vrArmCalibrationAPrevious)
    {
        g_vrArmCalibrationSelected = 1;
        g_vrArmCalibrationEditItem = false;
        g_vrArmCalibrationEditAim = false;
        g_vrArmCalibrationEditSpellAim = false;
        g_vrArmCalibrationEditWearable = false;
        g_vrArmCalibrationEditStatusEffects = false;
        g_vrArmCalibrationSpatialPage = -1;
        g_vrArmCalibrationEditFlail = false;
        g_vrArmCalibrationEditBow = false;
        g_vrArmCalibrationEditMelee = false;
        vrShowVisualArmStatus(false);
    }
    if (bPressed && !aPressed && !g_vrArmCalibrationBPrevious)
    {
        g_vrArmCalibrationSelected = 0;
        g_vrArmCalibrationEditItem = false;
        g_vrArmCalibrationEditAim = false;
        g_vrArmCalibrationEditSpellAim = false;
        g_vrArmCalibrationEditWearable = false;
        g_vrArmCalibrationEditStatusEffects = false;
        g_vrArmCalibrationSpatialPage = -1;
        g_vrArmCalibrationEditFlail = false;
        g_vrArmCalibrationEditBow = false;
        g_vrArmCalibrationEditMelee = false;
        vrShowVisualArmStatus(false);
    }
    g_vrArmCalibrationAPrevious = aPressed;
    g_vrArmCalibrationBPrevious = bPressed;
    // STEP638EI: add a dedicated wearable page for either wrist, without
    // stealing the existing arm/item/spell/weapon calibration controls.
    if (left.thumbstickClick && !g_vrArmCalibrationLeftClickPrevious && !chord)
    {
        // Hold left grip while clicking to browse backward; keep click edge gating.
        vrCalibrationStepPage(left.squeeze >= .72f);
        g_vrArmCalibrationResetStart = 0;
        g_vrArmCalibrationResetLatched = false;
        vrShowVisualArmStatus(false);
    }
    g_vrArmCalibrationLeftClickPrevious = left.thumbstickClick;
    if (g_vrArmCalibrationEditAim && !vrCalibrationRangedItem())
        g_vrArmCalibrationEditAim = false;
    if (g_vrArmCalibrationEditItem &&
        !vrCalibrationEquippedItem(g_vrArmCalibrationSelected))
        g_vrArmCalibrationEditItem = false;
    }
    else
    {
        if (barrelSpawnPressed) vrSpawnCalibrationBarrel();
        g_vrArmCalibrationAPrevious = aPressed;
        g_vrArmCalibrationBPrevious = bPressed;
        g_vrArmCalibrationLeftClickPrevious = left.thumbstickClick;
        g_vrArmCalibrationRightClickPrevious = right.thumbstickClick;
    }
    if (g_vrArmCalibrationEditBow) {
        static bool choiceHeld=false;
        const float dt=std::min(.06f,static_cast<float>(now-g_vrArmCalibrationLastSample)/1000.f);
        g_vrArmCalibrationLastSample=now;
        if(g_vrBowEditGrip) {
            auto& g=g_vrBowGrip[1-g_vrBowDrawHand];
            const auto dz=[](float v){return std::fabs(v)<.20f?0.f:v;};
            const float lx=dz(left.thumbstick.x),ly=dz(left.thumbstick.y);
            const float rx=dz(right.thumbstick.x),ry=dz(right.thumbstick.y);
            if(!chord && (lx||ly||rx||ry)) {
                const int bowHand=1-g_vrBowDrawHand;
                if(left.squeeze>.65f)
                    vrBowAdjustControllerRotation(bowHand,0,lx*dt);
                else {
                    vrBowAdjustControllerRotation(bowHand,1,-lx*dt);
                    vrBowAdjustControllerRotation(bowHand,2,ly*dt);
                }
                g.side+=rx*.10f*dt;
                if(right.squeeze>.65f)g.forward+=ry*.10f*dt;
                else g.up+=ry*.10f*dt;
                g.forward=std::max(-.30f,std::min(.30f,g.forward));
                g.up=std::max(-.30f,std::min(.30f,g.up));
                g.side=std::max(-.30f,std::min(.30f,g.side));
                for(float* angle:{&g.yaw,&g.pitch,&g.roll})
                    *angle=std::max(-3.14159f,std::min(3.14159f,*angle));
                g_vrArmCalibrationDirty=true;
            }
            if(right.trigger>=.90f) {
                if(!g_vrArmCalibrationResetStart)g_vrArmCalibrationResetStart=now;
                if(!g_vrArmCalibrationResetLatched && now-g_vrArmCalibrationResetStart>=1500u) {
                    g={};g_vrBowZRotation[1-g_vrBowDrawHand]=0.f;
                    g_vrBowControllerRotation[1-g_vrBowDrawHand]={0,0,0,1};
                    g_vrArmCalibrationDirty=true;g_vrArmCalibrationResetLatched=true;
                }
            } else {g_vrArmCalibrationResetStart=0;g_vrArmCalibrationResetLatched=false;}
        } else {
            const bool choice=std::fabs(right.thumbstick.x)>.65f;
            if(!choice)choiceHeld=false;
            if(choice && !choiceHeld) {
                choiceHeld=true;g_vrBowDrawHand=right.thumbstick.x>0.f?1:0;
                g_vrBowPreview.drawing=false;g_vrBowPreview.armed=false;
                g_vrArmCalibrationDirty=true;
            }
            if(std::fabs(right.thumbstick.y)>.20f) {
                g_vrBowFullDraw=std::max(.20f,std::min(.65f,g_vrBowFullDraw+right.thumbstick.y*.12f*dt));
                g_vrArmCalibrationDirty=true;
            }
            g_vrArmCalibrationResetStart=0;g_vrArmCalibrationResetLatched=false;
        }
        return;
    }
    // Reset requires 1.5s continuous trigger press; reset only the selected visual.
    if (right.trigger >= .90f)
    {
        if (!g_vrArmCalibrationResetStart) g_vrArmCalibrationResetStart = now;
        if (!g_vrArmCalibrationResetLatched &&
            now - g_vrArmCalibrationResetStart >= 1500u)
        {
            if (g_vrArmCalibrationEditFlail)
            {
                g_vrFlailAnchorForward = 2.4f;
                g_vrFlailAnchorUp = 0.240f;
                g_vrFlailAnchorSide = 0.f;
                g_vrFlailChainLength = 1.8f;
                g_vrFlailDamping = 0.45f;
                g_vrFlailModelSize = 1.5f; // Restore Step638FU's visual size.
                g_vrFlailSettingsDirty = true;
            }
            else if (g_vrArmCalibrationEditMelee)
            {
                g_vrMeleeTuning = {};
                g_vrMeleeTuningDirty = true;
            }
            else if (g_vrArmCalibrationSpatialPage >= 0)
            {
                g_vrSpatialHud[g_vrArmCalibrationSpatialPage] =
                    { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f };
            }
            else if (g_vrArmCalibrationEditStatusEffects)
            {
                g_vrStatusEffectsHud = g_vrStatusEffectsHudDefault;
            }
            else if (g_vrArmCalibrationEditWearable)
            {
                g_vrWearableHud[g_vrArmCalibrationSelected] =
                    g_vrWearableHudDefaults[g_vrArmCalibrationSelected];
            }
            else if (g_vrArmCalibrationEditSpellAim)
            {
                g_vrSpellAimVisual = {};
            }
            else if (g_vrArmCalibrationEditAim)
            {
                if (const Item* item = vrCalibrationRangedItem())
                    g_vrRangedAimVisual.erase(static_cast<int>(item->type));
            }
            else if (auto* selected = vrCalibrationWritableSelection())
            {
                *selected = {};
            }
            if (!g_vrArmCalibrationEditMelee && !g_vrArmCalibrationEditFlail)
                g_vrArmCalibrationDirty = true;
            g_vrArmCalibrationResetLatched = true;
            messagePlayer(clientnum, MESSAGE_HINT,
                "Selected %s reset to defaults.",
                g_vrArmCalibrationEditFlail ? "flail pivot" :
                (g_vrArmCalibrationEditMelee ? "melee tuning" :
                    (g_vrArmCalibrationEditStatusEffects ? "status icons" :
                    (g_vrArmCalibrationEditSpellAim ? "left spell aim" :
                        (g_vrArmCalibrationEditAim ? "projectile aim" :
                            (g_vrArmCalibrationEditItem ? "equipment" : "arm"))))));
        }
    }
    else
    {
        g_vrArmCalibrationResetStart = 0;
        g_vrArmCalibrationResetLatched = false;
    }
    float dt = static_cast<float>(now - g_vrArmCalibrationLastSample) / 1000.f;
    g_vrArmCalibrationLastSample = now;
    if (dt > .06f) dt = .06f;
    auto deadzone = [](float v) { return std::fabs(v) < .16f ? 0.f : v; };
    const float lx = deadzone(left.thumbstick.x);
    const float ly = deadzone(left.thumbstick.y);
    const float rx = deadzone(right.thumbstick.x);
    const float ry = deadzone(right.thumbstick.y);
    if ((lx != 0.f || ly != 0.f || rx != 0.f || ry != 0.f) &&
        !chord && !bothGrips)
    {
        if (g_vrArmCalibrationEditFlail)
        {
            // Cosmetics only: RIGHT stick moves tip (local side/up), right
            // grip + right Y adjusts forward. LEFT grip + LEFT X tunes
            // the actual voxel model size, separately from chain length.
            const float step = 0.75f * dt;
            g_vrFlailAnchorSide = std::max(-4.f, std::min(4.f,
                g_vrFlailAnchorSide + step * rx));
            if (right.squeeze >= .65f)
                g_vrFlailAnchorForward = std::max(0.f, std::min(8.f,
                    g_vrFlailAnchorForward + step * ry));
            else
                g_vrFlailAnchorUp = std::max(-4.f, std::min(4.f,
                    g_vrFlailAnchorUp + step * ry));
            if (left.squeeze >= .65f)
                g_vrFlailModelSize = std::max(.6f, std::min(3.f,
                    g_vrFlailModelSize + .7f * dt * lx));
            else
                g_vrFlailChainLength = std::max(.5f, std::min(5.f,
                    g_vrFlailChainLength + .7f * dt * lx));
            g_vrFlailDamping = std::max(.1f, std::min(5.f,
                g_vrFlailDamping + .8f * dt * ly));
            g_vrFlailSettingsDirty = true;
        }
        else if (g_vrArmCalibrationEditMelee)
        {
            // Holding a stick adjusts values smoothly; autosave and X+Y
            // exit work exactly as with weapon/arm calibration.
            auto& v = g_vrMeleeTuning;
            v.minTipSpeed = std::max(0.8f, std::min(4.5f,
                v.minTipSpeed + 1.0f * dt * lx));
            v.minGripSpeed = std::max(0.2f, std::min(2.5f,
                v.minGripSpeed + 0.75f * dt * ly));
            v.rearmSpeed = std::max(0.08f, std::min(0.70f,
                v.rearmSpeed + 0.25f * dt * rx));
            g_vrMeleeTuningDirty = true;
        }
        else if (g_vrArmCalibrationEditWearable || g_vrArmCalibrationEditStatusEffects ||
            g_vrArmCalibrationSpatialPage >= 0)
        {
            auto& w = g_vrArmCalibrationSpatialPage >= 0 ?
                g_vrSpatialHud[g_vrArmCalibrationSpatialPage] :
                (g_vrArmCalibrationEditStatusEffects ?
                g_vrStatusEffectsHud : g_vrWearableHud[g_vrArmCalibrationSelected]);
            const float angle = dt * .75f;
            const float move = dt * 1.0f;
            if (left.squeeze >= .65f) w.roll += angle * lx;
            else w.yaw += angle * lx;
            if (left.squeeze < .65f) w.pitch += angle * ly;
            if (right.squeeze >= .65f)
            {
                w.scale = std::max(.35f, std::min(3.f,
                    w.scale + .7f * dt * rx));
                w.forward = std::max(-4.f, std::min(4.f, w.forward + move * ry));
            }
            else
            {
                w.side = std::max(-4.f, std::min(4.f, w.side + move * rx));
                w.up = std::max(-4.f, std::min(4.f, w.up + move * ry));
            }
            g_vrArmCalibrationDirty = true;
        }
        else if (g_vrArmCalibrationEditSpellAim)
        {
            const float speed = dt * .40f;
            g_vrSpellAimVisual.yaw = std::max(-VR_SPELL_AIM_MAX_YAW,
                std::min(VR_SPELL_AIM_MAX_YAW,
                    g_vrSpellAimVisual.yaw + speed * lx));
            g_vrSpellAimVisual.pitch = std::max(-VR_SPELL_AIM_MAX_PITCH,
                std::min(VR_SPELL_AIM_MAX_PITCH,
                    g_vrSpellAimVisual.pitch + speed * ly));
            if (lx != 0.f || ly != 0.f) g_vrArmCalibrationDirty = true;
        }
        else if (g_vrArmCalibrationEditAim)
        {
            if (const Item* equipped = vrCalibrationRangedItem())
            {
                // +/- 20 degrees; fine aiming, not a new weapon grip pose.
                // Positive yaw moves shots toward controller-right; positive
                // pitch moves shots toward controller-up. Shared ray keeps
                // the reticle and projectile perfectly aligned.
                auto& aim = g_vrRangedAimVisual[static_cast<int>(equipped->type)];
                const float speed = dt * .40f; // ~23 degrees/sec full stick
                aim.yaw = std::max(-.35f, std::min(.35f, aim.yaw + speed * lx));
                aim.pitch = std::max(-.35f, std::min(.35f, aim.pitch + speed * ly));
                if (lx != 0.f || ly != 0.f) g_vrArmCalibrationDirty = true;
            }
        }
        else if (auto* target = vrCalibrationWritableSelection())
        {
            const float angle = dt * .75f; // radians/sec, ~43deg/sec full stick
            const float distance = dt * 1.f; // Barony units/sec
            if (left.squeeze >= .65f) target->roll += angle * lx;
            else target->yaw += angle * lx;
            if (left.squeeze < .65f) target->pitch += angle * ly;
            target->side += distance * rx;
            // STEP638CT LEFT ARM SIZE EDITOR RESTORED.
            // LEFT ARM only: LEFT grip + RIGHT stick up/down changes size.
            // Preserve all other calibration gestures and item adjustments.
            if (!g_vrArmCalibrationEditItem && left.squeeze >= .65f &&
                right.squeeze < .65f)
            {
                auto& scale = g_vrArmCalibrationSelected == 0 ?
                    g_vrLeftArmScaleFactor : g_vrRightArmScaleFactor;
                scale = std::max(0.70f,std::min(3.50f,scale+0.65f*dt*ry));
            }
            else if (right.squeeze >= .65f) target->forward += distance * ry;
            else target->up += distance * ry;
            g_vrArmCalibrationDirty = true;
        }
    }
    // STEP606: persistent help panel holds the instructions/values in view.
    // Do not spam messagePlayer every few seconds and scroll the chat HUD.
    if (g_vrArmCalibrationDirty && now - g_vrArmCalibrationLastSave >= 2500u)
        vrSaveVisualArmCalibration();
    if (g_vrMeleeTuningDirty && now - g_vrMeleeLastSave >= 2500u)
        vrSaveMeleeTuning();
}

// Both controller positions and the VIEW/head space are located in the same
// LOCAL space. The shoulder region turns with the player's headset yaw.
static void vrUpdateVRShortcutGestures(XrTime predictedDisplayTime)
{
    const VrControllerHandState& left = g_xrControllerHands[0];
    const VrControllerHandState& right = g_xrControllerHands[1];

    // BOTH-stick click: short press opens Menu (as before), long hold (1.5s)
    // switches Standing/Seated. Long hold with RIGHT grip held recalibrates
    // the current mode instead. No menu pulse is sent for long holds.
    const bool bothClicks = left.thumbstickClick && right.thumbstickClick;
    const Uint32 chordNow = SDL_GetTicks();
    if (bothClicks && !g_xrVrMenuChordActive)
    {
        g_xrVrMenuChordActive = true;
        g_xrVrMenuChordHeldAction = false;
        g_xrVrMenuChordStartMs = chordNow;
    }
    if (bothClicks && g_xrVrMenuChordActive &&
        !g_xrVrMenuChordHeldAction &&
        chordNow - g_xrVrMenuChordStartMs >= 1500)
    {
        g_xrVrMenuChordHeldAction = true;
        if (right.squeeze >= 0.65f)
            vrRequestRoomCalibration();
        else
            vrTogglePlayMode();
    }
    if (!bothClicks && g_xrVrMenuChordActive)
    {
        if (!g_xrVrMenuChordHeldAction)
        {
            g_xrVrMenuPulseUntilMs = chordNow + 125;
            std::printf("OpenXR VR: BOTH_STICKS short press -> MENU (Start)\n");
        }
        g_xrVrMenuChordActive = false;
    }

    // Backpack: reach the right controller behind your right shoulder, then
    // squeeze the right grip. Suppress its RB/shoulder action while within the
    // backpack zone, including the beginning of a gradual grip press.
    bool headValid = false;
    float side = 0.f, behind = 0.f, height = 0.f;
    if (g_xrGuiViewSpace != XR_NULL_HANDLE && right.gripPoseValid)
    {
        XrSpaceLocation headLocation{};
        headLocation.type = XR_TYPE_SPACE_LOCATION;
        const XrResult headResult = xrLocateSpace(
            g_xrGuiViewSpace, g_xrReferenceSpace,
            predictedDisplayTime, &headLocation);
        if (XR_SUCCEEDED(headResult) &&
            (headLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (headLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        {
            const XrPosef& head = headLocation.pose;
            const XrQuaternionf& q = head.orientation;
            // Rotate the headset's forward (-Z) onto the horizontal plane.
            const float forwardX = -2.f * (q.x * q.z + q.y * q.w);
            const float forwardZ = -(1.f - 2.f * (q.x * q.x + q.y * q.y));
            const float len = std::sqrt(forwardX * forwardX +
                forwardZ * forwardZ);
            if (len > 0.25f) // Avoid unstable yaw when looking straight up.
            {
                const float dx = right.gripPose.position.x - head.position.x;
                const float dz = right.gripPose.position.z - head.position.z;
                const float fx = forwardX / len;
                const float fz = forwardZ / len;
                side = dx * (-fz) + dz * fx; // + means player's right
                behind = -(dx * fx + dz * fz); // + means behind the player
                height = right.gripPose.position.y - head.position.y;
                headValid = true;
            }
        }
    }

    // STEP 625: One backpack gesture for opening Inventory and closing ANY
    // in-dungeon GUI (including Assist Shrines).  Never use it in the main
    // menu, character creation, or the pause/settings screen: Quest B already
    // provides Escape/Back there, unchanged from Step 624.
    Player* const dungeonPlayer =
        (clientnum >= 0 && clientnum < MAXPLAYERS) ? players[clientnum] : nullptr;
    const bool inDungeon = !intro && !gamePaused && dungeonPlayer &&
        dungeonPlayer->entity;
    const bool dungeonMenuOpen = inDungeon && vrMenuPointerMode();
    const bool bowOwnsGrip = vrPhysicalBowOwnsAttack(clientnum) &&
        (g_vrBowPreview.drawing || g_vrBowRelease.pending);
    const bool backpackAllowed = inDungeon && !dungeonPlayer->usingCommand() && !bowOwnsGrip;

    // Keep the SAME shoulder volume for opening and closing.  Suppress the
    // normal grip/right-click while inside that volume.
    g_xrVrBackpackInZone = backpackAllowed && headValid &&
        side >= 0.10f && side <= 0.65f &&
        behind >= 0.09f && behind <= 0.60f &&
        height >= -0.60f && height <= 0.18f;

    const bool gripDown = right.squeeze >= 0.65f;
    if (g_vrMeleeExitGripsSuppressed)
    {
        g_xrVrBackpackGripWasDown = true;
        return;
    }
    if (!gripDown)
    {
        g_xrVrBackpackGripConsumed = false;
    }
    if (gripDown && !g_xrVrBackpackGripWasDown && backpackAllowed)
    {
        if (g_xrVrBackpackInZone)
        {
            if (dungeonMenuOpen)
            {
                // Do not close while dragging an item or holding a mouse
                // button: the native inventory must receive that release.
                auto* interaction = inputs.getUIInteraction(clientnum);
                if (!g_xrVrPointerMouseDown &&
                    !g_xrVrPointerRightMouseDown &&
                    !(interaction && interaction->selectedItem))
                {
                    g_xrVrBackpackPulseUntilMs = 0;
                    g_xrVrBackpackGripConsumed = true;
                    vrResetPointer();
                    dungeonPlayer->closeAllGUIs(CLOSEGUI_ENABLE_SHOOTMODE,
                        CLOSEGUI_CLOSE_ALL);
                    std::printf("OpenXR VR: STEP625 BACKPACK -> CLOSE DUNGEON GUI\n");
                }
                else
                {
                    g_xrVrBackpackGripConsumed = true;
                    std::printf("OpenXR VR: STEP625 BACKPACK close deferred (item/drag active)\n");
                }
            }
            else
            {
                // Keep Step 624's proven Xbox Back inventory-open pulse.
                g_xrVrBackpackPulseUntilMs = SDL_GetTicks() + 125;
                g_xrVrBackpackGripConsumed = true;
                std::printf("OpenXR VR: STEP625 BACKPACK -> OPEN INVENTORY (Back)\n");
            }
        }
        // Log once per grip edge; do not auto-repeat while the grip is held.
        std::printf("OpenXR VR: Backpack gesture side=%.2f behind=%.2f height=%.2f head=%d result=%s\n",
            side, behind, height, headValid ? 1 : 0,
            g_xrVrBackpackInZone ? "TRIGGERED" : "OUTSIDE_ZONE");
    }
    g_xrVrBackpackGripWasDown = gripDown;

    // STEP 611: Left-shoulder spell-pouch gesture intentionally removed.
    // Normal left-grip (left shoulder) gamepad input remains available.

}

// The mouse button release is queued for Barony's NEXT SDL input pump.
// Report selection only after that pump, not immediately after SDL_PushEvent.
static bool g_xrVrPointerPostReleasePending = false;

// Cast the chosen controller's OpenXR aim ray onto the headset-anchored
// 1.2 m x 0.675 m GUI quad, centered one meter forward in VIEW space.
// Convert the ray position into Barony's existing real + virtual mouse paths.
// This makes menu items highlight on hover, not just draw a decorative cursor.
static void vrUpdateGuiLaserPointer(XrTime predictedDisplayTime)
{
    extern SDL_Window* screen;
    const VrControllerHandState& hand = g_xrControllerHands[g_xrVrPointerHand];
    const bool previousActive = g_xrVrPointerActive;
    g_xrVrPointerActive = false;
    if (g_xrVrPointerPostReleasePending)
    {
        if (clientnum >= 0 && clientnum < MAXPLAYERS && players[clientnum])
        {
            auto* interaction = inputs.getUIInteraction(clientnum);
            std::printf("OpenXR VR: STEP622 AFTER LEFT UP "
                        "itemSelected=%d gui_mode=%d\n",
                (interaction && interaction->selectedItem) ? 1 : 0,
                players[clientnum]->gui_mode);
        }
        g_xrVrPointerPostReleasePending = false;
    }

    // Only divert the index trigger when Barony is showing a mouse-driven UI.
    // During normal combat the trigger retains its original controller input.
    const bool uiVisible = vrMenuPointerMode();
    // Log overlay transitions so we can distinguish a ray miss from the
    // *game itself* leaving/entering a modal GUI (especially shrines).
    static bool previousUiVisible = false;
    if (uiVisible != previousUiVisible)
    {
        const bool hasPlayer = clientnum >= 0 && clientnum < MAXPLAYERS &&
            players[clientnum] != nullptr;
        std::printf("OpenXR VR: STEP622 UI %s intro=%d paused=%d "
                    "shootmode=%d gui_mode=%d overlay=%d\n",
            uiVisible ? "ENTER" : "EXIT", intro ? 1 : 0,
            gamePaused ? 1 : 0,
            hasPlayer ? (players[clientnum]->shootmode ? 1 : 0) : -1,
            hasPlayer ? players[clientnum]->gui_mode : -999,
            (hasPlayer && GenericGUI[clientnum].isGUIOpen()) ? 1 : 0);
        previousUiVisible = uiVisible;
    }
    if (!uiVisible)
    {
        vrResetPointer();
        return;
    }

    // STEP 621: SDL warps are not human motion. The Step620 automatic
    // mouse-idle timer repeatedly stole the laser when it observed our own
    // cursor motion. Ownership now changes ONLY on an explicit UI action.
    const bool vrHoldingButton = g_xrVrPointerMouseDown ||
        g_xrVrPointerRightMouseDown;
    const bool switchDown = hand.thumbstickClick &&
        !g_xrControllerHands[0].thumbstickClick;
    if (switchDown && !g_xrVrPointerSwitchWasDown && !vrHoldingButton)
    {
        g_xrVrPhysicalMouseOwns = !g_xrVrPhysicalMouseOwns;
        g_xrVrLastWarpValid = false;
        g_xrVrPointerSmoothValid = false;
        std::printf("OpenXR VR: STEP621 MENU INPUT=%s (right-stick click)\n",
            g_xrVrPhysicalMouseOwns ? "DESKTOP MOUSE" : "VR LASER");
    }
    g_xrVrPointerSwitchWasDown = switchDown;
    // STEP 624: Quest A is native Xbox Confirm in character creation.
    // Only the right trigger drives the SDL mouse there; no double click.
    const bool triggerRequested = hand.trigger >= 0.65f ||
        (hand.primaryButton && !vrCharacterCreationLobbyActive());
    if (g_xrVrPhysicalMouseOwns && triggerRequested &&
        !vrHoldingButton)
    {
        int x = 0, y = 0;
        const Uint32 desktopButtons = SDL_GetMouseState(&x, &y);
        if (!(desktopButtons & (SDL_BUTTON(SDL_BUTTON_LEFT) |
                                SDL_BUTTON(SDL_BUTTON_RIGHT))))
        {
            g_xrVrPhysicalMouseOwns = false;
            g_xrVrLastWarpValid = false;
            g_xrVrPointerSmoothValid = false;
            std::printf("OpenXR VR: STEP621 MENU INPUT=VR LASER (trigger)\n");
        }
    }
    const bool physicalOwns = g_xrVrPhysicalMouseOwns;
    g_xrVrPointerRayHit = false;

    const bool canPoint = !physicalOwns && uiVisible &&
        g_xrSessionState == XR_SESSION_STATE_FOCUSED &&
        g_xrControllerActionsAttached &&
        screen && g_xrGuiSwapchain != XR_NULL_HANDLE &&
        g_xrGuiViewSpace != XR_NULL_HANDLE;

    // STEP 627: Controller-only horizontal alignment calibration. Keep this
    // separate from mouse button ownership so existing drags are untouched.
    vrLoadMenuPointerOffset();
    const bool adjustingPointer = canPoint &&
        !g_xrVrPointerMouseDown && !g_xrVrPointerRightMouseDown &&
        g_xrControllerHands[0].squeeze >= 0.75f;
    if (adjustingPointer)
    {
        const float stickX = hand.thumbstick.x;
        const Uint32 tick = SDL_GetTicks();
        if (std::fabs(stickX) > 0.55f &&
            static_cast<Sint32>(tick - g_xrVrMenuPointerAdjustNextTick) >= 0)
        {
            const float step = std::fabs(stickX) > 0.90f ? 2.0f : 1.0f;
            const float next = g_xrVrMenuPointerOffsetX +
                (stickX > 0.f ? step : -step);
            const float clamped = next < -80.f ? -80.f :
                (next > 80.f ? 80.f : next);
            if (clamped != g_xrVrMenuPointerOffsetX)
            {
                g_xrVrMenuPointerOffsetX = clamped;
                g_xrVrMenuPointerOffsetDirty = true;
                std::printf("OpenXR VR: STEP627 POINTER ALIGN X=%.1f px\n",
                    g_xrVrMenuPointerOffsetX);
            }
            g_xrVrMenuPointerAdjustNextTick = tick + 85;
        }
    }
    if (g_xrVrMenuPointerAdjustWasHeld && !adjustingPointer)
        vrSaveMenuPointerOffset();
    g_xrVrMenuPointerAdjustWasHeld = adjustingPointer;

    // Keep the last cursor visible when the tracked ray briefly misses the
    // quad. That prevents the laser blinking off mid-menu; new clicks still
    // require a valid ray hit, but an existing drag is never dropped early.
    g_xrVrPointerActive = canPoint;
    // Native GUI widgets must interpret the VR laser as a mouse even though
    // the virtual Quest controller remains connected. In desktop-pointer
    // mode, return control to the real physical mouse instead.
    vrSetNativeMenuMouseMode(canPoint);
    if (canPoint && hand.aimPoseValid)
    {
        const XrSpace aimSpace = g_xrVrPointerHand == 0 ?
            g_xrLeftAimSpace : g_xrRightAimSpace;
        if (aimSpace != XR_NULL_HANDLE)
        {
            XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
            const XrResult res = xrLocateSpace(aimSpace, g_xrGuiViewSpace,
                predictedDisplayTime, &location);
            if (XR_SUCCEEDED(res) &&
                (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
            {
                const XrPosef& aim = location.pose;
                const XrQuaternionf& q = aim.orientation;
                // OpenXR aim-space's ray runs along local -Z.
                const float dx = -2.f * (q.x * q.z + q.y * q.w);
                const float dy = -2.f * (q.y * q.z - q.x * q.w);
                const float dz = -1.f + 2.f * (q.x * q.x + q.y * q.y);
                if (std::fabs(dz) > 0.001f)
                {
                    const float t = (g_xrGuiLayer.pose.position.z -
                        aim.position.z) / dz;
                    if (t > 0.02f && t < 6.0f)
                    {
                        const float x = aim.position.x + t * dx;
                        const float y = aim.position.y + t * dy;
                        const float halfWidth = g_xrGuiLayer.size.width * 0.5f;
                        const float halfHeight = g_xrGuiLayer.size.height * 0.5f;

                        // Aiming right at the thin edge was prone to switching
                        // the pointer off. Permit a small outside margin and
                        // clamp the displayed cursor to the actual panel.
                        // STEP 622: Keep the outer portions of menus reachable
                        // even if the ray grazes the panel boundary. During a
                        // held drag, keep the nearest valid 2D edge target.
                        const float marginX = 0.22f;
                        const float marginY = 0.15f;
                        // Do not cancel a held inventory drag at the panel
                        // boundary: clamp the target to the nearest edge.
                        // The UI, not OpenXR, decides what dropping there does.
                        if (g_xrVrPointerMouseDown ||
                            g_xrVrPointerRightMouseDown ||
                            (x >= -halfWidth - marginX &&
                             x <= halfWidth + marginX &&
                             y >= -halfHeight - marginY &&
                             y <= halfHeight + marginY))
                        {
                            const float uRaw = (x + halfWidth) /
                                g_xrGuiLayer.size.width;
                            const float vRaw = (halfHeight - y) /
                                g_xrGuiLayer.size.height;
                            // Offset is applied BEFORE deriving the GUI
                            // cursor, desktop warp, drawable coordinates and
                            // mouse press/release targets: they stay identical.
                            const float alignedU = uRaw +
                                g_xrVrMenuPointerOffsetX / 1280.f;
                            const float rawU = alignedU < 0.f ? 0.f :
                                (alignedU > 1.f ? 1.f : alignedU);
                            const float rawV = vRaw < 0.f ? 0.f :
                                (vRaw > 1.f ? 1.f : vRaw);
                            // STEP 626: Use the CURRENT controller-ray hit
                            // for both the visible cursor and the actual
                            // Barony mouse event. The previous 0.45 smoothing
                            // trailed the ray by several pixels during hand
                            // movement, making small menu targets inaccurate.
                            // Do not reintroduce lag while dragging items.
                            g_xrVrPointerSmoothU = rawU;
                            g_xrVrPointerSmoothV = rawV;
                            g_xrVrPointerSmoothValid = true;
                            const float u = rawU;
                            const float v = rawV;
                            g_xrVrPointerGuiX = static_cast<int>(u * 1279.f + 0.5f);
                            g_xrVrPointerGuiY = static_cast<int>(v * 719.f + 0.5f);

                            int w = 0, h = 0;
                            SDL_GetWindowSize(screen, &w, &h);
                            if (w > 0 && h > 0)
                            {
                                g_xrVrPointerWindowX = static_cast<int>(u * (w - 1));
                                g_xrVrPointerWindowY = static_cast<int>(v * (h - 1));

                                // Warp only when needed, not 90 times a
                                // second at the same coordinates. This is the
                                // SINGLE source of mouse motion for Barony UI.
                                if (!g_xrVrLastWarpValid ||
                                    g_xrVrLastWarpX != g_xrVrPointerWindowX ||
                                    g_xrVrLastWarpY != g_xrVrPointerWindowY)
                                {
                                    SDL_WarpMouseInWindow(screen,
                                        g_xrVrPointerWindowX,
                                        g_xrVrPointerWindowY);
                                    g_xrVrLastWarpX = g_xrVrPointerWindowX;
                                    g_xrVrLastWarpY = g_xrVrPointerWindowY;
                                    g_xrVrLastWarpValid = true;
                                    // In Step621, warps do not affect ownership.
                                }

                                // Barony may use either its ordinary mouse or
                                // its controller virtual cursor depending on
                                // configured input ownership. Update ONLY
                                // current coordinates here. NEVER call
                                // inputs.warpMouse(): that function resets
                                // original mouse-down coordinates (ox/oy),
                                // which BREAKS drag/drop and slider widgets.
                                if (clientnum >= 0 && clientnum < MAXPLAYERS)
                                {
                                    int drawableW = w, drawableH = h;
                                    SDL_GL_GetDrawableSize(screen,
                                        &drawableW, &drawableH);
                                    if (drawableW > 0 && drawableH > 0)
                                    {
                                        const int px = static_cast<int>(
                                            u * (drawableW - 1));
                                        const int py = static_cast<int>(
                                            v * (drawableH - 1));
                                        inputs.setMouse(clientnum, Inputs::MouseInputs::X, px);
                                        inputs.setMouse(clientnum, Inputs::MouseInputs::Y, py);
                                        if (auto* vm = inputs.getVirtualMouse(clientnum))
                                        {
                                            vm->lastMovementFromController = false;
                                            vm->moved = true;
                                            vm->x = px;
                                            vm->y = py;
                                            vm->floatx = static_cast<real_t>(px);
                                            vm->floaty = static_cast<real_t>(py);
                                        }
                                    }
                                }
                                g_xrVrPointerRayHit = true;
                            }
                        }
                    }
                }
            }
        }
    }

    if (!g_xrVrPointerActive &&
        (g_xrVrPointerMouseDown || g_xrVrPointerRightMouseDown) &&
        !physicalOwns && canPoint)
    {
        // Tracking briefly lost while dragging: retain the most recent
        // cursor so mouse-up is delivered cleanly when the trigger releases.
        g_xrVrPointerActive = true;
    }
    if (!g_xrVrPointerActive)
        g_xrVrPointerSmoothValid = false;
    if (g_xrVrPointerActive != previousActive)
        std::printf("OpenXR VR: RIGHT_UI_LASER %s\n",
            g_xrVrPointerActive ? "ON" : "OFF");

    // STEP 620: Trigger or A is a REAL HELD mouse-left action. Do not release
    // it while a hand is moving over inventory/hotbar/chest or outside the
    // panel margin. Left-click must be a drag, not an artificial short pulse.
    // Right grip is a separate held right-click for context actions.
    // Native controller A/B are disabled in UI to avoid duplicate actions.
    const bool triggerDown = hand.trigger >= 0.65f ||
        (hand.primaryButton && !vrCharacterCreationLobbyActive());
    // STEP 625: The shoulder close gesture must never double as a UI
    // right-click, including on the same frame that a shrine is dismissed.
    const bool gripDown = hand.squeeze >= 0.65f &&
        !g_xrVrBackpackInZone && !g_xrVrBackpackGripConsumed;
    if (!triggerDown)
        g_xrVrPointerTriggerConsumed = false;
    if (g_xrVrPointerMouseDown && (!g_xrVrPointerActive || !triggerDown))
    {
        vrSendPointerMouseClick(SDL_BUTTON_LEFT, false);
        g_xrVrPointerMouseDown = false;
        g_xrVrPointerPostReleasePending = true;
        if (clientnum >= 0 && clientnum < MAXPLAYERS && players[clientnum])
        {
            auto* interaction = inputs.getUIInteraction(clientnum);
            std::printf("OpenXR VR: STEP622 LEFT UP QUEUED gui=(%d,%d) "
                        "preEventItemSelected=%d gui_mode=%d overlay=%d ray=%d\n",
                g_xrVrPointerGuiX, g_xrVrPointerGuiY,
                (interaction && interaction->selectedItem) ? 1 : 0,
                players[clientnum]->gui_mode,
                GenericGUI[clientnum].isGUIOpen() ? 1 : 0,
                g_xrVrPointerRayHit ? 1 : 0);
        }
        else
            std::printf("OpenXR VR: STEP622 LEFT UP QUEUED gui=(%d,%d)\n",
                g_xrVrPointerGuiX, g_xrVrPointerGuiY);
    }
    if (g_xrVrPointerActive && g_xrVrPointerRayHit &&
        triggerDown && !g_xrVrPointerMouseDown)
    {
        // Set BOTH possible UI input paths' original press coordinates
        // exactly once. Barony's normal mouse-up handling will release the
        // origin lock; never touch ox/oy while the trigger is held.
        if (clientnum >= 0 && clientnum < MAXPLAYERS)
        {
            const int px = inputs.getMouse(clientnum, Inputs::MouseInputs::X);
            const int py = inputs.getMouse(clientnum, Inputs::MouseInputs::Y);
            inputs.setMouse(clientnum, Inputs::MouseInputs::OX, px);
            inputs.setMouse(clientnum, Inputs::MouseInputs::OY, py);
            if (auto* vm = inputs.getVirtualMouse(clientnum))
            {
                vm->ox = vm->x;
                vm->oy = vm->y;
                vm->floatox = vm->floatx;
                vm->floatoy = vm->floaty;
            }
        }
        vrSendPointerMouseClick(SDL_BUTTON_LEFT, true);
        g_xrVrPointerMouseDown = true;
        g_xrVrPointerTriggerConsumed = true;
        std::printf("OpenXR VR: STEP622 LEFT DOWN gui=(%d,%d)\n",
            g_xrVrPointerGuiX, g_xrVrPointerGuiY);
    }
    if (g_xrVrPointerRightMouseDown && (!g_xrVrPointerActive || !gripDown))
    {
        vrSendPointerMouseClick(SDL_BUTTON_RIGHT, false);
        g_xrVrPointerRightMouseDown = false;
        std::printf("OpenXR VR: UI MOUSE RIGHT UP\n");
    }
    if (g_xrVrPointerActive && g_xrVrPointerRayHit &&
        gripDown && !g_xrVrPointerRightMouseDown)
    {
        vrSendPointerMouseClick(SDL_BUTTON_RIGHT, true);
        g_xrVrPointerRightMouseDown = true;
        std::printf("OpenXR VR: UI MOUSE RIGHT DOWN at (%d,%d)\n",
            g_xrVrPointerGuiX, g_xrVrPointerGuiY);
    }
    if (uiVisible && triggerDown && !g_xrVrPointerTriggerWasDown &&
        !g_xrVrPointerRayHit)
        std::printf("OpenXR VR: STEP621 UI POINTER MISS (ray not on menu panel)\n");
    g_xrVrPointerTriggerWasDown = triggerDown;
}
bool vrSuppressGripQuickTurn(int playerIndex)
{
    if (playerIndex < 0 || playerIndex >= MAXPLAYERS || playerIndex != clientnum || !g_xrVirtualSDLJoystick ||
        g_xrSession == XR_NULL_HANDLE) return false;
    const auto binding=Input::inputs[playerIndex].input("Quick Turn");
    if (binding.type != Input::binding_t::CONTROLLER_BUTTON ||
        binding.padButton != SDL_CONTROLLER_BUTTON_RIGHTSHOULDER || !binding.pad) return false;
    SDL_Joystick* const bound=SDL_GameControllerGetJoystick(binding.pad);
    return bound && SDL_JoystickInstanceID(bound)==SDL_JoystickInstanceID(g_xrVirtualSDLJoystick);
}

// Four cardinal D-pad directions with separate engage/release thresholds.
// 0 neutral, 1 up, 2 right, 3 down, 4 left. No synthetic key repeats:
// Barony receives held SDL buttons and owns action/repeat behavior.
static int vrDpadDirection(bool active, float x, float y, int previous)
{
    if (!active || !std::isfinite(x) || !std::isfinite(y)) return 0;
    const float ax=std::fabs(x), ay=std::fabs(y);
    if (std::max(ax,ay)>=.65f)
        return ax>ay ? (x>0.f?2:4) : (y>0.f?1:3);
    switch(previous) {
        case 1: return y>.40f?1:0;
        case 2: return x>.40f?2:0;
        case 3: return y<-.40f?3:0;
        case 4: return x<-.40f?4:0;
        default: return 0;
    }
}

static void vrUpdateVirtualSDLGamepad(bool inputsValid = true)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (!g_xrVirtualSDLJoystick)
        return;

    // Release every control if SteamVR stops focusing Barony, or if sync fails.
    const bool useInput = inputsValid &&
        g_xrSessionState == XR_SESSION_STATE_FOCUSED &&
        g_xrControllerActionsAttached;
    const VrControllerHandState neutral{};
    const VrControllerHandState& left = useInput ? g_xrControllerHands[0] : neutral;
    const VrControllerHandState& right = useInput ? g_xrControllerHands[1] : neutral;
    const bool dpadMode=useInput && !g_vrArmCalibrationActive &&
        right.thumbrestAvailable && right.thumbrestTouch;
    static int dpadDirection=0;
    const int previousDirection=dpadDirection;
    dpadDirection=vrDpadDirection(dpadMode,left.thumbstick.x,left.thumbstick.y,dpadDirection);
    static bool previousDpadMode=false;
    if (previousDpadMode!=dpadMode || previousDirection!=dpadDirection) {
        printlog("[VR GA21] dpad mode=%d direction=%d",dpadMode?1:0,dpadDirection);
        previousDpadMode=dpadMode;
    }
    static int bowInputHand = -1;
    static bool bowInputArmed = false;
    const bool bowControls = useInput && vrBowGameplayActive() && !vrMenuPointerMode();
    const auto& bowDrawControl = g_vrBowDrawHand == 1 ? right : left;
    const auto& bowHoldControl = g_vrBowDrawHand == 1 ? left : right;
    if (!bowControls || !bowDrawControl.gripPoseValid || !bowHoldControl.gripPoseValid ||
        (g_vrBowDrawHand == 1 && (g_xrVrBackpackInZone || g_xrVrBackpackGripConsumed))) {
        bowInputHand = -1; bowInputArmed = false;
    } else if (bowInputHand != g_vrBowDrawHand) {
        bowInputHand = g_vrBowDrawHand; bowInputArmed = false;
    } else if (bowDrawControl.trigger < .2f && bowHoldControl.trigger < .2f &&
        bowDrawControl.squeeze < .2f) {
        bowInputArmed = true;
    }
    const bool bowHotbarSelection = bowControls && vrBowHotbarSelecting();
    const bool physicalBowControls = bowControls && vrPhysicalBowOwnsAttack(clientnum) && !bowHotbarSelection;
    static bool bowSpellTriggerArmed = false;
    if (!physicalBowControls || !bowInputArmed || g_vrBowPreview.drawing ||
        g_vrBowRelease.pending || bowDrawControl.squeeze >= .35f) {
        bowSpellTriggerArmed = false;
    } else if (bowDrawControl.trigger < .2f) {
        bowSpellTriggerArmed = true;
    }
    const bool bowSpellPressed = physicalBowControls && bowInputArmed &&
        bowSpellTriggerArmed && bowDrawControl.trigger >= .65f;
    if (!useInput)
    {
        g_xrVrMenuChordLatched = false;
        g_xrVrQuickTurnStickDownHeld = false;
        g_xrVrMenuChordActive = false;
        g_xrRoomHavePreviousPose = false;
        g_xrRoomStickX = g_xrRoomStickY = 0.0f;
        g_xrPhysicalSneaking = false;
        g_xrVrBackpackGripWasDown = false;
        g_xrVrBackpackGripConsumed = false;
        g_xrVrBackpackInZone = false;
        g_xrVrUiXWasDown = false;
        g_xrVrUiYWasDown = false;
        vrResetPointer();
        g_xrVrMenuPulseUntilMs = 0;
        g_xrVrBackpackPulseUntilMs = 0;
    }

    auto stickAxis = [](float v, bool invert) -> Sint16
        {
            if (invert) v = -v; // SDL uses positive Y = down; OpenXR uses up.
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            if (v > -0.15f && v < 0.15f) v = 0.0f;
            return static_cast<Sint16>(std::lround(v * 32767.0f));
        };
    auto triggerAxis = [](float v) -> Sint16
        {
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            // SDL2 virtual joystick triggers must range from -32768 (released)
            // through +32767 (fully pressed), not from 0 to +32767.
            return static_cast<Sint16>(-32768 + std::lround(v * 65535.0f));
        };
    auto setAxis = [](SDL_Joystick* joy, SDL_GameControllerAxis axis, Sint16 value)
        {
            SDL_JoystickSetVirtualAxis(joy, static_cast<int>(axis), value);
        };
    auto setButton = [](SDL_Joystick* joy, SDL_GameControllerButton button, bool down)
        {
            SDL_JoystickSetVirtualButton(joy, static_cast<int>(button),
                down ? SDL_PRESSED : SDL_RELEASED);
        };

    auto applyDpad = [&]() {
        setButton(g_xrVirtualSDLJoystick,SDL_CONTROLLER_BUTTON_DPAD_UP,dpadDirection==1);
        setButton(g_xrVirtualSDLJoystick,SDL_CONTROLLER_BUTTON_DPAD_RIGHT,dpadDirection==2);
        setButton(g_xrVirtualSDLJoystick,SDL_CONTROLLER_BUTTON_DPAD_DOWN,dpadDirection==3);
        setButton(g_xrVirtualSDLJoystick,SDL_CONTROLLER_BUTTON_DPAD_LEFT,dpadDirection==4);
        if(dpadMode && vrMenuPointerMode()) {
            // While touch explicitly selects native UI navigation, expose
            // confirm/cancel and page bumpers instead of mouse clicks.
            setButton(g_xrVirtualSDLJoystick,SDL_CONTROLLER_BUTTON_A,right.primaryButton);
            setButton(g_xrVirtualSDLJoystick,SDL_CONTROLLER_BUTTON_B,right.secondaryButton);
            setButton(g_xrVirtualSDLJoystick,SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
                left.squeeze>=.5f && !g_vrMeleeExitGripsSuppressed);
            setButton(g_xrVirtualSDLJoystick,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
                right.squeeze>=.5f && !g_vrMeleeExitGripsSuppressed &&
                !g_xrVrBackpackInZone && !g_xrVrBackpackGripConsumed);
        }
    };

    // STEP 605: Neutralize all gameplay input while editing the model.
    if (g_vrArmCalibrationActive && useInput)
    {
        for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; ++i)
            SDL_JoystickSetVirtualAxis(g_xrVirtualSDLJoystick, i,
                (i == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
                 i == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) ? -32768 : 0);
        for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; ++i)
            SDL_JoystickSetVirtualButton(g_xrVirtualSDLJoystick, i, SDL_RELEASED);
        applyDpad();
        SDL_JoystickUpdate();
        return;
    }

    // STEP 614: One input owner per UI. The Quest controllers emulate the
    // ordinary SDL mouse/keyboard above, NEVER both mouse clicks and native
    // gamepad confirm/navigation on the same frame. Barony still receives
    // physical mouse/keyboard events normally. In the dungeon, continue with
    // the existing gamepad + 6DoF combat path below, unchanged.
    if (useInput && vrMenuPointerMode())
    {
        // STEP 624: Restore native Xbox-style focus/confirm ONLY in Barony's
        // character creation lobby. Shrines, inventory, chests and every other
        // pointer-driven UI retain Step623's neutral virtual gamepad behavior.
        if (vrCharacterCreationLobbyActive())
        {
            vrBindVirtualControllerInLobby();
            for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; ++i)
                SDL_JoystickSetVirtualAxis(g_xrVirtualSDLJoystick, i,
                    (i == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
                     i == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) ? -32768 : 0);
            SDL_JoystickSetVirtualAxis(g_xrVirtualSDLJoystick,
                SDL_CONTROLLER_AXIS_LEFTX, dpadMode ? 0 : stickAxis(left.thumbstick.x, false));
            SDL_JoystickSetVirtualAxis(g_xrVirtualSDLJoystick,
                SDL_CONTROLLER_AXIS_LEFTY, dpadMode ? 0 : stickAxis(left.thumbstick.y, true));
            for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; ++i)
                SDL_JoystickSetVirtualButton(g_xrVirtualSDLJoystick, i, SDL_RELEASED);
            SDL_JoystickSetVirtualButton(g_xrVirtualSDLJoystick,
                SDL_CONTROLLER_BUTTON_A, right.primaryButton ? SDL_PRESSED : SDL_RELEASED);
            applyDpad();
            SDL_JoystickUpdate();
            return;
        }
        for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; ++i)
            SDL_JoystickSetVirtualAxis(g_xrVirtualSDLJoystick, i,
                (i == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
                 i == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) ? -32768 : 0);
        for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; ++i)
            SDL_JoystickSetVirtualButton(g_xrVirtualSDLJoystick, i, SDL_RELEASED);
        // STEP638FW: Native follower/callout wheels remain real Barony
        // gamepad radial menus even when shown over the VR floating GUI.
        // FollowerRadialMenu/CalloutRadialMenu confirm with MenuConfirm
        // (controller A); they intentionally reject MenuLeftClick when a
        // gamepad is attached. Neutralizing A here leaves a highlighted
        // command impossible to choose. Restore ONLY A while a wheel is
        // actually open. Keep all analog axes and other buttons neutral, so
        // pointer-based menus, attacks and calibration remain unchanged.
        // Exclude the later world-targeting phase of Move To/Attack.
        const bool wheelOpen = !intro && !gamePaused &&
            clientnum >= 0 && clientnum < MAXPLAYERS &&
            players[clientnum] &&
            ((FollowerMenu[clientnum].followerToCommand &&
              !FollowerMenu[clientnum].selectMoveTo) ||
             (CalloutMenu[clientnum].bOpen &&
              !CalloutMenu[clientnum].selectMoveTo));
        if (wheelOpen)
        {
            SDL_JoystickSetVirtualButton(g_xrVirtualSDLJoystick,
                SDL_CONTROLLER_BUTTON_A,
                right.primaryButton ? SDL_PRESSED : SDL_RELEASED);
        }
        applyDpad();
        SDL_JoystickUpdate();
        return;
    }

    // Blend analog locomotion with the optional Standing room-scale input.
    // Both pass through Barony's normal movement/collision path.
    auto limitStick = [](float v) { return v < -1.f ? -1.f : (v > 1.f ? 1.f : v); };
    const bool locomotionActive = useInput &&
        g_xrPlayMode == VrPlayMode::Standing && !vrMenuPointerMode();
    setAxis(g_xrVirtualSDLJoystick, SDL_CONTROLLER_AXIS_LEFTX,
        dpadMode ? 0 : stickAxis(limitStick(left.thumbstick.x +
            (locomotionActive ? g_xrRoomStickX : 0.0f)), false));
    setAxis(g_xrVirtualSDLJoystick, SDL_CONTROLLER_AXIS_LEFTY,
        dpadMode ? 0 : stickAxis(limitStick(left.thumbstick.y +
            (locomotionActive ? g_xrRoomStickY : 0.0f)), true));
    // While pointing into a GUI, the right hand aims the cursor, and its
    // thumbstick should not drag the game's conventional menu cursor.
    setAxis(g_xrVirtualSDLJoystick, SDL_CONTROLLER_AXIS_RIGHTX,
        vrMenuPointerMode() ? 0 : stickAxis(right.thumbstick.x, false));
    // VR comfort: disable gamepad look-up/look-down in both seated and standing
    // play. Headset pitch controls vertical view. Keep right-stick X for yaw.
    // UI vertical navigation is still available on the LEFT thumbstick.
    setAxis(g_xrVirtualSDLJoystick, SDL_CONTROLLER_AXIS_RIGHTY, 0);
    // Barony's standard offhand trigger is Sneak *only* when the offhand is
    // empty. Auto-sneak must not raise a shield or cast/use an offhand item.
    const float leftSneakTrigger = (useInput && g_xrPhysicalSneaking) ?
        1.0f : (bowHotbarSelection ? left.trigger :
        (bowControls ? (bowInputArmed ? bowHoldControl.trigger : 0.f) : left.trigger));
    setAxis(g_xrVirtualSDLJoystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT,
        triggerAxis(leftSneakTrigger));
    // STEP 638DG: Empty-hand punches use the same physical-swing path as
    // melee weapons. Suppress the virtual trigger for BOTH cases so simply
    // holding trigger to charge cannot also create unintended vanilla punches.
    // Non-melee items, UI, and CLIENT mode retain the original trigger path.
    const bool physicalMeleeOwnsAttack = !intro && !gamePaused &&
        multiplayer != CLIENT && clientnum >= 0 && clientnum < MAXPLAYERS &&
        players[clientnum] && players[clientnum]->shootmode &&
        stats[clientnum] &&
        ((!stats[clientnum]->weapon &&
          !cast_animation[clientnum].spellWaitingAttackInput()) ||
         (stats[clientnum]->weapon &&
          (isMeleeWeapon(*stats[clientnum]->weapon) ||
           stats[clientnum]->weapon->type == TOOL_PICKAXE ||
           stats[clientnum]->weapon->type == TOOL_WHIP)));
    setAxis(g_xrVirtualSDLJoystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
        (vrMenuPointerMode() || g_xrVrPointerActive ||
         g_xrVrPointerTriggerConsumed ||
         physicalMeleeOwnsAttack || physicalBowControls) ?
        triggerAxis(0.f) : triggerAxis(bowHotbarSelection ? right.trigger :
            (bowControls ? (bowInputArmed ? bowDrawControl.trigger : 0.f) : right.trigger)));

    // Standard gamepad compatibility mapping; future physical VR combat
    // will use the original 6DoF poses instead of these conventional buttons.
    // Prevent short X+Y setup chord from triggering Barony input actions.
    const bool armCalChord = left.primaryButton && left.secondaryButton;
    const bool inGameInventory = useInput && !intro && !gamePaused &&
        clientnum >= 0 && clientnum < MAXPLAYERS && players[clientnum] &&
        !players[clientnum]->shootmode &&
        players[clientnum]->gui_mode == GUI_MODE_INVENTORY;
    // X/Y have explicit Character/Spells tab actions in the VR inventory.
    // Don't forward a second native X/Y event into the same menu.
    setButton(g_xrVirtualSDLJoystick, SDL_CONTROLLER_BUTTON_X,
        left.primaryButton && !armCalChord && !inGameInventory);
    setButton(g_xrVirtualSDLJoystick, SDL_CONTROLLER_BUTTON_Y,
        left.secondaryButton && !armCalChord && !inGameInventory);
    // Outside menus keep conventional controller A; mouse mode above does not
    // generate Xbox Confirm from the laser at all.
    setButton(g_xrVirtualSDLJoystick, SDL_CONTROLLER_BUTTON_A,
        right.primaryButton);
    setButton(g_xrVirtualSDLJoystick, SDL_CONTROLLER_BUTTON_B, right.secondaryButton);
    // STEP 611: No more left-shoulder spell pouch. Keep the ordinary
    // left bumper/grip binding unchanged in regular gameplay and native UI.
    setButton(g_xrVirtualSDLJoystick, SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        (bowHotbarSelection ? left.squeeze >= .5f :
            (physicalBowControls ? bowSpellPressed :
             (bowControls ? (bowInputArmed ? bowDrawControl.squeeze : 0.f) : left.squeeze) >= .5f)) &&
        !g_vrMeleeExitGripsSuppressed &&
        !(bowControls && g_vrBowDrawHand == 1 &&
          (g_xrVrBackpackInZone || g_xrVrBackpackGripConsumed)));
    // Restore the native RB actions except while the right grip is owned by
    // the bow string/backpack/calibration. Quick Turn is filtered separately.
    const bool bowOwnsRightGrip=vrPhysicalBowOwnsAttack(clientnum) &&
        g_vrBowDrawHand==1 && (g_vrBowPreview.drawing || g_vrBowRelease.pending);
    setButton(g_xrVirtualSDLJoystick, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        right.squeeze >= 0.5f && !bowOwnsRightGrip && !vrMenuPointerMode() &&
        !g_vrMeleeExitGripsSuppressed &&
        !g_xrVrBackpackInZone && !g_xrVrBackpackGripConsumed);
    // STEP638GA3: Push the RIGHT analog stick DOWN for vanilla Quick Turn.
    // OpenXR thumbstick Y is negative when down. The right stick's normal
    // Y/look axis stays zero (headset owns pitch), and right-stick CLICK is
    // still reserved for the two-stick menu chord / calibration controls.
    // Only emulate the existing gamepad Quick Turn button in dungeon play:
    // no turning while an inventory, dialogue, chest or other UI is active.
    const bool quickTurnAllowed = useInput && !intro && !gamePaused &&
        !g_vrArmCalibrationActive && !vrMenuPointerMode() &&
        clientnum >= 0 && clientnum < MAXPLAYERS && players[clientnum] &&
        players[clientnum]->shootmode;
    // STEP638GA5: The GA3 simulated stick-click depended on the user's
    // customizable gamepad binding. Start the native 180-degree animation
    // directly on the DOWN edge instead. This does not teleport, change
    // network ownership, or apply repeated rotation while held.
    const bool wasDown = g_xrVrQuickTurnStickDownHeld;
    if (!quickTurnAllowed)
        g_xrVrQuickTurnStickDownHeld = false;
    else if (right.thumbstick.y <= -0.78f)
        g_xrVrQuickTurnStickDownHeld = true;
    else if (right.thumbstick.y >= -0.42f)
        g_xrVrQuickTurnStickDownHeld = false;
    if (quickTurnAllowed && g_xrVrQuickTurnStickDownHeld && !wasDown)
    {
        if (players[clientnum]->ghost.isActive())
            players[clientnum]->ghost.startQuickTurn();
        else
            players[clientnum]->movement.startQuickTurn();
        std::printf("OpenXR VR GA5: right-stick-down direct Quick Turn fired\n");
    }

    setButton(g_xrVirtualSDLJoystick, SDL_CONTROLLER_BUTTON_LEFTSTICK, false);
    // Keep physical stick-click reserved for menu chords/calibration.
    setButton(g_xrVirtualSDLJoystick, SDL_CONTROLLER_BUTTON_RIGHTSTICK, false);
    setButton(g_xrVirtualSDLJoystick, SDL_CONTROLLER_BUTTON_START,
        left.menuButton || (useInput && vrShortcutPulseActive(g_xrVrMenuPulseUntilMs)));
    setButton(g_xrVirtualSDLJoystick, SDL_CONTROLLER_BUTTON_BACK,
        useInput && vrShortcutPulseActive(g_xrVrBackpackPulseUntilMs));

    // Apply the virtual joystick state before Barony's next event/input poll.
    applyDpad();
    SDL_JoystickUpdate();
#else
    (void)inputsValid;
#endif
}

static void vrAttachVirtualSDLGamepad()
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    vrLoadModeSettings();
    if (!g_xrEnableVirtualSDLGamepad || !g_xrControllerActionsAttached ||
        g_xrVirtualSDLJoystick)
        return;

    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0 &&
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
    {
        std::printf("OpenXR SDL: Could not initialize gamecontroller subsystem: %s\n",
            SDL_GetError());
        return;
    }

    const int deviceIndex = SDL_JoystickAttachVirtual(
        SDL_JOYSTICK_TYPE_GAMECONTROLLER,
        SDL_CONTROLLER_AXIS_MAX,
        SDL_CONTROLLER_BUTTON_MAX,
        0);
    if (deviceIndex < 0)
    {
        std::printf("OpenXR SDL: Virtual gamepad attach FAILED: %s\n", SDL_GetError());
        return;
    }

    g_xrVirtualSDLDeviceIndex = deviceIndex;
    g_xrVirtualSDLJoystick = SDL_JoystickOpen(deviceIndex);
    if (!g_xrVirtualSDLJoystick)
    {
        std::printf("OpenXR SDL: Virtual gamepad open FAILED: %s\n", SDL_GetError());
        SDL_JoystickDetachVirtual(deviceIndex);
        g_xrVirtualSDLDeviceIndex = -1;
        return;
    }

    // Give the virtual gamepad neutral values immediately, especially triggers.
    vrUpdateVirtualSDLGamepad(false);
    std::printf("OpenXR SDL: Virtual gamepad attached at device index %d. SDL game controller=%s\n",
        deviceIndex, SDL_IsGameController(deviceIndex) ? "YES" : "NO");
#else
    std::printf("OpenXR SDL: SDL 2.0.14 or newer is required for virtual controllers.\n");
#endif
}

bool vrGetOpenGLHandles(SDL_Window* window, HDC& hdc, HGLRC& hglrc)
{
    if (!window) {
        std::printf("OpenXR: SDL window is null.\n");
        return false;
    }

    SDL_SysWMinfo windowInfo{};
    SDL_VERSION(&windowInfo.version);

    if (SDL_GetWindowWMInfo(window, &windowInfo) != SDL_TRUE) {
        std::printf("OpenXR: Could not retrieve Windows window handle.\n");
        return false;
    }

    HWND hwnd = windowInfo.info.win.window;

    hdc = wglGetCurrentDC();
    hglrc = wglGetCurrentContext();

    if (!hdc || !hglrc) {
        std::printf("OpenXR: Could not retrieve OpenGL handles.\n");
        return false;
    }


    std::printf("OpenXR: Windows OpenGL handles retrieved successfully.\n");
    return true;
}
void vrTestOpenGLHandles()
{
    extern SDL_Window* screen;

    HDC hdc = nullptr;
    HGLRC hglrc = nullptr;

    if (vrGetOpenGLHandles(screen, hdc, hglrc))
    {
        // Load the OpenGL function for creating framebuffer objects.
        g_glGenFramebuffers = reinterpret_cast<PFNGLGENFRAMEBUFFERSPROC>(
            SDL_GL_GetProcAddress("glGenFramebuffers")
            );
        // Load the OpenGL function for binding framebuffer objects.
        g_glBindFramebuffer = reinterpret_cast<PFNGLBINDFRAMEBUFFERPROC>(
            SDL_GL_GetProcAddress("glBindFramebuffer")
            );
        // Load the OpenGL function for attaching stereo texture layers.
        g_glFramebufferTextureLayer =
            reinterpret_cast<PFNGLFRAMEBUFFERTEXTURELAYERPROC>(
                SDL_GL_GetProcAddress("glFramebufferTextureLayer")
                );
        // Load the OpenGL function for checking framebuffer completeness.
        g_glCheckFramebufferStatus =
            reinterpret_cast<PFNGLCHECKFRAMEBUFFERSTATUSPROC>(
                SDL_GL_GetProcAddress("glCheckFramebufferStatus")
                );
        // Load the OpenGL function for creating renderbuffers.
        g_glGenRenderbuffers =
            reinterpret_cast<PFNGLGENRENDERBUFFERSPROC>(
                SDL_GL_GetProcAddress("glGenRenderbuffers")
                );

        // Load the OpenGL function for binding renderbuffers.
        g_glBindRenderbuffer =
            reinterpret_cast<PFNGLBINDRENDERBUFFERPROC>(
                SDL_GL_GetProcAddress("glBindRenderbuffer")
                );

        // Load the OpenGL function for allocating renderbuffer storage.
        g_glRenderbufferStorage =
            reinterpret_cast<PFNGLRENDERBUFFERSTORAGEPROC>(
                SDL_GL_GetProcAddress("glRenderbufferStorage")
                );

        // Load the OpenGL function for attaching renderbuffers.
        g_glFramebufferRenderbuffer =
            reinterpret_cast<PFNGLFRAMEBUFFERRENDERBUFFERPROC>(
                SDL_GL_GetProcAddress("glFramebufferRenderbuffer")
                );
        // Verify that all VR depth-buffer functions are available.
        if (g_glGenRenderbuffers &&
            g_glBindRenderbuffer &&
            g_glRenderbufferStorage &&
            g_glFramebufferRenderbuffer)
        {
            std::printf("OpenXR: All depth-buffer functions loaded successfully!\n");
        }
        else
        {
            std::printf("OpenXR: ERROR - One or more depth-buffer functions failed to load!\n");
        }
        if (g_glCheckFramebufferStatus)
        {
            std::printf("OpenXR: OpenGL framebuffer status function loaded!\n");
        }
        else
        {
            std::printf("OpenXR: Failed to load glCheckFramebufferStatus!\n");
        }

        if (g_glFramebufferTextureLayer)
        {
            std::printf("OpenXR: OpenGL texture-layer attachment function loaded!\n");
        }
        else
        {
            std::printf("OpenXR: Failed to load glFramebufferTextureLayer!\n");
        }
        if (g_glBindFramebuffer)
        {
            std::printf("OpenXR: OpenGL framebuffer binding function loaded!\n");
        }
        else
        {
            std::printf("OpenXR: Failed to load glBindFramebuffer!\n");
        }
        if (g_glGenFramebuffers)
        {
            std::printf("OpenXR: OpenGL framebuffer creation function loaded!\n");

            // Create one framebuffer for future VR eye rendering.
            g_glGenFramebuffers(1, &g_xrFramebuffer);
            // Create a dedicated framebuffer for the VR GUI.
            if (g_xrGuiFramebuffer == 0)
            {
                g_glGenFramebuffers(1, &g_xrGuiFramebuffer);

                if (g_xrGuiFramebuffer != 0)
                {
                    SDL_Log(
                        "OpenXR: GUI framebuffer created! ID = %u",
                        g_xrGuiFramebuffer
                    );
                }
                else
                {
                    SDL_Log("OpenXR: Failed to create GUI framebuffer!");
                }
            }

            if (g_xrFramebuffer != 0)
            {
                std::printf(
                    "OpenXR: VR framebuffer created successfully! ID = %u\n",
                    g_xrFramebuffer
                );
            }
            else
            {
                std::printf("OpenXR: Failed to create VR framebuffer!\n");
            }
        }
        else
        {
            std::printf("OpenXR: Failed to load glGenFramebuffers!\n");
        }
        std::printf("OpenXR: HDC and HGLRC are ready for session creation.\n");

        XrGraphicsBindingOpenGLWin32KHR graphicsBinding{};
        graphicsBinding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR;
        graphicsBinding.next = nullptr;
        graphicsBinding.hDC = hdc;
        graphicsBinding.hGLRC = hglrc;

        if (g_xrInstance != XR_NULL_HANDLE &&
            g_xrSystemId != XR_NULL_SYSTEM_ID &&
            g_xrSession == XR_NULL_HANDLE)
        {
            XrSessionCreateInfo sessionInfo{};
            sessionInfo.type = XR_TYPE_SESSION_CREATE_INFO;
            sessionInfo.next = &graphicsBinding;
            sessionInfo.systemId = g_xrSystemId;

            XrResult result = xrCreateSession(
                g_xrInstance,
                &sessionInfo,
                &g_xrSession
            );

            if (XR_SUCCEEDED(result))
            {
                std::printf("OpenXR: VR session created successfully!\n");
                // Attach only after every controller action and profile binding exists.
                if (g_xrControllerActionSet != XR_NULL_HANDLE &&
                    g_xrControllerBindingsSuggested)
                {
                    XrSessionActionSetsAttachInfo attachInfo{};
                    attachInfo.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
                    attachInfo.countActionSets = 1;
                    attachInfo.actionSets = &g_xrControllerActionSet;
                    const XrResult attachResult = xrAttachSessionActionSets(
                        g_xrSession, &attachInfo);
                    g_xrControllerActionsAttached = XR_SUCCEEDED(attachResult);
                    std::printf("OpenXR: Controller action-set attach %s (%d)\n",
                        g_xrControllerActionsAttached ? "SUCCEEDED" : "FAILED",
                        static_cast<int>(attachResult));
                    // Expose conventional Quest controls through Barony's SDL input path.
                    vrAttachVirtualSDLGamepad();
                }
                else
                {
                    std::printf("OpenXR: Controller actions not attached; creation/bindings incomplete.\n");
                }

                // Create the left-hand 6DoF tracking space.
                if (g_xrSession != XR_NULL_HANDLE &&
                    g_xrLeftHandPoseAction != XR_NULL_HANDLE &&
                    g_xrLeftHandPath != XR_NULL_PATH)
                {
                    XrActionSpaceCreateInfo leftHandSpaceInfo{};
                    leftHandSpaceInfo.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;

                    leftHandSpaceInfo.action = g_xrLeftHandPoseAction;
                    leftHandSpaceInfo.subactionPath = g_xrLeftHandPath;

                    // Identity pose: no additional controller offset.
                    leftHandSpaceInfo.poseInActionSpace.orientation.w = 1.0f;

                    XrResult leftHandSpaceResult = xrCreateActionSpace(
                        g_xrSession,
                        &leftHandSpaceInfo,
                        &g_xrLeftHandSpace
                    );

                    if (XR_SUCCEEDED(leftHandSpaceResult))
                    {
                        std::printf("OpenXR: Left-hand 6DoF tracking space created!\n");
                    }
                    else
                    {
                        SDL_Log(
                            "OpenXR: Left-hand tracking space creation failed (%d)",
                            static_cast<int>(leftHandSpaceResult)
                        );

                        g_xrLeftHandSpace = XR_NULL_HANDLE;
                    }
                }
                else
                {
                    SDL_Log(
                        "OpenXR: Cannot create left-hand tracking space; required handles unavailable."
                    );
                }
                // Create the right-hand 6DoF tracking space.
                if (g_xrSession != XR_NULL_HANDLE &&
                    g_xrRightHandPoseAction != XR_NULL_HANDLE &&
                    g_xrRightHandPath != XR_NULL_PATH)
                {
                    XrActionSpaceCreateInfo rightHandSpaceInfo{};
                    rightHandSpaceInfo.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;

                    rightHandSpaceInfo.action = g_xrRightHandPoseAction;
                    rightHandSpaceInfo.subactionPath = g_xrRightHandPath;

                    // Identity pose: no additional controller offset.
                    rightHandSpaceInfo.poseInActionSpace.orientation.w = 1.0f;

                    XrResult rightHandSpaceResult = xrCreateActionSpace(
                        g_xrSession,
                        &rightHandSpaceInfo,
                        &g_xrRightHandSpace
                    );

                    if (XR_SUCCEEDED(rightHandSpaceResult))
                    {
                        std::printf("OpenXR: Right-hand 6DoF tracking space created!\n");
                    }
                    else
                    {
                        SDL_Log(
                            "OpenXR: Right-hand tracking space creation failed (%d)",
                            static_cast<int>(rightHandSpaceResult)
                        );

                        g_xrRightHandSpace = XR_NULL_HANDLE;
                    }
                }
                else
                {
                    SDL_Log(
                        "OpenXR: Cannot create right-hand tracking space; required handles unavailable."
                    );
                }
                // Aim spaces use aim/pose instead of grip/pose for pointing.
                auto createAimSpace = [&](XrAction action, XrPath hand,
                    XrSpace& output, const char* handName)
                    {
                        if (action == XR_NULL_HANDLE || hand == XR_NULL_PATH)
                        {
                            SDL_Log("OpenXR: %s aim space skipped (missing action/path)", handName);
                            return;
                        }
                        XrActionSpaceCreateInfo info{};
                        info.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
                        info.action = action;
                        info.subactionPath = hand;
                        info.poseInActionSpace.orientation.w = 1.0f;
                        const XrResult r = xrCreateActionSpace(g_xrSession, &info, &output);
                        std::printf("OpenXR: %s aim space %s (%d)\n", handName,
                            XR_SUCCEEDED(r) ? "created" : "FAILED", static_cast<int>(r));
                        if (XR_FAILED(r)) output = XR_NULL_HANDLE;
                    };
                createAimSpace(g_xrLeftAimPoseAction, g_xrLeftHandPath,
                    g_xrLeftAimSpace, "left");
                createAimSpace(g_xrRightAimPoseAction, g_xrRightHandPath,
                    g_xrRightAimSpace, "right");

                // Create a local reference space for headset tracking.
                XrReferenceSpaceCreateInfo spaceInfo{};
                spaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
                spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
                spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;

                XrResult spaceResult = xrCreateReferenceSpace(
                    g_xrSession,
                    &spaceInfo,
                    &g_xrReferenceSpace
                );

                if (XR_SUCCEEDED(spaceResult))
                {
                    std::printf(
                        "OpenXR: LOCAL reference space created successfully!\n"
                    );
                }
                else
                {
                    std::printf(
                        "OpenXR: Reference space creation failed (%d)\n",
                        static_cast<int>(spaceResult)
                    );
                }

                // Create a headset-relative reference space for the GUI.
                XrReferenceSpaceCreateInfo guiSpaceInfo{};
                guiSpaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
                guiSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
                guiSpaceInfo.poseInReferenceSpace.orientation.w = 1.0f;

                XrResult guiSpaceResult = xrCreateReferenceSpace(
                    g_xrSession,
                    &guiSpaceInfo,
                    &g_xrGuiViewSpace
                );

                if (XR_SUCCEEDED(guiSpaceResult))
                {
                    SDL_Log("OpenXR: GUI VIEW reference space created!");
                    // Configure the headset-relative GUI quad layer.
                    g_xrGuiLayer.space = g_xrGuiViewSpace;
                    g_xrGuiLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;

                    // Keep the GUI directly in front of the headset.
                    g_xrGuiLayer.pose.orientation.w = 1.0f;
                    g_xrGuiLayer.pose.position.z = -1.0f;

                    // Set the GUI panel to a 16:9 aspect ratio.
                    g_xrGuiLayer.size.width = 1.2f;
                    g_xrGuiLayer.size.height = 0.675f;
                }
                else
                {
                    SDL_Log(
                        "OpenXR: GUI VIEW reference space failed (%d)",
                        static_cast<int>(guiSpaceResult)
                    );
                }
                if (XR_SUCCEEDED(result))
                {
                    std::printf("OpenXR: VR session created successfully!\n");

                    // Check which swapchain formats SteamVR supports.
                    vrCheckSwapchainFormats();
                    vrCheckViewConfiguration();
                    // Create the stereo swapchain for both eyes.
                    vrCreateSwapchain();
                    // Create a separate OpenXR swapchain for the VR GUI.
                    if (g_xrGuiViewSpace != XR_NULL_HANDLE &&
                        g_xrGuiSwapchain == XR_NULL_HANDLE)
                    {
                        XrSwapchainCreateInfo guiSwapchainInfo{};
                        guiSwapchainInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
                        guiSwapchainInfo.usageFlags =
                            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
                        guiSwapchainInfo.format = g_xrSwapchainFormat;
                        guiSwapchainInfo.sampleCount = 1;
                        guiSwapchainInfo.width = 1280;
                        guiSwapchainInfo.height = 720;
                        guiSwapchainInfo.faceCount = 1;
                        guiSwapchainInfo.arraySize = 1;
                        guiSwapchainInfo.mipCount = 1;

                        XrResult guiSwapchainResult = xrCreateSwapchain(
                            g_xrSession,
                            &guiSwapchainInfo,
                            &g_xrGuiSwapchain
                        );

                        if (XR_SUCCEEDED(guiSwapchainResult))
                        {
                            SDL_Log("OpenXR: GUI swapchain created successfully!");

                            // Connect the GUI quad layer to its swapchain.
                            g_xrGuiLayer.subImage.swapchain = g_xrGuiSwapchain;
                            g_xrGuiLayer.subImage.imageRect.offset = { 0, 0 };
                            g_xrGuiLayer.subImage.imageRect.extent = { 1280, 720 };
                            g_xrGuiLayer.subImage.imageArrayIndex = 0;
                            // Retrieve the OpenGL textures for the GUI swapchain.
                            uint32_t guiImageCount = 0;

                            XrResult guiImagesResult = xrEnumerateSwapchainImages(
                                g_xrGuiSwapchain,
                                0,
                                &guiImageCount,
                                nullptr
                            );

                            if (XR_SUCCEEDED(guiImagesResult) && guiImageCount > 0)
                            {
                                g_xrGuiSwapchainImages.resize(guiImageCount);

                                for (auto& image : g_xrGuiSwapchainImages)
                                {
                                    image.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
                                    image.next = nullptr;
                                }

                                guiImagesResult = xrEnumerateSwapchainImages(
                                    g_xrGuiSwapchain,
                                    guiImageCount,
                                    &guiImageCount,
                                    reinterpret_cast<XrSwapchainImageBaseHeader*>(
                                        g_xrGuiSwapchainImages.data()
                                        )
                                );

                                if (XR_SUCCEEDED(guiImagesResult))
                                {
                                    SDL_Log(
                                        "OpenXR: Retrieved %u GUI swapchain images!",
                                        guiImageCount
                                    );
                                }
                                else
                                {
                                    SDL_Log(
                                        "OpenXR: Failed to retrieve GUI images (%d)",
                                        static_cast<int>(guiImagesResult)
                                    );

                                    g_xrGuiSwapchainImages.clear();
                                }
                            }
                            else
                            {
                                SDL_Log(
                                    "OpenXR: Failed to query GUI image count (%d)",
                                    static_cast<int>(guiImagesResult)
                                );
                            }
                        }
                        else
                        {
                            SDL_Log(
                                "OpenXR: GUI swapchain creation failed (%d)",
                                static_cast<int>(guiSwapchainResult)
                            );
                        }
                    }
                }
            }
            else
            {
                std::printf(
                    "OpenXR: VR session creation failed (%d)\n",
                    static_cast<int>(result)
                );
                g_xrSession = XR_NULL_HANDLE;
            }
        }
    }
    else
    {
        std::printf("OpenXR: Failed to retrieve graphics handles.\n");
    }
}


bool vrOpenXRTest()
{
    // Safe desktop fallback even when the OpenXR runtime is installed:
    // never create an XR instance when the user disables VR. If no headset
    // is present, existing xrGetSystem failure handling returns false and
    // the regular desktop game continues using its original renderer/input.
    vrReadFeatureSettings();
    // One-shot desktop launch: no write to vr_settings.cfg, and no XR API calls.
    const char* desktopOnly = std::getenv("BARONY_VR_DESKTOP");
    if (desktopOnly && std::strcmp(desktopOnly, "1") == 0)
    {
        std::printf("OpenXR VR STEP638FP: BARONY_VR_DESKTOP=1; skipping OpenXR.\n");
        return false;
    }
    if (!g_vrUserEnabled)
    {
        std::printf("OpenXR VR STEP638FO: vr_enabled=0, desktop mode requested.\n");
        return false;
    }
    XrInstance& instance = g_xrInstance;

    if (instance != XR_NULL_HANDLE)
    {
        std::printf("OpenXR: Instance already initialized.\n");
        return true;
    }

    XrInstanceCreateInfo createInfo{};
    createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;


    // Enable OpenGL support for future VR session creation.
    const char* extensions[] = {
        XR_KHR_OPENGL_ENABLE_EXTENSION_NAME
    };

    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = extensions;

    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);

    sprintf_s(
        createInfo.applicationInfo.applicationName,
        XR_MAX_APPLICATION_NAME_SIZE,
        "%s",
        "Barony VR Test"
    );

    sprintf_s(
        createInfo.applicationInfo.engineName,
        XR_MAX_ENGINE_NAME_SIZE,
        "%s",
        "Barony"
    );

    XrResult result = xrCreateInstance(&createInfo, &instance);

    if (XR_FAILED(result))
    {
        std::printf("OpenXR: xrCreateInstance failed (%d)\n",
            static_cast<int>(result));
        return false;
    }
    // Create the action set for Quest 3 controller inputs.
// This will later contain buttons, thumbsticks,
// triggers, grips, and independent hand poses.
    XrActionSetCreateInfo actionSetInfo{};
    actionSetInfo.type = XR_TYPE_ACTION_SET_CREATE_INFO;

    sprintf_s(
        actionSetInfo.actionSetName,
        XR_MAX_ACTION_SET_NAME_SIZE,
        "%s",
        "barony_vr_controls"
    );

    sprintf_s(
        actionSetInfo.localizedActionSetName,
        XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE,
        "%s",
        "Barony VR Controls"
    );

    actionSetInfo.priority = 0;

    XrResult actionSetResult = xrCreateActionSet(
        instance,
        &actionSetInfo,
        &g_xrControllerActionSet
    );

    if (XR_SUCCEEDED(actionSetResult))
    {
        SDL_Log(
            "OpenXR: Quest 3 controller action set created!"
        );
    }
    else
    {
        SDL_Log(
            "OpenXR: Controller action set creation failed (%d)",
            static_cast<int>(actionSetResult)
        );

        g_xrControllerActionSet = XR_NULL_HANDLE;
    }
    // Register OpenXR paths for both Quest 3 controllers.
    XrResult leftHandResult = xrStringToPath(
        instance,
        "/user/hand/left",
        &g_xrLeftHandPath
    );

    XrResult rightHandResult = xrStringToPath(
        instance,
        "/user/hand/right",
        &g_xrRightHandPath
    );

    if (XR_SUCCEEDED(leftHandResult) &&
        XR_SUCCEEDED(rightHandResult))
    {
        SDL_Log("OpenXR: Left and right controller paths registered!");
    }
    else
    {
        SDL_Log(
            "OpenXR: Controller path registration failed (Left: %d, Right: %d)",
            static_cast<int>(leftHandResult),
            static_cast<int>(rightHandResult)
        );
    }
    // Create the left-hand 6DoF tracking action.
    if (g_xrControllerActionSet != XR_NULL_HANDLE &&
        g_xrLeftHandPath != XR_NULL_PATH)
    {
        XrActionCreateInfo leftPoseInfo{};
        leftPoseInfo.type = XR_TYPE_ACTION_CREATE_INFO;
        leftPoseInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;

        sprintf_s(
            leftPoseInfo.actionName,
            XR_MAX_ACTION_NAME_SIZE,
            "%s",
            "left_hand_pose"
        );

        sprintf_s(
            leftPoseInfo.localizedActionName,
            XR_MAX_LOCALIZED_ACTION_NAME_SIZE,
            "%s",
            "Left Hand Pose"
        );

        leftPoseInfo.countSubactionPaths = 1;
        leftPoseInfo.subactionPaths = &g_xrLeftHandPath;

        XrResult leftPoseResult = xrCreateAction(
            g_xrControllerActionSet,
            &leftPoseInfo,
            &g_xrLeftHandPoseAction
        );

        if (XR_SUCCEEDED(leftPoseResult))
        {
            SDL_Log("OpenXR: Left-hand 6DoF pose action created!");
        }
        else
        {
            SDL_Log(
                "OpenXR: Left-hand pose action creation failed (%d)",
                static_cast<int>(leftPoseResult)
            );

            g_xrLeftHandPoseAction = XR_NULL_HANDLE;
        }
    }
    else
    {
        SDL_Log(
            "OpenXR: Skipping left-hand pose action; action set or hand path unavailable."
        );
    }
    // Create the right-hand 6DoF tracking action.
    if (g_xrControllerActionSet != XR_NULL_HANDLE &&
        g_xrRightHandPath != XR_NULL_PATH)
    {
        XrActionCreateInfo rightPoseInfo{};
        rightPoseInfo.type = XR_TYPE_ACTION_CREATE_INFO;
        rightPoseInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;

        sprintf_s(
            rightPoseInfo.actionName,
            XR_MAX_ACTION_NAME_SIZE,
            "%s",
            "right_hand_pose"
        );

        sprintf_s(
            rightPoseInfo.localizedActionName,
            XR_MAX_LOCALIZED_ACTION_NAME_SIZE,
            "%s",
            "Right Hand Pose"
        );

        rightPoseInfo.countSubactionPaths = 1;
        rightPoseInfo.subactionPaths = &g_xrRightHandPath;

        XrResult rightPoseResult = xrCreateAction(
            g_xrControllerActionSet,
            &rightPoseInfo,
            &g_xrRightHandPoseAction
        );

        if (XR_SUCCEEDED(rightPoseResult))
        {
            SDL_Log("OpenXR: Right-hand 6DoF pose action created!");
        }
        else
        {
            SDL_Log(
                "OpenXR: Right-hand pose action creation failed (%d)",
                static_cast<int>(rightPoseResult)
            );

            g_xrRightHandPoseAction = XR_NULL_HANDLE;
        }
    }
    else
    {
        SDL_Log(
            "OpenXR: Skipping right-hand pose action; action set or hand path unavailable."
        );
    }
    // Create a shared analog thumbstick action with independent
// left-hand and right-hand subaction paths.
    if (g_xrControllerActionSet != XR_NULL_HANDLE &&
        g_xrLeftHandPath != XR_NULL_PATH &&
        g_xrRightHandPath != XR_NULL_PATH)
    {
        XrPath thumbstickHandPaths[2] = {
            g_xrLeftHandPath,
            g_xrRightHandPath
        };

        XrActionCreateInfo thumbstickInfo{};
        thumbstickInfo.type = XR_TYPE_ACTION_CREATE_INFO;
        thumbstickInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;

        sprintf_s(
            thumbstickInfo.actionName,
            XR_MAX_ACTION_NAME_SIZE,
            "%s",
            "thumbstick"
        );

        sprintf_s(
            thumbstickInfo.localizedActionName,
            XR_MAX_LOCALIZED_ACTION_NAME_SIZE,
            "%s",
            "Controller Thumbstick"
        );

        thumbstickInfo.countSubactionPaths = 2;
        thumbstickInfo.subactionPaths = thumbstickHandPaths;

        XrResult thumbstickResult = xrCreateAction(
            g_xrControllerActionSet,
            &thumbstickInfo,
            &g_xrThumbstickAction
        );

        if (XR_SUCCEEDED(thumbstickResult))
        {
            SDL_Log(
                "OpenXR: Dual-controller thumbstick action created!"
            );
        }
        else
        {
            SDL_Log(
                "OpenXR: Thumbstick action creation failed (%d)",
                static_cast<int>(thumbstickResult)
            );

            g_xrThumbstickAction = XR_NULL_HANDLE;
        }
    }
    else
    {
        SDL_Log(
            "OpenXR: Cannot create thumbstick action; required handles unavailable."
        );
    }
    // Create a shared analog trigger action with independent
// left-hand and right-hand subaction paths.
    if (g_xrControllerActionSet != XR_NULL_HANDLE &&
        g_xrLeftHandPath != XR_NULL_PATH &&
        g_xrRightHandPath != XR_NULL_PATH)
    {
        XrPath triggerHandPaths[2] = {
            g_xrLeftHandPath,
            g_xrRightHandPath
        };

        XrActionCreateInfo triggerInfo{};
        triggerInfo.type = XR_TYPE_ACTION_CREATE_INFO;
        triggerInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;

        sprintf_s(
            triggerInfo.actionName,
            XR_MAX_ACTION_NAME_SIZE,
            "%s",
            "trigger"
        );

        sprintf_s(
            triggerInfo.localizedActionName,
            XR_MAX_LOCALIZED_ACTION_NAME_SIZE,
            "%s",
            "Controller Trigger"
        );

        triggerInfo.countSubactionPaths = 2;
        triggerInfo.subactionPaths = triggerHandPaths;

        XrResult triggerResult = xrCreateAction(
            g_xrControllerActionSet,
            &triggerInfo,
            &g_xrTriggerAction
        );

        if (XR_SUCCEEDED(triggerResult))
        {
            SDL_Log(
                "OpenXR: Dual-controller analog trigger action created!"
            );
        }
        else
        {
            SDL_Log(
                "OpenXR: Trigger action creation failed (%d)",
                static_cast<int>(triggerResult)
            );

            g_xrTriggerAction = XR_NULL_HANDLE;
        }
    }
    else
    {
        SDL_Log(
            "OpenXR: Cannot create trigger action; required handles unavailable."
        );
    }
    // Create a shared analog squeeze/grip action for both Quest controllers.
    // Left and right grips are read independently through subaction paths.
    if (g_xrControllerActionSet != XR_NULL_HANDLE &&
        g_xrLeftHandPath != XR_NULL_PATH &&
        g_xrRightHandPath != XR_NULL_PATH)
    {
        XrPath gripHandPaths[2] = {
            g_xrLeftHandPath,
            g_xrRightHandPath
        };

        XrActionCreateInfo gripInfo{};
        gripInfo.type = XR_TYPE_ACTION_CREATE_INFO;
        gripInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;

        sprintf_s(
            gripInfo.actionName,
            XR_MAX_ACTION_NAME_SIZE,
            "%s",
            "squeeze"
        );

        sprintf_s(
            gripInfo.localizedActionName,
            XR_MAX_LOCALIZED_ACTION_NAME_SIZE,
            "%s",
            "Controller Grip"
        );

        gripInfo.countSubactionPaths = 2;
        gripInfo.subactionPaths = gripHandPaths;

        XrResult gripResult = xrCreateAction(
            g_xrControllerActionSet,
            &gripInfo,
            &g_xrGripAction
        );

        if (XR_SUCCEEDED(gripResult))
        {
            SDL_Log("OpenXR: Dual-controller analog grip action created!");
        }
        else
        {
            SDL_Log(
                "OpenXR: Grip action creation failed (%d)",
                static_cast<int>(gripResult)
            );
            g_xrGripAction = XR_NULL_HANDLE;
        }
    }
    else
    {
        SDL_Log("OpenXR: Cannot create grip action; required handles unavailable.");
    }

    // Finish the controller action set BEFORE attaching it to the session.
    // The helper gives every button the correct hand-specific subaction path.
    auto createExtraAction = [&](XrActionType type, const char* actionName,
        const char* localizedName, XrAction& output,
        const XrPath* subactionPaths, uint32_t count) -> bool
        {
            if (g_xrControllerActionSet == XR_NULL_HANDLE) return false;
            XrActionCreateInfo info{};
            info.type = XR_TYPE_ACTION_CREATE_INFO;
            info.actionType = type;
            sprintf_s(info.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", actionName);
            sprintf_s(info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE,
                "%s", localizedName);
            info.countSubactionPaths = count;
            info.subactionPaths = subactionPaths;
            const XrResult r = xrCreateAction(g_xrControllerActionSet, &info, &output);
            if (XR_FAILED(r))
            {
                output = XR_NULL_HANDLE;
                SDL_Log("OpenXR: Failed to create %s action (%d)",
                    actionName, static_cast<int>(r));
            }
            return XR_SUCCEEDED(r);
        };

    const XrPath leftOnly[] = { g_xrLeftHandPath };
    const XrPath rightOnly[] = { g_xrRightHandPath };
    const XrPath bothHands[] = { g_xrLeftHandPath, g_xrRightHandPath };
    bool additionalActionsCreated = true;
    additionalActionsCreated &= createExtraAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x", "X Button",
        g_xrXButtonAction, leftOnly, 1);
    additionalActionsCreated &= createExtraAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y", "Y Button",
        g_xrYButtonAction, leftOnly, 1);
    additionalActionsCreated &= createExtraAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", "A Button",
        g_xrAButtonAction, rightOnly, 1);
    additionalActionsCreated &= createExtraAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", "B Button",
        g_xrBButtonAction, rightOnly, 1);
    additionalActionsCreated &= createExtraAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu_left", "Left Menu",
        g_xrLeftMenuAction, leftOnly, 1);
    additionalActionsCreated &= createExtraAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Stick Click",
        g_xrThumbstickClickAction, bothHands, 2);
    additionalActionsCreated &= createExtraAction(XR_ACTION_TYPE_POSE_INPUT, "left_aim", "Left Aim Pose",
        g_xrLeftAimPoseAction, leftOnly, 1);
    additionalActionsCreated &= createExtraAction(XR_ACTION_TYPE_POSE_INPUT, "right_aim", "Right Aim Pose",
        g_xrRightAimPoseAction, rightOnly, 1);
    additionalActionsCreated &= createExtraAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "hand_haptics", "Hand Haptics",
        g_xrHapticAction, bothHands, 2);
    // Optional: failure must not remove the established controller actions.
    createExtraAction(XR_ACTION_TYPE_BOOLEAN_INPUT,"right_thumbrest","Right Thumb Rest",
        g_xrRightThumbrestAction,rightOnly,1);
    SDL_Log("OpenXR: Additional controller actions %s",
        additionalActionsCreated ? "created" : "INCOMPLETE");

    // Register the standard Oculus Touch interaction profile.
// This provides the input path layout for Quest controllers.
    XrResult touchProfileResult = xrStringToPath(
        instance,
        "/interaction_profiles/oculus/touch_controller",
        &g_xrTouchControllerProfilePath
    );

    if (XR_SUCCEEDED(touchProfileResult))
    {
        SDL_Log(
            "OpenXR: Oculus Touch interaction profile path registered!"
        );
    }
    else
    {
        SDL_Log(
            "OpenXR: Oculus Touch profile path registration failed (%d)",
            static_cast<int>(touchProfileResult)
        );

        g_xrTouchControllerProfilePath = XR_NULL_PATH;
    }
    // Suggest all controller bindings together, exactly once per interaction profile.
    // This does not bind a right-hand system/menu button: runtimes reserve it.
    if (g_xrTouchControllerProfilePath != XR_NULL_PATH &&
        g_xrLeftHandPoseAction != XR_NULL_HANDLE &&
        g_xrRightHandPoseAction != XR_NULL_HANDLE &&
        g_xrThumbstickAction != XR_NULL_HANDLE &&
        g_xrTriggerAction != XR_NULL_HANDLE &&
        g_xrGripAction != XR_NULL_HANDLE && additionalActionsCreated)
    {
        std::vector<XrActionSuggestedBinding> controllerBindings;
        bool allPathsValid = true;
        auto addBinding = [&](XrAction action, const char* path)
            {
                XrPath inputPath = XR_NULL_PATH;
                const XrResult r = xrStringToPath(instance, path, &inputPath);
                if (XR_FAILED(r))
                {
                    SDL_Log("OpenXR: Failed to register input path %s (%d)",
                        path, static_cast<int>(r));
                    allPathsValid = false;
                    return;
                }
                controllerBindings.push_back({ action, inputPath });
            };

        addBinding(g_xrLeftHandPoseAction, "/user/hand/left/input/grip/pose");
        addBinding(g_xrRightHandPoseAction, "/user/hand/right/input/grip/pose");
        addBinding(g_xrLeftAimPoseAction, "/user/hand/left/input/aim/pose");
        addBinding(g_xrRightAimPoseAction, "/user/hand/right/input/aim/pose");
        addBinding(g_xrThumbstickAction, "/user/hand/left/input/thumbstick");
        addBinding(g_xrThumbstickAction, "/user/hand/right/input/thumbstick");
        addBinding(g_xrThumbstickClickAction, "/user/hand/left/input/thumbstick/click");
        addBinding(g_xrThumbstickClickAction, "/user/hand/right/input/thumbstick/click");
        addBinding(g_xrTriggerAction, "/user/hand/left/input/trigger/value");
        addBinding(g_xrTriggerAction, "/user/hand/right/input/trigger/value");
        addBinding(g_xrGripAction, "/user/hand/left/input/squeeze/value");
        addBinding(g_xrGripAction, "/user/hand/right/input/squeeze/value");
        addBinding(g_xrXButtonAction, "/user/hand/left/input/x/click");
        addBinding(g_xrYButtonAction, "/user/hand/left/input/y/click");
        addBinding(g_xrAButtonAction, "/user/hand/right/input/a/click");
        addBinding(g_xrBButtonAction, "/user/hand/right/input/b/click");
        addBinding(g_xrLeftMenuAction, "/user/hand/left/input/menu/click");
        addBinding(g_xrHapticAction, "/user/hand/left/output/haptic");
        addBinding(g_xrHapticAction, "/user/hand/right/output/haptic");

        const size_t baseBindingCount=controllerBindings.size();
        if (g_xrRightThumbrestAction != XR_NULL_HANDLE) {
            XrPath restPath=XR_NULL_PATH;
            if (XR_SUCCEEDED(xrStringToPath(instance,"/user/hand/right/input/thumbrest/touch",&restPath)))
                controllerBindings.push_back({g_xrRightThumbrestAction,restPath});
        }
        if (allPathsValid)
        {
            XrInteractionProfileSuggestedBinding suggested{};
            suggested.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
            suggested.interactionProfile = g_xrTouchControllerProfilePath;
            suggested.countSuggestedBindings =
                static_cast<uint32_t>(controllerBindings.size());
            suggested.suggestedBindings = controllerBindings.data();
            XrResult r = xrSuggestInteractionProfileBindings(instance, &suggested);
            if (XR_FAILED(r) && controllerBindings.size()>baseBindingCount) {
                printlog("[VR GA21] thumbrest binding rejected (%d); retry original bindings",static_cast<int>(r));
                controllerBindings.resize(baseBindingCount);
                suggested.countSuggestedBindings=static_cast<uint32_t>(baseBindingCount);
                suggested.suggestedBindings=controllerBindings.data();
                r=xrSuggestInteractionProfileBindings(instance,&suggested);
            }
            g_xrControllerBindingsSuggested = XR_SUCCEEDED(r);
            SDL_Log("OpenXR: Suggested %u Touch bindings: %s (%d)",
                suggested.countSuggestedBindings,
                g_xrControllerBindingsSuggested ? "SUCCESS" : "FAILED",
                static_cast<int>(r));
        }
    }
    else
    {
        SDL_Log("OpenXR: Controller binding setup skipped; action/profile missing.");
    }
    XrSystemId& systemId = g_xrSystemId;
    static XrSystemId g_xrSystemId = XR_NULL_SYSTEM_ID;
    XrGraphicsRequirementsOpenGLKHR graphicsRequirements{};
    graphicsRequirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR;
    PFN_xrGetOpenGLGraphicsRequirementsKHR getGraphicsRequirements = nullptr;

    XrResult functionResult = xrGetInstanceProcAddr(
        instance,
        "xrGetOpenGLGraphicsRequirementsKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&getGraphicsRequirements)
    );

    if (XR_FAILED(functionResult) || getGraphicsRequirements == nullptr)
    {
        std::printf("OpenXR: Failed to locate OpenGL graphics requirements function.\n");
        xrDestroyInstance(instance);
        instance = XR_NULL_HANDLE;
        return false;
    }

    std::printf("OpenXR: OpenGL graphics requirements function found!\n");

    XrSystemGetInfo systemInfo{};
    systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrResult systemResult = xrGetSystem(
        instance,
        &systemInfo,
        &systemId
    );
    if (XR_SUCCEEDED(systemResult))
    {
        // Check which environment blend modes SteamVR supports.
        uint32_t blendModeCount = 0;

        XrResult blendResult = xrEnumerateEnvironmentBlendModes(
            instance,
            systemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            0,
            &blendModeCount,
            nullptr
        );

        if (XR_SUCCEEDED(blendResult) && blendModeCount > 0)
        {
            std::vector<XrEnvironmentBlendMode> blendModes(blendModeCount);

            blendResult = xrEnumerateEnvironmentBlendModes(
                instance,
                systemId,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                blendModeCount,
                &blendModeCount,
                blendModes.data()
            );

            if (XR_SUCCEEDED(blendResult))
            {
                for (XrEnvironmentBlendMode mode : blendModes)
                {
                    std::printf(
                        "OpenXR: Supported environment blend mode: %d (%s)\n",
                        static_cast<int>(mode),
                        mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE ? "OPAQUE" :
                        mode == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE ? "ADDITIVE" :
                        mode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND ? "ALPHA_BLEND" :
                        "UNKNOWN"
                    );
                }
            }
        }
        else
        {
            std::printf(
                "OpenXR: Failed to enumerate environment blend modes (%d)\n",
                static_cast<int>(blendResult)
            );
        }
        XrResult graphicsResult = getGraphicsRequirements(
            instance,
            systemId,
            &graphicsRequirements
        );

        if (XR_SUCCEEDED(graphicsResult))
        {
            std::printf(
                "OpenXR: Minimum OpenGL version: %u.%u\n",
                XR_VERSION_MAJOR(graphicsRequirements.minApiVersionSupported),
                XR_VERSION_MINOR(graphicsRequirements.minApiVersionSupported)
            );

            std::printf(
                "OpenXR: Maximum OpenGL version: %u.%u\n",
                XR_VERSION_MAJOR(graphicsRequirements.maxApiVersionSupported),
                XR_VERSION_MINOR(graphicsRequirements.maxApiVersionSupported)
            );
        }
        else
        {
            std::printf(
                "OpenXR: Failed to retrieve OpenGL requirements (%d)\n",
                static_cast<int>(graphicsResult)
            );
        }
    }
    if (XR_SUCCEEDED(systemResult)) {
        std::printf("OpenXR: VR headset detected successfully!\n");
        XrSystemProperties properties{};
        properties.type = XR_TYPE_SYSTEM_PROPERTIES;

        XrResult propertiesResult = xrGetSystemProperties(
            instance,
            systemId,
            &properties
        );

        if (XR_SUCCEEDED(propertiesResult)) {
            std::printf("OpenXR: Headset system: %s\n", properties.systemName);
            std::printf("OpenXR: Maximum swapchain width: %u\n",
                properties.graphicsProperties.maxSwapchainImageWidth);
            std::printf("OpenXR: Maximum swapchain height: %u\n",
                properties.graphicsProperties.maxSwapchainImageHeight);
            std::printf("OpenXR: Orientation tracking: %s\n",
                properties.trackingProperties.orientationTracking ? "Yes" : "No");
            std::printf("OpenXR: Position tracking: %s\n",
                properties.trackingProperties.positionTracking ? "Yes" : "No");
        }
        else {
            std::printf("OpenXR: Could not retrieve headset properties (%d)\n",
                static_cast<int>(propertiesResult));
        }
    }
    else {
        std::printf(
            "OpenXR: headset detection failed (%d)\n",
            static_cast<int>(systemResult)
        );

        xrDestroyInstance(instance);
        instance = XR_NULL_HANDLE;
        systemId = XR_NULL_SYSTEM_ID;
        return false;
    }
    // Keep the OpenXR instance alive for future VR session creation.
    std::printf("OpenXR: runtime instance retained for VR session creation.\n");
    return true;
}
void vrPollOpenXREvents()
{
    if (g_xrInstance == XR_NULL_HANDLE)
    {
        return;
    }

    XrEventDataBuffer event{};
    event.type = XR_TYPE_EVENT_DATA_BUFFER;

    XrResult result;

    while ((result = xrPollEvent(g_xrInstance, &event)) == XR_SUCCESS)
    {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            const XrEventDataSessionStateChanged* stateEvent =
                reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);

            if (stateEvent->session == g_xrSession)
            {
                g_xrSessionState = stateEvent->state;
                if (g_xrSessionState != XR_SESSION_STATE_FOCUSED)
                {
                    vrUpdateMenuNavigation(false);
                    vrUpdateVirtualSDLGamepad(false);
                }

                std::printf(
                    "OpenXR: Session state changed to %d (%s)\n",
                    static_cast<int>(g_xrSessionState),
                    g_xrSessionState == XR_SESSION_STATE_FOCUSED ? "FOCUSED" :
                    g_xrSessionState == XR_SESSION_STATE_VISIBLE ? "VISIBLE" :
                    g_xrSessionState == XR_SESSION_STATE_SYNCHRONIZED ? "SYNCHRONIZED" :
                    g_xrSessionState == XR_SESSION_STATE_READY ? "READY" :
                    g_xrSessionState == XR_SESSION_STATE_STOPPING ? "STOPPING" :
                    "OTHER"
                );                if (g_xrSessionState == XR_SESSION_STATE_READY && !g_xrSessionRunning)
                {
                    XrSessionBeginInfo beginInfo{};
                    beginInfo.type = XR_TYPE_SESSION_BEGIN_INFO;
                    beginInfo.primaryViewConfigurationType =
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

                    XrResult beginResult = xrBeginSession(g_xrSession, &beginInfo);

                    if (XR_SUCCEEDED(beginResult))
                    {
                        g_xrSessionRunning = true;
                        std::printf("OpenXR: VR session started successfully!\n");
                    }
                    else
                    {
                        std::printf(
                            "OpenXR: Failed to start VR session (%d)\n",
                            static_cast<int>(beginResult)
                        );
                    }
                }
            }
        }

        event = {};
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
    }

    if (XR_FAILED(result))
    {
        std::printf(
            "OpenXR: Event polling failed (%d)\n",
            static_cast<int>(result)
        );
    }
}
// Read controller inputs and tracking without affecting Barony's existing controls.
// Both grip and aim poses are relative to the SAME LOCAL reference space.
static void vrProcessPhysicalMelee(XrTime predictedDisplayTime);
static void vrUpdateCustomCrosshairAim(XrTime predictedTime);
static void vrUpdateControllerInputs(XrTime predictedDisplayTime)
{
    if (!g_xrControllerActionsAttached || g_xrSession == XR_NULL_HANDLE ||
        g_xrReferenceSpace == XR_NULL_HANDLE)
    {
        vrUpdateMenuNavigation(false);
        vrUpdateVirtualSDLGamepad(false);
        return;
    }

    XrActiveActionSet activeSet{};
    activeSet.actionSet = g_xrControllerActionSet;
    activeSet.subactionPath = XR_NULL_PATH;
    XrActionsSyncInfo syncInfo{};
    syncInfo.type = XR_TYPE_ACTIONS_SYNC_INFO;
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeSet;
    const XrResult syncResult = xrSyncActions(g_xrSession, &syncInfo);
    if (XR_FAILED(syncResult))
    {
        static bool syncErrorLogged = false;
        if (!syncErrorLogged)
        {
            std::printf("OpenXR: xrSyncActions failed (%d)\n",
                static_cast<int>(syncResult));
            syncErrorLogged = true;
        }
        vrUpdateMenuNavigation(false);
        vrUpdateVirtualSDLGamepad(false);
        return;
    }

    auto readStick = [&](XrPath hand) -> XrVector2f
        {
            XrActionStateGetInfo info{};
            info.type = XR_TYPE_ACTION_STATE_GET_INFO;
            info.action = g_xrThumbstickAction;
            info.subactionPath = hand;
            XrActionStateVector2f state{};
            state.type = XR_TYPE_ACTION_STATE_VECTOR2F;
            if (XR_SUCCEEDED(xrGetActionStateVector2f(g_xrSession, &info, &state)) &&
                state.isActive) return state.currentState;
            return {};
        };
    auto readFloat = [&](XrAction action, XrPath hand) -> float
        {
            XrActionStateGetInfo info{};
            info.type = XR_TYPE_ACTION_STATE_GET_INFO;
            info.action = action;
            info.subactionPath = hand;
            XrActionStateFloat state{};
            state.type = XR_TYPE_ACTION_STATE_FLOAT;
            if (XR_SUCCEEDED(xrGetActionStateFloat(g_xrSession, &info, &state)) &&
                state.isActive) return state.currentState;
            return 0.0f;
        };
    auto readButton = [&](XrAction action, XrPath hand) -> bool
        {
            XrActionStateGetInfo info{};
            info.type = XR_TYPE_ACTION_STATE_GET_INFO;
            info.action = action;
            info.subactionPath = hand;
            XrActionStateBoolean state{};
            state.type = XR_TYPE_ACTION_STATE_BOOLEAN;
            return XR_SUCCEEDED(xrGetActionStateBoolean(g_xrSession, &info, &state)) &&
                state.isActive && state.currentState == XR_TRUE;
        };
    auto readPose = [&](XrAction action, XrPath hand, XrSpace space,
        bool& valid, XrPosef& pose)
        {
            valid = false;
            if (space == XR_NULL_HANDLE) return;
            XrActionStateGetInfo info{};
            info.type = XR_TYPE_ACTION_STATE_GET_INFO;
            info.action = action;
            info.subactionPath = hand;
            XrActionStatePose poseState{};
            poseState.type = XR_TYPE_ACTION_STATE_POSE;
            if (XR_FAILED(xrGetActionStatePose(g_xrSession, &info, &poseState)) ||
                !poseState.isActive) return;
            XrSpaceLocation location{};
            location.type = XR_TYPE_SPACE_LOCATION;
            if (XR_SUCCEEDED(xrLocateSpace(space, g_xrReferenceSpace,
                predictedDisplayTime, &location)) &&
                (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
            {
                pose = location.pose;
                valid = true;
            }
        };

    const XrPath hands[2] = { g_xrLeftHandPath, g_xrRightHandPath };
    const XrAction grips[2] = { g_xrLeftHandPoseAction, g_xrRightHandPoseAction };
    const XrAction aims[2] = { g_xrLeftAimPoseAction, g_xrRightAimPoseAction };
    const XrSpace gripSpaces[2] = { g_xrLeftHandSpace, g_xrRightHandSpace };
    const XrSpace aimSpaces[2] = { g_xrLeftAimSpace, g_xrRightAimSpace };
    for (int handIndex = 0; handIndex < 2; ++handIndex)
    {
        const XrPath hand = hands[handIndex];
        VrControllerHandState& state = g_xrControllerHands[handIndex];
        const VrControllerHandState previousState = state;
        state = {};
        state.thumbstick = readStick(hand);
        state.trigger = readFloat(g_xrTriggerAction, hand);
        state.squeeze = readFloat(g_xrGripAction, hand);
        state.thumbstickClick = readButton(g_xrThumbstickClickAction, hand);
        if(handIndex==1 && g_xrRightThumbrestAction!=XR_NULL_HANDLE) {
            XrActionStateGetInfo restInfo{};
            restInfo.type=XR_TYPE_ACTION_STATE_GET_INFO;
            restInfo.action=g_xrRightThumbrestAction;restInfo.subactionPath=hand;
            XrActionStateBoolean restState{};restState.type=XR_TYPE_ACTION_STATE_BOOLEAN;
            if(XR_SUCCEEDED(xrGetActionStateBoolean(g_xrSession,&restInfo,&restState))) {
                state.thumbrestAvailable=restState.isActive==XR_TRUE;
                state.thumbrestTouch=state.thumbrestAvailable && restState.currentState==XR_TRUE;
            }
        }
        state.primaryButton = readButton(handIndex == 0 ?
            g_xrXButtonAction : g_xrAButtonAction, hand);
        state.secondaryButton = readButton(handIndex == 0 ?
            g_xrYButtonAction : g_xrBButtonAction, hand);
        if (handIndex == 0)
            state.menuButton = readButton(g_xrLeftMenuAction, hand);
        readPose(grips[handIndex], hand, gripSpaces[handIndex],
            state.gripPoseValid, state.gripPose);
        readPose(aims[handIndex], hand, aimSpaces[handIndex],
            state.aimPoseValid, state.aimPose);

        // Report input transitions immediately; brief presses should not be
        // missed between the periodic snapshots below. This is diagnostic
        // only and does not send any events into Barony's gameplay system.
        const char* handName = handIndex == 0 ? "LEFT" : "RIGHT";
        auto logButtonChange = [&](const char* buttonName, bool before, bool after)
            {
                if (before != after)
                {
                    std::printf("OpenXR INPUT %s %s: %s\n", handName,
                        buttonName, after ? "PRESSED" : "RELEASED");
                }
            };
        logButtonChange("FACE_PRIMARY", previousState.primaryButton, state.primaryButton);
        logButtonChange("FACE_SECONDARY", previousState.secondaryButton, state.secondaryButton);
        logButtonChange("STICK_CLICK", previousState.thumbstickClick, state.thumbstickClick);
        if(handIndex==1) {
            static int reportedThumbrest=-1;
            if(reportedThumbrest!=(state.thumbrestAvailable?1:0)) {
                reportedThumbrest=state.thumbrestAvailable?1:0;
                printlog("[VR GA21] thumbrest action available=%d",reportedThumbrest);
            }
            logButtonChange("THUMBREST_AVAILABLE",previousState.thumbrestAvailable,state.thumbrestAvailable);
            logButtonChange("THUMBREST_TOUCH",previousState.thumbrestTouch,state.thumbrestTouch);
        }
        if (handIndex == 0)
        {
            logButtonChange("MENU", previousState.menuButton, state.menuButton);
        }
        logButtonChange("GRIP_TRACKING", previousState.gripPoseValid, state.gripPoseValid);
        logButtonChange("AIM_TRACKING", previousState.aimPoseValid, state.aimPoseValid);

        // Report the trigger/squeeze transition through the halfway point.
        const bool triggerWasDown = previousState.trigger >= 0.5f;
        const bool triggerIsDown = state.trigger >= 0.5f;
        if (triggerWasDown != triggerIsDown)
        {
            std::printf("OpenXR INPUT %s TRIGGER: %s (value=%.2f)\n",
                handName, triggerIsDown ? "SQUEEZED" : "RELEASED", state.trigger);
        }
        const bool squeezeWasDown = previousState.squeeze >= 0.5f;
        const bool squeezeIsDown = state.squeeze >= 0.5f;
        if (squeezeWasDown != squeezeIsDown)
        {
            std::printf("OpenXR INPUT %s GRIP: %s (value=%.2f)\n",
                handName, squeezeIsDown ? "SQUEEZED" : "RELEASED", state.squeeze);
        }

        // Crossing the thumbstick half-way mark in either direction is enough
        // to confirm that both X and Y input paths work without flooding logs.
        auto stickZone = [](float value) -> int
            {
                return value > 0.5f ? 1 : (value < -0.5f ? -1 : 0);
            };
        const int oldX = stickZone(previousState.thumbstick.x);
        const int oldY = stickZone(previousState.thumbstick.y);
        const int newX = stickZone(state.thumbstick.x);
        const int newY = stickZone(state.thumbstick.y);
        if (oldX != newX || oldY != newY)
        {
            std::printf("OpenXR INPUT %s STICK: x=%.2f y=%.2f\n",
                handName, state.thumbstick.x, state.thumbstick.y);
        }
    }

    vrUpdateVisualArmCalibration(predictedDisplayTime);
    vrUpdateBowPreview();

    // STEP 614: The actual Skills page is MODULE_SKILLS_LIST, not
    // MODULE_CHARACTERSHEET. Cycle X through Inventory -> Character -> Skills
    // -> Inventory. Y toggles Spells, while all other menus use laser clicks.
    // This API is the game's normal status-screen UI, not a pretend key tap.
    const bool uiXDown = g_xrControllerHands[0].primaryButton &&
        !g_xrControllerHands[0].secondaryButton;
    const bool uiYDown = g_xrControllerHands[0].secondaryButton &&
        !g_xrControllerHands[0].primaryButton;
    const bool canSwitchTabs = !g_vrArmCalibrationActive &&
        g_xrSessionState == XR_SESSION_STATE_FOCUSED &&
        !intro && !gamePaused &&
        clientnum >= 0 && clientnum < MAXPLAYERS &&
        players[clientnum] && !players[clientnum]->shootmode &&
        players[clientnum]->gui_mode == GUI_MODE_INVENTORY &&
        !players[clientnum]->usingCommand();
    if (canSwitchTabs && uiXDown && !g_xrVrUiXWasDown)
    {
        Player* p = players[clientnum];
        Player::GUI_t::GUIModules next = Player::GUI_t::MODULE_CHARACTERSHEET;
        if (p->GUI.activeModule == Player::GUI_t::MODULE_CHARACTERSHEET)
            next = Player::GUI_t::MODULE_SKILLS_LIST;
        else if (p->GUI.activeModule == Player::GUI_t::MODULE_SKILLS_LIST)
            next = Player::GUI_t::MODULE_INVENTORY;
        p->openStatusScreen(GUI_MODE_INVENTORY, INVENTORY_MODE_ITEM, next);
        std::printf("OpenXR VR: STEP614 X -> UI module %d\n",
            static_cast<int>(next));
    }
    if (canSwitchTabs && uiYDown && !g_xrVrUiYWasDown)
    {
        Player* p = players[clientnum];
        const bool alreadySpells =
            p->GUI.activeModule == Player::GUI_t::MODULE_SPELLS;
        p->openStatusScreen(GUI_MODE_INVENTORY,
            alreadySpells ? INVENTORY_MODE_ITEM : INVENTORY_MODE_SPELL,
            alreadySpells ? Player::GUI_t::MODULE_INVENTORY :
                Player::GUI_t::MODULE_SPELLS);
        std::printf("OpenXR VR: STEP614 Y -> %s tab\n",
            alreadySpells ? "INVENTORY" : "SPELLS");
    }
    g_xrVrUiXWasDown = uiXDown;
    g_xrVrUiYWasDown = uiYDown;

    // Explicit thumbrest D-pad mode yields pointer ownership to native UI.
    vrUpdateMenuNavigation(!g_vrArmCalibrationActive &&
        !(g_xrControllerHands[1].thumbrestAvailable && g_xrControllerHands[1].thumbrestTouch));
    if (g_vrArmCalibrationActive)
    {
        vrResetPointer();
        // Avoid a false backpack-grip edge when leaving calibration.
        g_xrVrBackpackGripWasDown = g_xrControllerHands[1].squeeze >= .65f;
    }
    else
    {
        vrUpdateVRShortcutGestures(predictedDisplayTime);
        vrUpdateGuiLaserPointer(predictedDisplayTime);
    }

    // Pass analog controls and buttons to the SDL virtual gamepad.
    vrUpdateVirtualSDLGamepad();

    // Periodic OpenXR diagnostic snapshot, in addition to live SDL input.
    static unsigned inputFrames = 0;
    if (++inputFrames % 240 == 0)
    {
        for (int i = 0; i < 2; ++i)
        {
            const VrControllerHandState& h = g_xrControllerHands[i];
            std::printf("OpenXR CTRL %s: gripPose=%d aimPose=%d xyz=(%.2f %.2f %.2f) stick=(%.2f %.2f) trigger=%.2f squeeze=%.2f face=%d/%d stickClick=%d menu=%d\n",
                i == 0 ? "LEFT" : "RIGHT", h.gripPoseValid ? 1 : 0,
                h.aimPoseValid ? 1 : 0,
                h.gripPose.position.x, h.gripPose.position.y, h.gripPose.position.z,
                h.thumbstick.x, h.thumbstick.y, h.trigger, h.squeeze,
                h.primaryButton ? 1 : 0, h.secondaryButton ? 1 : 0,
                h.thumbstickClick ? 1 : 0, h.menuButton ? 1 : 0);
        }
    }
}

// Store OpenXR frame information until GUI rendering is complete.
static bool g_xrFramePending = false;
static XrTime g_xrPendingDisplayTime = 0;
static bool g_xrPendingShouldRender = false;

void vrProcessOpenXRFrame(view_t* camera)
{
    static bool frameFunctionLogged = false;

    if (!frameFunctionLogged)
    {
        SDL_Log("OpenXR DEBUG: vrProcessOpenXRFrame was called!");
        frameFunctionLogged = true;
    }
    if (!g_xrSessionRunning || g_xrSession == XR_NULL_HANDLE)
    {
        return;
    }
    // Do not begin another OpenXR frame until the previous one is finalized.
    if (g_xrFramePending)
    {
        SDL_Log(
            "OpenXR WARNING: Previous frame has not been finalized."
        );
        return;
    }
    XrFrameWaitInfo waitInfo{};
    waitInfo.type = XR_TYPE_FRAME_WAIT_INFO;

    XrFrameState frameState{};
    frameState.type = XR_TYPE_FRAME_STATE;

    XrResult result = xrWaitFrame(
        g_xrSession,
        &waitInfo,
        &frameState
    );

    if (XR_FAILED(result))
    {
        std::printf("OpenXR: xrWaitFrame failed (%d)\n",
            static_cast<int>(result));
        return;
    }

    XrFrameBeginInfo beginInfo{};
    beginInfo.type = XR_TYPE_FRAME_BEGIN_INFO;

    result = xrBeginFrame(g_xrSession, &beginInfo);

    if (XR_FAILED(result))
    {
        std::printf("OpenXR: xrBeginFrame failed (%d)\n",
            static_cast<int>(result));
        return;
    }
    // Preserve frame timing for deferred OpenXR submission.
    g_xrFramePending = true;
    g_xrPendingDisplayTime = frameState.predictedDisplayTime;
    g_xrPendingShouldRender = (frameState.shouldRender == XR_TRUE);
    // Synchronize buttons/sticks/triggers and sample both hands at frame time.
    vrUpdateControllerInputs(frameState.predictedDisplayTime);
    // Retrieve headset tracking for both eyes at the predicted display time.
    vrLocateEyeViews(frameState.predictedDisplayTime);
    // STEP 629: Both controller and headset poses now share this frame time.
    vrProcessPhysicalMelee(frameState.predictedDisplayTime);
    vrUpdateCustomCrosshairAim(frameState.predictedDisplayTime);
    vrSampleHeadsetForLocomotion(frameState.predictedDisplayTime);
    // Send the room-scale movement sample through the SAME SDL input bridge.
    vrUpdateVirtualSDLGamepad();
    // Extract horizontal headset rotation from the tracked eye orientation.
    if (g_xrEyeViewsValid)
    {
        const XrQuaternionf& q = g_xrViews[0].pose.orientation;

        vrHeadYaw = atan2f(
            2.0f * (q.w * q.y + q.x * q.z),
            1.0f - 2.0f * (q.y * q.y + q.x * q.x)
        );
        // Extract vertical headset rotation (pitch).
        const float pitchSin = 2.0f * (q.w * q.x - q.y * q.z);
        vrHeadPitch = asinf(
            fmaxf(-1.0f, fminf(1.0f, pitchSin))
        );
        static int yawLogCounter = 0;
        if (++yawLogCounter >= 120)
        {
            yawLogCounter = 0;
            SDL_Log("OpenXR HEAD YAW: %.3f radians", vrHeadYaw);
        }
    }
    else
    {
        vrHeadYaw = 0.0f;
        vrHeadPitch = 0.0f;
    }
    // Configure the stereo projection layer for this frame.
    g_xrProjectionLayer.space = g_xrReferenceSpace;
    g_xrProjectionLayer.viewCount = 2;
    g_xrProjectionLayer.views = g_xrProjectionViews;
    // Treat the stereo projection layer as fully opaque.
    g_xrProjectionLayer.layerFlags = 0;
    // Step 609: menu-only frames have no dungeon camera.
    g_xrStereoFrameReady = false;
    if (frameState.shouldRender == XR_TRUE && camera != nullptr)
    {
        vrRenderStereoTestFrame(camera);
    }
    // Test the swapchain image lifecycle once.
    static bool swapchainAcquisitionTested = false;

    if (!swapchainAcquisitionTested && g_xrSwapchain != XR_NULL_HANDLE)
    {
        swapchainAcquisitionTested = true;
        vrTestAcquireSwapchainImage();
    }
}
// STEP606: A persistent VR GUI help panel, redrawn every frame while editing.
// It is composited into the existing XR quad GUI layer. Unlike messagePlayer,
// nothing here expires or scrolls away as chat/history messages arrive.
static void vrDrawEquipmentCalibrationHelp()
{
    if (!g_vrArmCalibrationActive || !g_xrGuiFramebuffer) return;
    TTF_Font* font = ttf16 ? ttf16 : (ttf12 ? ttf12 : ttf8);
    if (!font) return;

    static GLuint helpTexture = 0;
    static GLuint helpFramebuffer = 0;
    static Uint32 lastUpload = 0;
    static int previousHand = -1;
    static int previousItemType = -999;
    static bool previousMode = false;
    static bool previousAimMode = false;
    static bool previousSpellAimMode = false;
    static bool previousWearableMode = false;
    static bool previousMeleeMode = false;
    static bool previousFlailMode = false;
    static bool previousBowMode = false, previousBowGripMode = false;
    static bool previousBodyMode = false, previousStatusMode = false;
    static int previousSpatialPage = -2;
    const Item* selected = vrCalibrationEquippedItem(g_vrArmCalibrationSelected);
    const int selectedItemType = selected ? static_cast<int>(selected->type) : -1;
    const Uint32 now = SDL_GetTicks();
    const int W = 1180, H = 432;
    const bool updateText = !helpTexture || !helpFramebuffer ||
        now - lastUpload >= 180u ||
        previousHand != g_vrArmCalibrationSelected ||
        previousItemType != selectedItemType ||
        previousMode != g_vrArmCalibrationEditItem ||
        previousAimMode != g_vrArmCalibrationEditAim ||
        previousSpellAimMode != g_vrArmCalibrationEditSpellAim ||
        previousWearableMode != g_vrArmCalibrationEditWearable ||
        previousMeleeMode != g_vrArmCalibrationEditMelee ||
        previousFlailMode != g_vrArmCalibrationEditFlail ||
        previousBowGripMode != g_vrBowEditGrip ||
        previousBowMode != g_vrArmCalibrationEditBow ||
        previousBodyMode != g_vrBodyCalibrationPage ||
        previousStatusMode != g_vrArmCalibrationEditStatusEffects ||
        previousSpatialPage != g_vrArmCalibrationSpatialPage;

    GLint originalReadFbo = 0, originalDrawFbo = 0;
    GLint originalActiveTexture = 0, originalTexture = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &originalReadFbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &originalDrawFbo);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &originalActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &originalTexture);

    if (updateText)
    {
        SDL_Surface* panel = SDL_CreateRGBSurfaceWithFormat(
            0, W, H, 32, SDL_PIXELFORMAT_RGBA32);
        if (panel)
        {
            SDL_FillRect(panel, nullptr,
                SDL_MapRGBA(panel->format, 12, 22, 32, 255));
            SDL_Color heading{ 110, 222, 255, 255 };
            SDL_Color white{ 242, 247, 251, 255 };
            SDL_Color grey{ 180, 201, 212, 255 };
            // Existing SDL panel primitives; no new artwork or asset files.
            const auto fill = [&](int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b)
            {
                SDL_Rect rect{x, y, w, h};
                SDL_FillRect(panel, &rect, SDL_MapRGBA(panel->format, r, g, b, 255));
            };
            fill(0, 0, W, 40, 22, 48, 62);
            fill(12, 112, W - 24, 108, 20, 34, 45);
            fill(12, 244, W - 24, 132, 17, 31, 41);
            fill(12, 396, W - 24, 2, 68, 116, 136);
            const auto drawLine = [&](const char* str, int x, int y, SDL_Color color)
            {
                SDL_Surface* line = TTF_RenderUTF8_Blended(font, str, color);
                if (!line) return;
                const int available = W - x - 20;
                if (line->w > available || line->h > 24)
                {
                    const float fit = std::min(static_cast<float>(available) / line->w,
                        24.f / line->h);
                    SDL_Rect place{x, y, std::max(1, static_cast<int>(line->w * fit)),
                        std::max(1, static_cast<int>(line->h * fit))};
                    SDL_BlitScaled(line, nullptr, panel, &place);
                }
                else
                {
                    SDL_Rect place{x, y, 0, 0};
                    SDL_BlitSurface(line, nullptr, panel, &place);
                }
                SDL_FreeSurface(line);
            };
            if (g_vrArmCalibrationEditBow) {
                if(g_vrBowEditGrip) {
                    drawLine("BARONY VR / BOW GRIP / POSITION & ROTATION",20,12,heading);
                    drawLine("Right-stick click: return to draw test | Hold X+Y: save/close",20,44,white);
                    drawLine("Each bow hand has its own saved grip. Other equipment offsets are unchanged.",20,70,grey);
                    drawLine("ADJUST CONTROLS",20,94,grey);
                    drawLine("LEFT stick: turn / tilt around HAND axes | LEFT grip + stick sideways: twist",20,123,white);
                    drawLine("Right stick: side/up | Right grip + right stick up/down: depth",20,154,white);
                    drawLine("Hold right trigger 1.5s: reset this bow hand only | Drawing paused while editing",20,185,white);
                    drawLine("CURRENT VALUES",20,225,grey);
                    const auto& g=g_vrBowGrip[1-g_vrBowDrawHand];char line[240]{};
                    sprintf_s(line,sizeof(line),"%s BOW HAND: Side %+.1f cm | Up %+.1f cm | Depth %+.1f cm",
                        g_vrBowDrawHand?"LEFT":"RIGHT",g.side*100.,g.up*100.,g.forward*100.);
                    drawLine(line,20,251,heading);
                    sprintf_s(line,sizeof(line),"Controller-relative turn, tilt and twist | Saved separately for each bow hand");
                    drawLine(line,20,282,white);
                    drawLine("Keep your bow hand steady; turn/tilt/twist the bow, then click right stick to test.",20,313,grey);
                    drawLine("SAVE & CLOSE: hold X+Y | Saved bow grip is also used in gameplay",20,405,white);
                } else {
                drawLine("BARONY VR  /  BOW SETUP  /  SHORTBOW DRAW TEST",20,12,heading);
                drawLine("Left-stick click: Next | Left grip + click: Previous | Hold X+Y: save/close",20,44,white);
                drawLine("Original shortbow and arrow art | Calibration preview only - no firing",20,70,grey);
                drawLine("CONTROLS",20,94,grey);
                drawLine("Right stick left/right: choose drawing hand | Up/down: full-draw distance",20,123,white);
                drawLine("Bring drawing hand near the string, squeeze its GRIP, then pull back",20,154,white);
                drawLine("Release that GRIP to test release | Tracking loss or leaving this page cancels",20,185,white);
                drawLine("CURRENT VALUES",20,225,grey);
                char info[240]{};
                sprintf_s(info,sizeof(info),"%s-HANDED: bow in %s hand, draw with %s hand | Full draw %.0f cm",
                    g_vrBowDrawHand?"RIGHT":"LEFT",g_vrBowDrawHand?"LEFT":"RIGHT",
                    g_vrBowDrawHand?"RIGHT":"LEFT",static_cast<double>(g_vrBowFullDraw*100.f));
                drawLine(info,20,251,heading);
                sprintf_s(info,sizeof(info),"Draw %.0f / %.0f cm | %s | Test releases %u",
                    static_cast<double>(g_vrBowPreview.draw*100.f),static_cast<double>(g_vrBowFullDraw*100.f),
                    g_vrBowPreview.drawing?"STRING HELD":"READY",g_vrBowPreview.releases);
                drawLine(info,20,282,white);
                drawLine(vrBowPreviewSelected()?"Gameplay: GRIP grabs string; release fires. Draw-hand TRIGGER casts when not drawing.":
                    "Equip a SHORTBOW to see the preview. Other bow types come after this alignment test.",20,313,heading);
                drawLine("Right-stick click: edit bow grip position/rotation. No shots or ammo consumed.",20,344,grey);
                drawLine("SAVE & CLOSE: hold X+Y | Your weapon, HUD and combat settings are retained",20,405,white);
                }
            } else {
            const auto& c = vrCalibrationSelectedValues();
            const auto& hud = g_vrArmCalibrationSpatialPage >= 0 ?
                g_vrSpatialHud[g_vrArmCalibrationSpatialPage] :
                (g_vrArmCalibrationEditStatusEffects ?
                g_vrStatusEffectsHud : g_vrWearableHud[g_vrArmCalibrationSelected]);
            char selectedText[240]{};
            if (g_vrBodyCalibrationPage)
                sprintf_s(selectedText, sizeof(selectedText),
                    "BARONY VR CALIBRATION  |  BODY FIT  |  TRACKED HAND ANCHORS");
            else if (g_vrArmCalibrationEditFlail)
                sprintf_s(selectedText, sizeof(selectedText),
                    "BARONY VR  /  FLAIL  /  LIVE CHAIN PIVOT (COSMETIC)");
            else if (g_vrArmCalibrationEditMelee)
                sprintf_s(selectedText, sizeof(selectedText),
                    "BARONY VR CALIBRATION  |  RIGHT MELEE (DRY RUN; NO DAMAGE)");
            else if (g_vrArmCalibrationSpatialPage >= 0)
                sprintf_s(selectedText, sizeof(selectedText),
                    "BARONY VR  /  HUD LAYOUT  /  %s",
                    vrSpatialHudName(g_vrArmCalibrationSpatialPage));
            else if (g_vrArmCalibrationEditStatusEffects)
                sprintf_s(selectedText, sizeof(selectedText),
                    "BARONY VR CALIBRATION  |  LEFT STATUS EFFECTS (INDEPENDENT)");
            else if (g_vrArmCalibrationEditWearable)
                sprintf_s(selectedText, sizeof(selectedText),
                    "BARONY VR CALIBRATION  |  %s ORIGINAL HUD",
                    g_vrArmCalibrationSelected ? "RIGHT MAP WATCH" : "LEFT HP/MP FOREARM");
            else if (g_vrArmCalibrationEditSpellAim)
                sprintf_s(selectedText, sizeof(selectedText),
                    "BARONY VR CALIBRATION  |  LEFT SPELL PROJECTILE AIM");
            else if (g_vrArmCalibrationEditAim)
                sprintf_s(selectedText, sizeof(selectedText),
                    "BARONY VR  /  WEAPON AIM  /  %s",
                    selected ? selected->getName() : "No equipment");
            else if (g_vrArmCalibrationEditItem)
                sprintf_s(selectedText, sizeof(selectedText),
                    "BARONY VR  /  %s EQUIPMENT  /  %s",
                    g_vrArmCalibrationSelected ? "RIGHT" : "LEFT",
                    selected ? selected->getName() : "No equipment");
            else
                sprintf_s(selectedText, sizeof(selectedText),
                    "BARONY VR CALIBRATION  |  %s ARM",
                    g_vrArmCalibrationSelected ? "RIGHT" : "LEFT");
            drawLine(selectedText, 20, 12, heading);
            if (g_vrBodyCalibrationPage)
                drawLine("BODY FIT: A+B held 0.7s = CLOSE THIS PAGE | X+Y = SAVE/CLOSE CAL", 20, 44, white);
            else if (g_vrArmCalibrationEditMelee)
                drawLine("MELEE only  |  Hold BOTH GRIPS for 3 seconds = SAVE & EXIT TO GAME", 20, 44, white);
            else
                drawLine("A: Right hand   B: Left hand   |   Left-stick click: Next   |   Left grip + click: Previous", 20, 44, white);
            if (g_vrBodyCalibrationPage)
            {
                drawLine("1. Recenter with Meta/SteamVR normally. Face forward and stand upright.", 20, 123, white);
                drawLine("2. T-POSE: arms straight sideways at shoulder height, hold BOTH GRIPS 2s.", 20, 154, white);
            }
            else if (g_vrArmCalibrationEditFlail)
            {
                drawLine("RIGHT stick: LEFT/RIGHT = anchor side | UP/DOWN = anchor height", 20, 123, white);
                drawLine("Hold RIGHT GRIP + RIGHT Y = anchor forward/back", 20, 154, white);
            }
            else if (g_vrArmCalibrationEditMelee)
            {
                drawLine("LEFT stick: sideways = required TIP speed; up/down = required HAND speed", 20, 123, white);
                drawLine("RIGHT stick: sideways = recovery/rearm speed  |  Reach & damage unchanged", 20, 154, white);
            }
            else if (g_vrArmCalibrationEditWearable || g_vrArmCalibrationEditStatusEffects ||
                g_vrArmCalibrationSpatialPage >= 0)
            {
                drawLine("LEFT stick: yaw/pitch | LEFT grip + LEFT stick sideways: roll", 20, 123, white);
                drawLine("RIGHT stick: side/up | RIGHT grip + RIGHT stick: size/depth", 20, 154, white);
            }
            else if (g_vrArmCalibrationEditSpellAim)
            {
                drawLine("LEFT stick: adjust spell projectile yaw / pitch (up, down, left, right)", 20, 123, white);
                drawLine("LEFT controller drives BOTH the aiming guide and spell missile", 20, 154, white);
            }
            else if (g_vrArmCalibrationEditAim)
            {
                drawLine("LEFT stick: adjust horizontal/vertical crosshair direction", 20, 123, white);
                drawLine("Reticle AND thrown/arrow launch share this per-item aim offset", 20, 154, white);
            }
            else
            {
                drawLine("LEFT stick: yaw / pitch  |  LEFT grip + LEFT stick sideways: roll", 20, 123, white);
                if (!g_vrArmCalibrationEditItem)
                    drawLine(g_vrArmCalibrationSelected == 0 ?
                        "RIGHT stick: side/up | LEFT grip + RIGHT stick vertical: SIZE | CLICK: mesh" :
                        "RIGHT stick: side/up | LEFT grip + RIGHT stick vertical: RIGHT ARM SIZE",20,154,white);
                else
                    drawLine("RIGHT stick: side / up  |  RIGHT grip + RIGHT stick up/down: depth", 20, 154, white);
            }
            if (g_vrBodyCalibrationPage)
            {
                char holdLine[160]{};
                sprintf_s(holdLine, sizeof(holdLine),
                    "T-POSE HOLD %.1f/2.0 sec (%d samples) | RIGHT TRIGGER 1.5s resets",
                    g_vrBodyPoseStarted ?
                      static_cast<double>(std::min(2.f,
                        static_cast<float>(now - g_vrBodyPoseStarted) / 1000.f)) : 0.0,
                    g_vrBodyPoseSamples);
                drawLine(holdLine, 20, 185, heading);
            }
            else if (g_vrMeleeGripChordStartedAt && !g_vrMeleeGripChordLatched)
            {
                char holdLine[160]{};
                const float held = std::min(3.0f,
                    static_cast<float>(now - g_vrMeleeGripChordStartedAt) / 1000.f);
                sprintf_s(holdLine, sizeof(holdLine),
                    "BOTH GRIPS HELD: %.1f / 3.0 seconds",
                    static_cast<double>(held));
                drawLine(holdLine, 20, 185, heading);
            }
            else if (g_vrArmCalibrationEditFlail)
                drawLine("LEFT X: length | LEFT GRIP + X: 3D MODEL SIZE | LEFT Y: damping", 20, 185, white);
            else if (g_vrArmCalibrationEditMelee)
                drawLine("A = SPAWN TEST PROP (offline) | Trigger 1.5s resets | Grips save & exit", 20, 185, white);
            else
                drawLine("RESET: Hold right trigger 1.5s  |  SAVE / CLOSE: Hold X+Y  |  MELEE: Hold both grips", 20, 185, white);
            char valueLine[250]{};
            if (g_vrBodyCalibrationPage)
            {
                vrLoadBodyFit();
                sprintf_s(valueLine, sizeof(valueLine),
                    "PROFILE: %s  LEFT REACH %.2fm  RIGHT REACH %.2fm",
                    g_vrBodyFit.enabled ? "ACTIVE" : "OFF",
                    static_cast<double>(g_vrBodyFit.measuredReachMeters[0]),
                    static_cast<double>(g_vrBodyFit.measuredReachMeters[1]));
                drawLine(valueLine, 20, 251, heading);
                sprintf_s(valueLine, sizeof(valueLine),
                    "HAND MODEL SCALE: L %.0f%% R %.0f%% | HAND ANCHOR auto aligned",
                    static_cast<double>(g_vrBodyFit.handScale[0] * 100.f),
                    static_cast<double>(g_vrBodyFit.handScale[1] * 100.f));
                drawLine(valueLine, 20, 282, grey);
                drawLine("Weapon grip edits kept intact | No gameplay reach or aim changes", 20, 313, heading);
            }
            else if (g_vrArmCalibrationEditFlail)
            {
                sprintf_s(valueLine, sizeof(valueLine),
                    "PIVOT  Forward %+.3f  Up %+.3f  Side %+.3f",
                    static_cast<double>(g_vrFlailAnchorForward),
                    static_cast<double>(g_vrFlailAnchorUp),
                    static_cast<double>(g_vrFlailAnchorSide));
                drawLine(valueLine, 20, 251, heading);
                sprintf_s(valueLine, sizeof(valueLine),
                    "CHAIN  Length %.3f  Damping %.2f  MODEL SIZE %.0f%%",
                    static_cast<double>(g_vrFlailChainLength),
                    static_cast<double>(g_vrFlailDamping),
                    static_cast<double>(g_vrFlailModelSize * 100.f));
                drawLine(valueLine, 20, 282, grey);
                drawLine("Glow marker = chain mount. Left-stick click: exit flail | X+Y: save/close", 20, 313, heading);
            }
            else if (g_vrArmCalibrationEditMelee)
            {
                sprintf_s(valueLine, sizeof(valueLine),
                    "SWING    Required TIP %.2f m/s   |   Required HAND %.2f m/s",
                    static_cast<double>(g_vrMeleeTuning.minTipSpeed),
                    static_cast<double>(g_vrMeleeTuning.minGripSpeed));
                drawLine(valueLine, 20, 251, heading);
                sprintf_s(valueLine, sizeof(valueLine),
                    "RECOVERY %.2f m/s   |  RETURN hand ~6cm, then SWING forward",
                    static_cast<double>(g_vrMeleeTuning.rearmSpeed));
                drawLine(valueLine, 20, 282, grey);
                sprintf_s(valueLine, sizeof(valueLine),
                    "TIP %.2f   HAND %.2f m/s  |  %s  |  RETURN %.2f / 0.060m",
                    static_cast<double>(g_vrMeleeLiveTipSpeed),
                    static_cast<double>(g_vrMeleeLiveGripSpeed),
                    g_vrMeleeLiveReady ? "READY" : "RETURNING",
                    static_cast<double>(g_vrMeleeLiveReverseMeters));
                drawLine(valueLine, 20, 313, heading);
                drawLine((g_vrMeleeLastPreviewAt != 0 &&
                    now - g_vrMeleeLastPreviewAt < 350u) ?
                    "SWING DETECTED (dry run; no damage)" :
                    "A spawns a native breakable. Dry-run here; EXIT to damage it", 20, 344, white);
            }
            else if (g_vrArmCalibrationEditWearable || g_vrArmCalibrationEditStatusEffects ||
                g_vrArmCalibrationSpatialPage >= 0)
            {
                sprintf_s(valueLine, sizeof(valueLine),
                    "HUD POSITION  Side %+.2f  Up %+.2f  Depth %+.2f  Size %.2f",
                    static_cast<double>(hud.side), static_cast<double>(hud.up),
                    static_cast<double>(hud.forward), static_cast<double>(hud.scale));
                drawLine(valueLine, 20, 251, heading);
                sprintf_s(valueLine, sizeof(valueLine),
                    "HUD ROTATION  Yaw %+.1f   Pitch %+.1f   Roll %+.1f deg",
                    static_cast<double>(hud.yaw * 180.f / static_cast<float>(PI)),
                    static_cast<double>(hud.pitch * 180.f / static_cast<float>(PI)),
                    static_cast<double>(hud.roll * 180.f / static_cast<float>(PI)));
                drawLine(valueLine, 20, 282, grey);
                if (g_vrArmCalibrationSpatialPage == 3)
                    drawLine("XP BAR follows BELT | adjustments are relative to belt", 20, 313, white);
                else if (g_vrArmCalibrationSpatialPage == 5)
                {
                    drawLine("SKILL-UP follows XP | position, rotation and size are independent", 20, 313, white);
                    drawLine("After a real skill-up, this page retains its artwork as a preview", 20, 344, grey);
                }
            }
            else if (!g_vrArmCalibrationEditAim && !g_vrArmCalibrationEditSpellAim)
            {
                sprintf_s(valueLine, sizeof(valueLine),
                    "POSITION   Side: %+.2f    Up: %+.2f    Forward: %+.2f (game units)",
                    static_cast<double>(c.side), static_cast<double>(c.up),
                    static_cast<double>(c.forward));
                drawLine(valueLine, 20, 251, grey);
                sprintf_s(valueLine, sizeof(valueLine),
                    "ROTATION   Yaw: %+.1f   Pitch: %+.1f   Roll: %+.1f degrees",
                    static_cast<double>(c.yaw * 180.f / static_cast<float>(PI)),
                    static_cast<double>(c.pitch * 180.f / static_cast<float>(PI)),
                    static_cast<double>(c.roll * 180.f / static_cast<float>(PI)));
                drawLine(valueLine, 20, 282, grey);
            }
            if (g_vrBodyCalibrationPage)
            {
                // Above: body reach, model scale and reset instructions.
            }
            else if (g_vrArmCalibrationEditSpellAim)
            {
                sprintf_s(valueLine, sizeof(valueLine),
                    "SPELL AIM  Horizontal: %+.2f deg    Vertical: %+.2f deg",
                    static_cast<double>(g_vrSpellAimVisual.yaw * 180.f / static_cast<float>(PI)),
                    static_cast<double>(g_vrSpellAimVisual.pitch * 180.f / static_cast<float>(PI)));
                drawLine(valueLine, 20, 251, heading);
                drawLine("Yaw range +/-49 deg | Pitch +/-20 deg | RIGHT trigger 1.5s resets", 20, 313, heading);
            }
            else if (g_vrArmCalibrationEditAim)
            {
                const auto aim = vrCalibrationAimValues();
                sprintf_s(valueLine, sizeof(valueLine),
                    "AIM DIRECTION  Horizontal: %+.2f deg    Vertical: %+.2f deg",
                    static_cast<double>(aim.yaw * 180.f / static_cast<float>(PI)),
                    static_cast<double>(aim.pitch * 180.f / static_cast<float>(PI)));
                drawLine(valueLine, 20, 251, heading);
                drawLine("Aim calibration is per ItemType | Thrown arc still follows gravity", 20, 313, heading);
            }
            else if (g_vrArmCalibrationEditMelee)
            {
                // Live measurements occupy all three value rows above.
            }
            else if (!g_vrBodyCalibrationPage && !g_vrArmCalibrationEditFlail &&
                g_vrArmCalibrationSelected == 0 && !g_vrArmCalibrationEditItem &&
                !g_vrArmCalibrationEditWearable && !g_vrArmCalibrationEditStatusEffects &&
                g_vrArmCalibrationSpatialPage < 0)
            {
                sprintf_s(valueLine, sizeof(valueLine),
                    "ARM MODEL: %s   |   SIZE %.2fx   |   Right-stick click: change model",
                    vrLeftArmMeshName(),
                    static_cast<double>(g_vrLeftArmMeshMode == 0 ?
                        g_vrLeftArmScaleFactor :
                        g_vrLeftArmScaleFactor / VR_DEFAULT_LEFT_ARM_SCALE_FACTOR));
                drawLine(valueLine, 20, 313, heading);
            }
            else if (!g_vrBodyCalibrationPage && !g_vrArmCalibrationEditItem &&
                !g_vrArmCalibrationEditFlail && !g_vrArmCalibrationEditWearable &&
                !g_vrArmCalibrationEditStatusEffects && g_vrArmCalibrationSpatialPage < 0)
            {
                sprintf_s(valueLine,sizeof(valueLine),"RIGHT ARM SIZE %.2fx | LEFT grip + RIGHT stick up/down: size",g_vrRightArmScaleFactor);
                drawLine(valueLine,20,313,heading);
            }
            else if (!g_vrArmCalibrationEditFlail && !g_vrArmCalibrationEditWearable &&
                !g_vrArmCalibrationEditStatusEffects && g_vrArmCalibrationSpatialPage < 0)
                drawLine("Saved separately for each arm and equipment type | Gameplay inputs paused", 20, 313, heading);

            drawLine("ADJUST CONTROLS", 20, 94, grey);
            drawLine("CURRENT VALUES", 20, 225, grey);
            if (!g_vrBodyCalibrationPage && !g_vrArmCalibrationEditMelee)
            {
                const auto pages = vrCalibrationPages();
                const auto current = vrCalibrationCurrentPage();
                const auto found = std::find(pages.begin(), pages.end(), current);
                const int count = static_cast<int>(pages.size());
                const int index = found == pages.end() ? -1 : static_cast<int>(found - pages.begin());
                char navigation[240]{};
                if (index >= 0)
                    sprintf_s(navigation, sizeof(navigation), "Previous: %s     [ %s  |  %d / %d ]     Next: %s",
                        vrCalibrationPageName(pages[(index + count - 1) % count]),
                        vrCalibrationPageName(current), index + 1, count,
                        vrCalibrationPageName(pages[(index + 1) % count]));
                else
                    sprintf_s(navigation, sizeof(navigation), "Flail shortcut   |   Left-stick click returns to Arm");
                drawLine(navigation, 20, 70, heading);
            }
            else
                drawLine(g_vrBodyCalibrationPage ? "BODY FIT  /  Pose setup" :
                    "MELEE  /  Live swing test", 20, 70, heading);
            drawLine(g_vrArmCalibrationEditMelee ?
                "SAVE & EXIT: hold both grips 3s   |   RESET THIS PAGE: hold right trigger 1.5s" :
                "SAVE & CLOSE: hold X+Y   |   BODY FIT: hold A+B   |   MELEE: hold both grips 3s",
                20, 405, white);

            }

            // SDL surface rows are top-first; OpenGL texture rows bottom-first.
            std::vector<unsigned char> pixels(static_cast<size_t>(W) * H * 4u);
            for (int y = 0; y < H; ++y)
                std::memcpy(pixels.data() + static_cast<size_t>(H - 1 - y) * W * 4,
                    static_cast<const unsigned char*>(panel->pixels) + y * panel->pitch,
                    static_cast<size_t>(W) * 4);
            SDL_FreeSurface(panel);
            if (!helpTexture) glGenTextures(1, &helpTexture);
            glBindTexture(GL_TEXTURE_2D, helpTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            GLint unpack = 0;
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0,
                GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
            if (!helpFramebuffer) glGenFramebuffers(1, &helpFramebuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, helpFramebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                GL_TEXTURE_2D, helpTexture, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                SDL_Log("OpenXR STEP606: help overlay framebuffer incomplete");
            lastUpload = now;
            previousHand = g_vrArmCalibrationSelected;
            previousItemType = selectedItemType;
            previousMode = g_vrArmCalibrationEditItem;
            previousAimMode = g_vrArmCalibrationEditAim;
            previousSpellAimMode = g_vrArmCalibrationEditSpellAim;
            previousWearableMode = g_vrArmCalibrationEditWearable;
            previousMeleeMode = g_vrArmCalibrationEditMelee;
            previousFlailMode = g_vrArmCalibrationEditFlail;
            previousBowGripMode = g_vrBowEditGrip;
            previousBowMode = g_vrArmCalibrationEditBow;
            previousBodyMode = g_vrBodyCalibrationPage;
            previousStatusMode = g_vrArmCalibrationEditStatusEffects;
            previousSpatialPage = g_vrArmCalibrationSpatialPage;
        }
    }
    if (helpFramebuffer && helpTexture)
    {
        // Copy the opaque panel as pixels; no shader or legacy glBegin needed.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, helpFramebuffer);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_xrGuiFramebuffer);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        const GLboolean scissors = glIsEnabled(GL_SCISSOR_TEST);
        if (scissors) glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, W, H, 50, 268, 50 + W, 268 + H,
            GL_COLOR_BUFFER_BIT, GL_NEAREST);
        if (scissors) glEnable(GL_SCISSOR_TEST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(originalReadFbo));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(originalDrawFbo));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(originalTexture));
    glActiveTexture(static_cast<GLenum>(originalActiveTexture));
}

// Attach a GUI swapchain texture to the dedicated GUI framebuffer.
static bool vrAttachGuiSwapchainImage(uint32_t imageIndex)
{
    if (g_xrGuiFramebuffer == 0 ||
        imageIndex >= g_xrGuiSwapchainImages.size())
    {
        return false;
    }

    glBindFramebuffer(
        GL_FRAMEBUFFER,
        g_xrGuiFramebuffer
    );

    glFramebufferTexture2D(
        GL_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        g_xrGuiSwapchainImages[imageIndex].image,
        0
    );

    glDrawBuffer(GL_COLOR_ATTACHMENT0);

    const GLenum status =
        glCheckFramebufferStatus(GL_FRAMEBUFFER);

    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        SDL_Log(
            "OpenXR: GUI framebuffer incomplete (0x%X)",
            static_cast<unsigned int>(status)
        );

        return false;
    }

    return true;
}
// Copy the vanilla HUD into the VR overlay first, then mask the portions
// successfully reproduced on tracked forearm/wrist surfaces. The original
// Barony GUI texture and desktop output are NEVER cleared.
static void vrMaskOriginalWearableHudOnGui();

// Acquire, prepare, and release a VR GUI swapchain image.
static bool vrProcessGuiSwapchainImage()
{
    if (g_xrGuiSwapchain == XR_NULL_HANDLE ||
        g_xrGuiFramebuffer == 0 ||
        g_xrGuiSwapchainImages.empty())
    {
        return false;
    }
    // Load Barony's original mouse cursor artwork.
    static bool cursorLoadChecked = false;

    if (!cursorLoadChecked)
    {
        cursorLoadChecked = true;

        // Preserve the currently bound OpenGL texture.
        GLint previousCursorTexture = 0;
        glGetIntegerv(
            GL_TEXTURE_BINDING_2D,
            &previousCursorTexture
        );

        Image* cursorImage = Image::get(
            g_xrMenuCursorImagePath
        );
        // Retain the cursor image for subsequent VR GUI rendering.
        g_xrMenuCursorImage = cursorImage;
        // Restore the original texture binding.
        glBindTexture(
            GL_TEXTURE_2D,
            static_cast<GLuint>(previousCursorTexture)
        );

        if (cursorImage &&
            cursorImage->getWidth() > 0 &&
            cursorImage->getHeight() > 0)
        {
            SDL_Log(
                "OpenXR: Barony cursor loaded! Size: %d x %d",
                cursorImage->getWidth(),
                cursorImage->getHeight()
            );
        }
        else
        {
            SDL_Log(
                "OpenXR: Failed to load Barony cursor: %s",
                g_xrMenuCursorImagePath
            );
        }
    }
    XrSwapchainImageAcquireInfo acquireInfo{};
    acquireInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;

    uint32_t imageIndex = 0;

    XrResult result = xrAcquireSwapchainImage(
        g_xrGuiSwapchain,
        &acquireInfo,
        &imageIndex
    );

    if (XR_FAILED(result))
    {
        SDL_Log(
            "OpenXR: GUI image acquisition failed (%d)",
            static_cast<int>(result)
        );
        return false;
    }

    XrSwapchainImageWaitInfo waitInfo{};
    waitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    waitInfo.timeout = XR_INFINITE_DURATION;

    result = xrWaitSwapchainImage(
        g_xrGuiSwapchain,
        &waitInfo
    );

    if (XR_FAILED(result))
    {
        SDL_Log(
            "OpenXR: GUI image wait failed (%d)",
            static_cast<int>(result)
        );

        return false;
    }

    // Preserve Barony's currently bound framebuffer.
    GLint previousFramebuffer = 0;
    glGetIntegerv(
        GL_FRAMEBUFFER_BINDING,
        &previousFramebuffer
    );

    // Attach the acquired GUI texture.
    const bool attached =
        vrAttachGuiSwapchainImage(imageIndex);
    // Copy Barony's existing GUI texture into the VR GUI framebuffer.
    bool guiCopied = false;

    if (attached)
    {
        const GLuint sourceTexture = vrGetGuiTexture();

        if (sourceTexture != 0)
        {
            // Determine the source GUI texture dimensions.
            GLint previousTexture = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

            glBindTexture(GL_TEXTURE_2D, sourceTexture);

            GLint sourceWidth = 0;
            GLint sourceHeight = 0;

            glGetTexLevelParameteriv(
                GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &sourceWidth
            );

            glGetTexLevelParameteriv(
                GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &sourceHeight
            );

            glBindTexture(
                GL_TEXTURE_2D,
                static_cast<GLuint>(previousTexture)
            );

            if (sourceWidth > 0 && sourceHeight > 0)
            {
                // Create a temporary framebuffer for reading the GUI.
                GLuint sourceFramebuffer = 0;
                glGenFramebuffers(1, &sourceFramebuffer);

                if (sourceFramebuffer != 0)
                {
                    glBindFramebuffer(
                        GL_READ_FRAMEBUFFER, sourceFramebuffer
                    );

                    glFramebufferTexture2D(
                        GL_READ_FRAMEBUFFER,
                        GL_COLOR_ATTACHMENT0,
                        GL_TEXTURE_2D,
                        sourceTexture,
                        0
                    );

                    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)
                        == GL_FRAMEBUFFER_COMPLETE)
                    {
                        glReadBuffer(GL_COLOR_ATTACHMENT0);

                        // Direct the copy into the OpenXR GUI image.
                        glBindFramebuffer(
                            GL_DRAW_FRAMEBUFFER, g_xrGuiFramebuffer
                        );

                        glDrawBuffer(GL_COLOR_ATTACHMENT0);

                        const GLboolean scissorEnabled =
                            glIsEnabled(GL_SCISSOR_TEST);

                        glDisable(GL_SCISSOR_TEST);

                        // Scale the existing GUI to the VR panel texture.
                        glBlitFramebuffer(
                            0, 0, sourceWidth, sourceHeight,
                            0, 0, 1280, 720,
                            GL_COLOR_BUFFER_BIT,
                            GL_LINEAR
                        );

                        if (scissorEnabled)
                        {
                            glEnable(GL_SCISSOR_TEST);
                        }

                        guiCopied = true;
                        // STEP638EL (combined): The original HUD data is
                        // still available for wrist/forearm captures; erase
                        // its duplicate ONLY from this OpenXR GUI copy.
                        vrMaskOriginalWearableHudOnGui();
                        // Keep the instructions visible as long as calibration is open.
                        vrDrawEquipmentCalibrationHelp();
                        // Draw Barony's original cursor over the floating VR GUI.
// Show it while the pause menu is active.
                        if ((gamePaused || g_xrVrPointerActive) && g_xrMenuCursorImage)
                        {
                            extern SDL_Window* screen;

                            if (screen)
                            {
                                int mouseX = 0;
                                int mouseY = 0;
                                SDL_GetMouseState(&mouseX, &mouseY);

                                int windowWidth = 0;
                                int windowHeight = 0;
                                SDL_GetWindowSize(
                                    screen, &windowWidth, &windowHeight
                                );

                                if (windowWidth > 0 && windowHeight > 0)
                                {
                                    // Convert desktop mouse coordinates to VR GUI coordinates.
                                    const int cursorX = g_xrVrPointerActive ?
                                        g_xrVrPointerGuiX : mouseX * 1280 / windowWidth;

                                    const int cursorY = g_xrVrPointerActive ?
                                        g_xrVrPointerGuiY : mouseY * 720 / windowHeight;

                                    // Preserve Barony's OpenGL rendering state.
                                    GLint oldViewport[4] = {};
                                    GLint oldProgram = 0;
                                    GLint oldVAO = 0;
                                    GLint oldArrayBuffer = 0;
                                    GLint oldActiveTexture = 0;
                                    GLint oldTexture = 0;

                                    GLint oldBlendSrcRGB = 0;
                                    GLint oldBlendDstRGB = 0;
                                    GLint oldBlendSrcAlpha = 0;
                                    GLint oldBlendDstAlpha = 0;

                                    GLboolean oldColorMask[4] = {};

                                    glGetIntegerv(GL_VIEWPORT, oldViewport);
                                    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
                                    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVAO);
                                    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldArrayBuffer);
                                    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);

                                    glGetIntegerv(GL_BLEND_SRC_RGB, &oldBlendSrcRGB);
                                    glGetIntegerv(GL_BLEND_DST_RGB, &oldBlendDstRGB);
                                    glGetIntegerv(GL_BLEND_SRC_ALPHA, &oldBlendSrcAlpha);
                                    glGetIntegerv(GL_BLEND_DST_ALPHA, &oldBlendDstAlpha);

                                    glGetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);

                                    const GLboolean oldBlend =
                                        glIsEnabled(GL_BLEND);

                                    const GLboolean oldDepth =
                                        glIsEnabled(GL_DEPTH_TEST);

                                    const GLboolean oldScissor =
                                        glIsEnabled(GL_SCISSOR_TEST);

                                    const GLboolean oldCull =
                                        glIsEnabled(GL_CULL_FACE);

                                    glActiveTexture(GL_TEXTURE0);
                                    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);

                                    // Prepare the OpenXR GUI framebuffer for 2D drawing.
                                    glViewport(0, 0, 1280, 720);
                                    glDisable(GL_DEPTH_TEST);
                                    glDisable(GL_SCISSOR_TEST);
                                    glDisable(GL_CULL_FACE);
                                    glEnable(GL_BLEND);
                                    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

                                    glBlendFuncSeparate(
                                        GL_SRC_ALPHA,
                                        GL_ONE_MINUS_SRC_ALPHA,
                                        GL_ONE,
                                        GL_ONE_MINUS_SRC_ALPHA
                                    );

                                    // Display the original 84x84 artwork at 42x42 pixels.
                                    SDL_Rect cursorDest = {
                                        cursorX,
                                        cursorY,
                                        42,
                                        42
                                    };

                                    SDL_Rect guiViewport = {
                                        0,
                                        0,
                                        1280,
                                        720
                                    };

                                    g_xrMenuCursorImage->draw(
                                        nullptr,
                                        cursorDest,
                                        guiViewport
                                    );

                                    // Restore Barony's original OpenGL state.
                                    glUseProgram(static_cast<GLuint>(oldProgram));
                                    glBindVertexArray(static_cast<GLuint>(oldVAO));
                                    glBindBuffer(
                                        GL_ARRAY_BUFFER,
                                        static_cast<GLuint>(oldArrayBuffer)
                                    );

                                    glBindTexture(
                                        GL_TEXTURE_2D,
                                        static_cast<GLuint>(oldTexture)
                                    );

                                    glActiveTexture(
                                        static_cast<GLenum>(oldActiveTexture)
                                    );

                                    glBlendFuncSeparate(
                                        oldBlendSrcRGB,
                                        oldBlendDstRGB,
                                        oldBlendSrcAlpha,
                                        oldBlendDstAlpha
                                    );

                                    glViewport(
                                        oldViewport[0],
                                        oldViewport[1],
                                        oldViewport[2],
                                        oldViewport[3]
                                    );

                                    glColorMask(
                                        oldColorMask[0],
                                        oldColorMask[1],
                                        oldColorMask[2],
                                        oldColorMask[3]
                                    );

                                    if (oldBlend) glEnable(GL_BLEND);
                                    else glDisable(GL_BLEND);

                                    if (oldDepth) glEnable(GL_DEPTH_TEST);
                                    else glDisable(GL_DEPTH_TEST);

                                    if (oldScissor) glEnable(GL_SCISSOR_TEST);
                                    else glDisable(GL_SCISSOR_TEST);

                                    if (oldCull) glEnable(GL_CULL_FACE);
                                    else glDisable(GL_CULL_FACE);
                                }
                            }
                        }
                        // STEP 629: Draw only into the dedicated OpenXR GUI
                        // swapchain image, AFTER the Barony GUI and cursor copy.
                        vrDrawSelectedCrosshairOnGui();
                        static bool guiCopyLogged = false;

                        if (!guiCopyLogged)
                        {
                            SDL_Log(
                                "OpenXR: GUI texture copied into VR framebuffer!"
                            );

                            guiCopyLogged = true;
                        }
                    }

                    // Restore the GUI framebuffer binding.
                    glBindFramebuffer(
                        GL_READ_FRAMEBUFFER, g_xrGuiFramebuffer
                    );

                    glDeleteFramebuffers(1, &sourceFramebuffer);
                }
            }
        }
    }
    // Restore Barony's original framebuffer.
    glBindFramebuffer(
        GL_FRAMEBUFFER,
        static_cast<GLuint>(previousFramebuffer)
    );

    // Release the image back to the OpenXR runtime.
    XrSwapchainImageReleaseInfo releaseInfo{};
    releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;

    result = xrReleaseSwapchainImage(
        g_xrGuiSwapchain,
        &releaseInfo
    );

    if (XR_FAILED(result))
    {
        SDL_Log(
            "OpenXR: GUI image release failed (%d)",
            static_cast<int>(result)
        );
        return false;
    }

    return guiCopied;
}
void vrFinalizeOpenXRFrame()
{
    // Only finalize frames that were successfully started.
    if (!g_xrFramePending)
    {
        return;
    }
    // Process the separate VR GUI swapchain.
    bool guiImageReady = false;

    if (g_xrPendingShouldRender &&
        g_xrGuiSwapchain != XR_NULL_HANDLE &&
        g_xrGuiViewSpace != XR_NULL_HANDLE)
    {
        guiImageReady = vrProcessGuiSwapchainImage();
        // Log the first successful GUI image operation.
        static bool guiImageReadyLogged = false;

        if (guiImageReady && !guiImageReadyLogged)
        {
            SDL_Log(
                "OpenXR: GUI swapchain image processed successfully!"
            );

            guiImageReadyLogged = true;
        }
    }
    // Prepare the completed stereo frame for submission.
    XrFrameEndInfo endInfo{};
    endInfo.type = XR_TYPE_FRAME_END_INFO;
    endInfo.displayTime = g_xrPendingDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    // Enable transparency for the floating VR GUI panel.
    g_xrGuiLayer.layerFlags =
        XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
        XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;

    // Only submit the dungeon when stereo rendering succeeded.
    const bool stereoReady =
        g_xrPendingShouldRender &&
        g_xrEyeViewsValid &&
        g_xrStereoFrameReady;

    // Only display the GUI when its swapchain image is ready.
    const bool showGui =
        g_xrPendingShouldRender &&
        guiImageReady &&
        g_xrGuiSwapchain != XR_NULL_HANDLE &&
        g_xrGuiViewSpace != XR_NULL_HANDLE;

    // Submit the dungeon first, then the floating GUI panel.
    const XrCompositionLayerBaseHeader* layers[2] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(
            &g_xrProjectionLayer
        ),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(
            &g_xrGuiLayer
        )
    };

    // Dungeon: project the stereo view and optionally the GUI.
    // Menu: submit only the floating GUI panel.
    if (stereoReady)
    {
        endInfo.layerCount = showGui ? 2u : 1u;
        endInfo.layers = layers;
    }
    else if (showGui)
    {
        endInfo.layerCount = 1u;
        endInfo.layers = &layers[1];
    }
    else
    {
        endInfo.layerCount = 0u;
        endInfo.layers = nullptr;
    }
    // Submit the frame to SteamVR.
    const XrResult result = xrEndFrame(g_xrSession, &endInfo);

    // Allow the next OpenXR frame to begin.
    g_xrFramePending = false;

    if (XR_SUCCEEDED(result))
    {
        ++g_xrFrameCount;

        if (g_xrFrameCount % 300 == 0)
        {
            SDL_Log(
                "OpenXR: Successfully processed %llu VR frames.",
                static_cast<unsigned long long>(g_xrFrameCount)
            );
        }
    }
    else
    {
        SDL_Log(
            "OpenXR: Deferred xrEndFrame failed (%d)",
            static_cast<int>(result)
        );
    }
}

void vrCheckSwapchainFormats()
{
    if (g_xrSession == XR_NULL_HANDLE)
    {
        return;
    }

    uint32_t formatCount = 0;

    XrResult result = xrEnumerateSwapchainFormats(
        g_xrSession,
        0,
        &formatCount,
        nullptr
    );

    if (XR_SUCCEEDED(result))
    {
        std::printf(
            "OpenXR: Runtime supports %u swapchain formats.\n",
            formatCount
        );

        if (formatCount > 0)
        {
            std::vector<int64_t> formats(formatCount);

            result = xrEnumerateSwapchainFormats(
                g_xrSession,
                formatCount,
                &formatCount,
                formats.data()
            );

            if (XR_SUCCEEDED(result))
            {
                for (uint32_t i = 0; i < formatCount; ++i)
                {
                    std::printf(
                        "OpenXR: Swapchain format %u = %lld\n",
                        i,
                        static_cast<long long>(formats[i])
                    );
                }
            }
        }
    }
    else
    {
        std::printf(
            "OpenXR: Failed to enumerate swapchain formats (%d)\n",
            static_cast<int>(result)
        );
    }
}
void vrCheckViewConfiguration()
{
    if (g_xrInstance == XR_NULL_HANDLE ||
        g_xrSystemId == XR_NULL_SYSTEM_ID)
    {
        return;
    }

    uint32_t viewCount = 0;

    XrResult result = xrEnumerateViewConfigurationViews(
        g_xrInstance,
        g_xrSystemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        0,
        &viewCount,
        nullptr
    );

    if (XR_FAILED(result))
    {
        std::printf(
            "OpenXR: Failed to query VR view count (%d)\n",
            static_cast<int>(result)
        );
        return;
    }

    std::printf(
        "OpenXR: Stereo view count = %u\n",
        viewCount
    );

    if (viewCount == 0)
    {
        return;
    }

    std::vector<XrViewConfigurationView> views(viewCount);

    for (auto& view : views)
    {
        view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        view.next = nullptr;
    }

    result = xrEnumerateViewConfigurationViews(
        g_xrInstance,
        g_xrSystemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        viewCount,
        &viewCount,
        views.data()
    );

    if (XR_SUCCEEDED(result))
    {
        for (uint32_t i = 0; i < viewCount; ++i)
        {
            std::printf(
                "OpenXR: Eye %u recommended resolution: %u x %u\n",
                i,
                views[i].recommendedImageRectWidth,
                views[i].recommendedImageRectHeight
            );
        }
    }
    else
    {
        std::printf(
            "OpenXR: Failed to retrieve VR eye resolutions (%d)\n",
            static_cast<int>(result)
        );
    }
}
void vrCreateSwapchain()
{
    if (g_xrSession == XR_NULL_HANDLE ||
        g_xrSwapchain != XR_NULL_HANDLE)
    {
        return;
    }

    XrSwapchainCreateInfo swapchainInfo{};
    swapchainInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    swapchainInfo.usageFlags =
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainInfo.format = g_xrSwapchainFormat;
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = g_xrEyeWidth;
    swapchainInfo.height = g_xrEyeHeight;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 2;
    swapchainInfo.mipCount = 1;

    XrResult result = xrCreateSwapchain(
        g_xrSession,
        &swapchainInfo,
        &g_xrSwapchain
    );

    if (XR_SUCCEEDED(result))
    {
        std::printf(
            "OpenXR: Stereo swapchain created successfully! "
            "%u x %u per eye.\n",
            g_xrEyeWidth,
            g_xrEyeHeight
        );
        // Connect each eye's projection view to its swapchain texture layer.
        for (uint32_t eye = 0; eye < 2; ++eye)
        {
            g_xrProjectionViews[eye].subImage.swapchain = g_xrSwapchain;
            g_xrProjectionViews[eye].subImage.imageRect.offset = { 0, 0 };
            g_xrProjectionViews[eye].subImage.imageRect.extent = {
                static_cast<int32_t>(g_xrEyeWidth),
                static_cast<int32_t>(g_xrEyeHeight)
            };
            g_xrProjectionViews[eye].subImage.imageArrayIndex = eye;
        }

        // Retrieve the OpenGL textures provided by SteamVR.
        vrGetSwapchainImages();
    }
    else
    {
        std::printf(
            "OpenXR: Stereo swapchain creation failed (%d)\n",
            static_cast<int>(result)
        );

        g_xrSwapchain = XR_NULL_HANDLE;
    }
}
void vrGetSwapchainImages()
{
    if (g_xrSwapchain == XR_NULL_HANDLE)
    {
        return;
    }

    uint32_t imageCount = 0;

    XrResult result = xrEnumerateSwapchainImages(
        g_xrSwapchain,
        0,
        &imageCount,
        nullptr
    );

    if (XR_FAILED(result) || imageCount == 0)
    {
        std::printf(
            "OpenXR: Failed to query swapchain image count (%d)\n",
            static_cast<int>(result)
        );
        return;
    }

    g_xrSwapchainImages.resize(imageCount);

    for (auto& image : g_xrSwapchainImages)
    {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        image.next = nullptr;
    }

    result = xrEnumerateSwapchainImages(
        g_xrSwapchain,
        imageCount,
        &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(
            g_xrSwapchainImages.data()
            )
    );

    if (XR_SUCCEEDED(result))
    {
        std::printf(
            "OpenXR: Retrieved %u OpenGL swapchain images!\n",
            imageCount
        );

        for (uint32_t i = 0; i < imageCount; ++i)
        {
            std::printf(
                "OpenXR: Swapchain image %u texture ID = %u\n",
                i,
                g_xrSwapchainImages[i].image
            );
        }
    }
    else
    {
        std::printf(
            "OpenXR: Failed to retrieve swapchain images (%d)\n",
            static_cast<int>(result)
        );

        g_xrSwapchainImages.clear();
    }
}
void vrTestAcquireSwapchainImage()
{
    if (g_xrSwapchain == XR_NULL_HANDLE ||
        !g_xrSessionRunning)
    {
        return;
    }

    XrSwapchainImageAcquireInfo acquireInfo{};
    acquireInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;

    uint32_t imageIndex = 0;

    XrResult result = xrAcquireSwapchainImage(
        g_xrSwapchain,
        &acquireInfo,
        &imageIndex
    );

    if (XR_SUCCEEDED(result))
    {
        std::printf(
            "OpenXR: Acquired swapchain image index %u\n",
            imageIndex
        );

        // Image must be waited on and released before reuse.
        XrSwapchainImageWaitInfo waitInfo{};
        waitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
        waitInfo.timeout = XR_INFINITE_DURATION;

        result = xrWaitSwapchainImage(
            g_xrSwapchain,
            &waitInfo
        );

        if (XR_SUCCEEDED(result))
        {
            std::printf(
                "OpenXR: Swapchain image ready for rendering!\n"
            );
            // Bind our VR framebuffer for future eye rendering.
            if (g_glBindFramebuffer && g_xrFramebuffer != 0)
            {
                // Remember Barony's currently bound framebuffer.
                GLint previousFramebuffer = 0;
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);

                // Bind our VR framebuffer.
                g_glBindFramebuffer(GL_FRAMEBUFFER, g_xrFramebuffer);
                // Enable VR viewport coordinates while the VR framebuffer is bound.
                vrCameraViewportEnabled = true;
                // Enable headset tracking for the VR eye cameras.
                vrCameraTrackingEnabled = true;
                // Diagnostic: check whether sRGB framebuffer conversion is enabled.
                static bool srgbDiagnosticPrinted = false;

                if (!srgbDiagnosticPrinted)
                {
                    GLboolean srgbEnabled = glIsEnabled(GL_FRAMEBUFFER_SRGB);

                    std::printf(
                        "OpenXR: GL_FRAMEBUFFER_SRGB enabled = %d\n",
                        static_cast<int>(srgbEnabled)
                    );

                    srgbDiagnosticPrinted = true;
                }
                // Attach the left-eye layer of the acquired stereo texture.
                if (g_glFramebufferTextureLayer &&
                    imageIndex < g_xrSwapchainImages.size())
                {
                    GLuint textureID = g_xrSwapchainImages[imageIndex].image;

                    g_glFramebufferTextureLayer(
                        GL_FRAMEBUFFER,
                        GL_COLOR_ATTACHMENT0,
                        textureID,
                        0,
                        0
                    );

                    std::printf(
                        "OpenXR: Left-eye texture layer attached! Texture ID = %u\n",
                        textureID
                    );
                }

                std::printf(
                    "OpenXR: VR framebuffer bound successfully!\n"
                );
                // Check whether the VR framebuffer is ready for rendering.
                if (g_glCheckFramebufferStatus)
                {
                    GLenum status = g_glCheckFramebufferStatus(GL_FRAMEBUFFER);

                    if (status == GL_FRAMEBUFFER_COMPLETE)
                    {
                        std::printf(
                            "OpenXR: VR framebuffer is COMPLETE!\n"
                        );
                        // Test rendering into the left-eye VR texture.
                        GLfloat previousClearColor[4];
                        glGetFloatv(GL_COLOR_CLEAR_VALUE, previousClearColor);

                        glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
                        glClear(GL_COLOR_BUFFER_BIT);
                        // Diagnostic: read the center pixel from the left-eye framebuffer.
                        static bool alphaDiagnosticPrinted = false;

                        if (!alphaDiagnosticPrinted)
                        {
                            GLubyte pixel[4] = { 0, 0, 0, 0 };

                            glReadPixels(
                                static_cast<GLint>(g_xrEyeWidth / 2),
                                static_cast<GLint>(g_xrEyeHeight / 2),
                                1,
                                1,
                                GL_RGBA,
                                GL_UNSIGNED_BYTE,
                                pixel
                            );

                            std::printf(
                                "OpenXR: Left-eye pixel RGBA = %u, %u, %u, %u\n",
                                static_cast<unsigned>(pixel[0]),
                                static_cast<unsigned>(pixel[1]),
                                static_cast<unsigned>(pixel[2]),
                                static_cast<unsigned>(pixel[3])
                            );

                            alphaDiagnosticPrinted = true;
                        }
                        // Restore Barony's original clear color.
                        glClearColor(
                            previousClearColor[0],
                            previousClearColor[1],
                            previousClearColor[2],
                            previousClearColor[3]
                        );

                        std::printf(
                            "OpenXR: Left-eye VR texture cleared to BLUE!\n"
                        );
                    }
                    else
                    {
                        std::printf(
                            "OpenXR: VR framebuffer incomplete! Status = %u\n",
                            static_cast<unsigned int>(status)
                        );
                    }
                }
                // Attach the right-eye layer of the stereo swapchain texture.
                if (g_glFramebufferTextureLayer &&
                    g_glCheckFramebufferStatus &&
                    imageIndex < g_xrSwapchainImages.size())
                {
                    GLuint textureID = g_xrSwapchainImages[imageIndex].image;

                    g_glFramebufferTextureLayer(
                        GL_FRAMEBUFFER,
                        GL_COLOR_ATTACHMENT0,
                        textureID,
                        0,
                        1
                    );

                    GLenum rightEyeStatus =
                        g_glCheckFramebufferStatus(GL_FRAMEBUFFER);

                    if (rightEyeStatus == GL_FRAMEBUFFER_COMPLETE)
                    {
                        std::printf(
                            "OpenXR: Right-eye VR framebuffer is COMPLETE!\n"
                        );
                        // Test rendering RED into the right-eye VR texture.
                        GLfloat previousClearColor[4];
                        glGetFloatv(GL_COLOR_CLEAR_VALUE, previousClearColor);

                        glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
                        glClear(GL_COLOR_BUFFER_BIT);

                        // Restore Barony's original clear color.
                        glClearColor(
                            previousClearColor[0],
                            previousClearColor[1],
                            previousClearColor[2],
                            previousClearColor[3]
                        );

                        std::printf(
                            "OpenXR: Right-eye VR texture cleared to RED!\n"
                        );
                    }
                    else
                    {
                        std::printf(
                            "OpenXR: Right-eye VR framebuffer INCOMPLETE! Status = %u\n",
                            static_cast<unsigned int>(rightEyeStatus)
                        );
                    }
                }
                // Restore Barony's original framebuffer.
                g_glBindFramebuffer(
                    GL_FRAMEBUFFER,
                    static_cast<GLuint>(previousFramebuffer)
                );
            }
        }
        else
        {
            std::printf(
                "OpenXR: Swapchain image wait failed (%d)\n",
                static_cast<int>(result)
            );
        }

        XrSwapchainImageReleaseInfo releaseInfo{};
        releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;

        XrResult releaseResult = xrReleaseSwapchainImage(
            g_xrSwapchain,
            &releaseInfo
        );

        if (XR_SUCCEEDED(releaseResult))
        {
            std::printf(
                "OpenXR: Swapchain image released successfully!\n"
            );
        }
        else
        {
            std::printf(
                "OpenXR: Swapchain image release failed (%d)\n",
                static_cast<int>(releaseResult)
            );
        }
    }
    else
    {
        std::printf(
            "OpenXR: Swapchain image acquisition failed (%d)\n",
            static_cast<int>(result)
        );
    }
}
void vrLocateEyeViews(XrTime predictedDisplayTime)
{
    // Assume tracking is invalid until OpenXR confirms otherwise.
    g_xrEyeViewsValid = false;

    if (g_xrSession == XR_NULL_HANDLE ||
        g_xrReferenceSpace == XR_NULL_HANDLE ||
        !g_xrSessionRunning)
    {
        return;
    }

    XrViewLocateInfo locateInfo{};
    locateInfo.type = XR_TYPE_VIEW_LOCATE_INFO;
    locateInfo.viewConfigurationType =
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = predictedDisplayTime;
    locateInfo.space = g_xrReferenceSpace;

    XrViewState viewState{};
    viewState.type = XR_TYPE_VIEW_STATE;

    uint32_t viewCount = 0;

    XrResult result = xrLocateViews(
        g_xrSession,
        &locateInfo,
        &viewState,
        2,
        &viewCount,
        g_xrViews
    );

    if (XR_SUCCEEDED(result))
    {
        // Update each eye's projection pose and field of view.
        if (viewCount == 2 &&
            (viewState.viewStateFlags &
                XR_VIEW_STATE_POSITION_VALID_BIT) &&
            (viewState.viewStateFlags &
                XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        {
            for (uint32_t eye = 0; eye < 2; ++eye)
            {
                g_xrProjectionViews[eye].pose = g_xrViews[eye].pose;
                g_xrProjectionViews[eye].fov = g_xrViews[eye].fov;
            }

            // Both eyes have valid tracking information for this frame.
            g_xrEyeViewsValid = true;
        }
        // Log successful tracking approximately once every 300 frames.
        static unsigned int trackingFrameCount = 0;
        ++trackingFrameCount;

        if (trackingFrameCount % 300 == 0)
        {
            std::printf(
                "OpenXR: Tracking %u eye views successfully!\n",
                viewCount
            );
        }
    }
    else
    {
        std::printf(
            "OpenXR: xrLocateViews failed (%d)\n",
            static_cast<int>(result)
        );
    }
}
// STEP 593: Hand and equipment are DIFFERENT voxel meshes; they should not
// share a single forced scale. Tune these visual-only values after headset test.
// Preserve first-person item's own base size (often 0.5 or 1.0).
static constexpr float VR_HAND_UNITS_PER_METER = 8.0f;
static constexpr float VR_HAND_VOXEL_SCALE = 0.19f; // Step 592 was 0.30: too large relative to equipment.
static constexpr float VR_ITEM_VOXEL_SCALE = 0.66f;
static constexpr float VR_SHIELD_VOXEL_SCALE = 0.65f;

// STEP 594: The equipped weapon and the first-person arm were authored with
// DIFFERENT resting rotations. In Barony's actHudArm() the ordinary humanoid
// arm uses pitch = -17*PI/32, yaw = -2*PI/32. In actHudWeapon() ordinary held
// items rest at pitch = PI/8 and a small negative yaw (approximately -0.1).
// Copying the hand's angles directly onto the weapon (Step 593) pointed the
// blade/head backward along the user's forearm. Keep the native relative
// weapon-to-grip rotation instead of making the two meshes parallel.
static constexpr float VR_RIGHT_WEAPON_GRIP_PITCH = 21.0f * static_cast<float>(PI) / 32.0f; // ~118 degrees
static constexpr float VR_RIGHT_WEAPON_GRIP_YAW =
2.0f * static_cast<float>(PI) / 32.0f - 0.1f; // ~5.5 degrees

// STEP 595: weapon-specific visual grip calibration. Barony's first-person
// equipment is authored for the flat HUD, not for a VR controller. Retaining
// that HUD focal offset (usually focalz=-4) in Step 594 lifted the weapon far
// above the actual hand. The safe default is a ZERO HUD focal offset. We can
// refine each ItemType profile using real headset observations as we test it.
// These positions are in Barony WORLD entity units, NOT OpenXR meters.
// Positive 'up' means visually higher (Barony's z axis points down).
struct VrWeaponGripProfile
{
    float yawOffset;
    float pitchOffset;
    float rollOffset;
    float forwardOffset;
    float sideOffset;
    float upOffset;
    float scaleMultiplier;
    // Additional downward displacement in Barony units along the controller's
    // rotated LOCAL -Y axis. Keep zero until calibrated for each item type.
    float localGripDownOffset;
};

static VrWeaponGripProfile vrWeaponGripForItem(const Item* equipped)
{
    // STEP 600: The Step 599 changes were only 0.04 Barony units (~5 mm),
    // below a reliably visible headset adjustment. Move the non-tomahawk
    // fallback 0.45 units (~5.6 cm) forward in the controller's LOCAL frame.
    // Identify the seasonal model's actual ItemType before assigning a
    // narrower per-weapon profile; this intentionally affects the fallback.
    const VrWeaponGripProfile generic = {
        VR_RIGHT_WEAPON_GRIP_YAW, VR_RIGHT_WEAPON_GRIP_PITCH, 0.0f,
        1.35f, 0.0f, 0.08f, 1.0f, 0.0f
    };
    if (!equipped)
        return generic;

    // STEP 603: The Step 602 +0.60 CONTROLLER-local reach moved a staff
    // toward the player's chest when that controller was tilted backwards.
    // Keep its generic grip here; a BODY-forward displacement is applied
    // after the rigid controller transform in vrDrawTrackedHeldVoxel().
    // This is visual only; it does not change magic projectile aiming.
    if (itemCategory(equipped) == MAGICSTAFF)
        return generic;

    switch (equipped->type)
    {
        // STEP 601: Crossbow grip is not the same as a melee weapon.
        // The controller AIM pose (below) points the barrel; this controller-
        // local translation places the model closer to the fist instead of
        // leaving the huge default first-person forward offset intact.
        // These values are initial calibration, pending headset feedback.
    case CROSSBOW:
    case HEAVY_CROSSBOW:
    case BLACKIRON_CROSSBOW:
        // STEP 603: The Step 602 +120-degree extra pitch turned the barrel
        // toward the player's face. Undo 100 degrees of THAT adjustment,
        // leaving +20 degrees relative to Step 601. Visual model only.
        return { VR_RIGHT_WEAPON_GRIP_YAW,
                 VR_RIGHT_WEAPON_GRIP_PITCH + static_cast<float>(PI) / 9.0f,
                 0.0f, 0.98f, 0.00f, -0.16f, 1.0f, 0.00f };
    case BRONZE_TOMAHAWK:
        // STEP 604: Lower the tomahawk another 10% of the estimated 2.4-unit
        // virtual arm length (0.24 Barony units), in the controller's LOCAL
        // negative Y direction. STEP 603 was 1.65; now 1.89. This changes
        // grip translation only, not the axe orientation or damage logic.
        return { VR_RIGHT_WEAPON_GRIP_YAW, VR_RIGHT_WEAPON_GRIP_PITCH, 0.0f,
                 0.85f, 0.0f, 0.08f, 1.0f, 1.89f };
        // Add individually tested profiles for swords, axes, staffs, bows,
        // thrown weapons etc. here, keyed by their actual ItemType enum.
    default:
        return generic;
    }
}

// The offhand HUD entity is *not* Player::HUD_t::shield. The vanilla game
// creates an Entity with behavior actHudShield() and skill[2]=player index.
// Resolve it every frame, rather than storing a pointer across level changes.
static Entity* vrFindLocalHudShield()
{
    if (!map.entities || clientnum < 0 || clientnum >= MAXPLAYERS)
        return nullptr;
    for (node_t* node = map.entities->first; node; node = node->next)
    {
        Entity* const candidate = static_cast<Entity*>(node->element);
        if (candidate && candidate->behavior == &actHudShield &&
            candidate->skill[2] == clientnum)
        {
            return candidate;
        }
    }
    return nullptr;
}

// STEP 613: The first-person book's second half is an independent HUD
// entity. Its screen-fixed placement must not survive into a tracked-eye pass.
// Keep both additional HUD limbs hidden only while drawing a VR world view;
// their state is restored before Barony's normal rendering resumes.
static Entity* vrFindLocalHudAdditional(bool second)
{
    if (!map.entities || clientnum < 0 || clientnum >= MAXPLAYERS)
        return nullptr;
    for (node_t* node = map.entities->first; node; node = node->next)
    {
        Entity* const candidate = static_cast<Entity*>(node->element);
        if (candidate &&
            candidate->behavior == (second ? &actHudAdditional2 : &actHudAdditional) &&
            candidate->skill[2] == clientnum)
            return candidate;
    }
    return nullptr;
}

// STEP 602: The "loose bolt" isn't the fired projectile. Vanilla Barony
// spawns a SEPARATE actHudArrowModel as a first-person equipment entity.
// Find it without caching across character creation / level transitions.
static Entity* vrFindLocalHudArrowModel()
{
    if (!map.entities || clientnum < 0 || clientnum >= MAXPLAYERS)
        return nullptr;
    for (node_t* node = map.entities->first; node; node = node->next)
    {
        Entity* const candidate = static_cast<Entity*>(node->element);
        if (candidate && candidate->behavior == &actHudArrowModel &&
            candidate->skill[2] == clientnum)
            return candidate;
    }
    return nullptr;
}

static bool vrIsCrossbowType(const Item* item)
{
    return item && (item->type == CROSSBOW ||
        item->type == HEAVY_CROSSBOW ||
        item->type == BLACKIRON_CROSSBOW);
}
// STEP638EL: Vanilla intentionally makes actHudArrowModel invisible when a
// quiver is equipped. In VR the quiver is hidden from the left hand, so a
// loaded crossbow needs its own visual bolt without changing the real quiver.
// Match vanilla's per-weapon loaded/reload sprite checks rather than drawing
// a bolt during recoil or when the first-person weapon is being hidden.
static bool vrCrossbowBoltShouldBeVisible(const Entity* hudBolt,
    const Entity* hudWeapon, const Item* weapon, const Item* offhand)
{
    if (!hudBolt || !hudWeapon || !vrIsCrossbowType(weapon) ||
        hudWeapon->skill[6] != 0 ||
        (clientnum >= 0 && clientnum < MAXPLAYERS &&
            (cast_animation[clientnum].active ||
             cast_animation[clientnum].active_spellbook)))
        return false;
    const bool quiver = offhand && itemTypeIsQuiver(offhand->type);
    if (!quiver)
        return !hudBolt->flags[INVISIBLE] && hudBolt->sprite > 0;
    switch (weapon->type)
    {
        case CROSSBOW:
            return hudWeapon->sprite == items[CROSSBOW].fpindex;
        case HEAVY_CROSSBOW:
            return hudWeapon->sprite != items[HEAVY_CROSSBOW].fpindex + 1;
        case BLACKIRON_CROSSBOW:
            return hudWeapon->sprite == items[BLACKIRON_CROSSBOW].fpindex;
        default:
            return false;
    }
}
// STEP 638EE: Preserve the arrow-model visibility Barony reported BEFORE
// suppressing the flat first-person HUD entity for each stereo eye.
// The child is placed on the right-hand crossbow, never on the offhand.
static bool g_vrLoadedCrossbowBoltForEye = false;

// STEP 597: A tracked item must be a RIGID child of the controller.
// Step 596 applied the controller quaternion to its POSITION but simply ADDED
// Euler angles to its ROTATION; those operations disagree whenever the player
// twists two axes at once. Barony glDrawVoxel() consumes Yaw/Pitch/Roll as:
//   Ry(-yaw) * Rz(-pitch) * Rx(roll)
// in its renderer coordinates (X=forward, Y=up, Z=right). We construct the
// controller basis directly from the OpenXR quaternion, postmultiply by the
// calibrated item basis, and decompose only once for Barony's voxel renderer.
// Both the grip translation and item rotation consequently follow ONE basis.
struct VrGripVector3 { float x, y, z; };
struct VrGripBasis3
{
    VrGripVector3 forward;
    VrGripVector3 up;
    VrGripVector3 right;
};

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

static VrGripVector3 vrGripXRToRender(VrGripVector3 v, float cameraYaw)
{
    // The EXACT axis mapping used by the tracked hand location above:
    // XR -Z -> Barony forward, XR +X -> Barony right, XR +Y -> up.
    const float c = cosf(cameraYaw), s = sinf(cameraYaw);
    return { -v.z * c - v.x * s, v.y, -v.z * s + v.x * c };
}

static VrGripBasis3 vrGripXRControllerBasis(const XrQuaternionf& q, float cameraYaw)
{
    return {
        vrGripXRToRender(vrGripRotateXR(q, { 0.f, 0.f, -1.f }), cameraYaw),
        vrGripXRToRender(vrGripRotateXR(q, { 0.f, 1.f, 0.f }), cameraYaw),
        vrGripXRToRender(vrGripRotateXR(q, { 1.f, 0.f, 0.f }), cameraYaw)
    };
}

// STEP 612: Shared right-controller aim for arrows and thrown projectiles.
// Headset look direction never replaces the right controller's launch ray.
// This immutable per-frame ray is shared by the VR reticle and the real
// projectile hook in entity.cpp. It uses the Quest RIGHT AIM pose, not the
// grip pose, because OpenXR's aim pose is specifically calibrated for pointing.
struct VrWorldAimRay
{
    VrGripVector3 origin; // Barony render basis (X game x, Y up, Z game y)
    VrGripVector3 forward;
    VrGripVector3 up;
    VrGripVector3 right;
};

static bool vrBuildCalibratedBowAim(const view_t* gameCamera, VrWorldAimRay& out);

static bool vrBuildControllerWorldAim(const view_t* gameCamera,
    VrWorldAimRay& out, bool requireRangedWeapon = true,
    bool applyRangedCalibration = true, bool leftHandSpellAim = false)
{
    if (!leftHandSpellAim && vrBowDisplaySelected())
        return vrBuildCalibratedBowAim(gameCamera, out);
    const int aimHand = leftHandSpellAim ? vrSpellControllerHand() : 1;
    if (!gameCamera || intro || gamePaused || !g_xrEyeViewsValid ||
        !g_xrSessionRunning || g_xrSessionState != XR_SESSION_STATE_FOCUSED ||
        clientnum < 0 || clientnum >= MAXPLAYERS ||
        !players[clientnum] || !players[clientnum]->entity ||
        !players[clientnum]->shootmode || !stats[clientnum] ||
        !g_xrControllerHands[aimHand].aimPoseValid)
        return false;
    // Bow and thrown hooks require explicitly equipped projectile items;
    // spells still bypass this and keep their existing independent path.
    if (requireRangedWeapon &&
        (!stats[clientnum]->weapon ||
            !vrIsAimedProjectileItem(*stats[clientnum]->weapon)))
        return false;

    const XrPosef& pose = g_xrControllerHands[aimHand].aimPose;
    const XrVector3f head{
        (g_xrViews[0].pose.position.x + g_xrViews[1].pose.position.x) * .5f,
        (g_xrViews[0].pose.position.y + g_xrViews[1].pose.position.y) * .5f,
        (g_xrViews[0].pose.position.z + g_xrViews[1].pose.position.z) * .5f
    };
    const float c = cosf(gameCamera->ang), si = sinf(gameCamera->ang);
    const float relativeRight = pose.position.x - head.x;
    const float relativeForward = head.z - pose.position.z;
    const float relativeUp = pose.position.y - head.y;
    out.origin = {
        static_cast<float>(gameCamera->x) * 16.f + VR_HAND_UNITS_PER_METER *
            (relativeForward * c - relativeRight * si),
        -(static_cast<float>(gameCamera->z) - 1.f) * .5f +
            VR_HAND_UNITS_PER_METER * relativeUp,
        static_cast<float>(gameCamera->y) * 16.f + VR_HAND_UNITS_PER_METER *
            (relativeForward * si + relativeRight * c)
    };
    const VrGripBasis3 basis = vrGripXRControllerBasis(
        pose.orientation, gameCamera->ang);
    out.forward = basis.forward;
    out.up = basis.up;
    out.right = basis.right;
    // Right weapons keep their per-ItemType calibration. Spells use a
    // separate, persistent LEFT-hand yaw/pitch adjustment. BOTH marker and
    // actual projectile consume the same recalibrated ray.
    VrRangedAimCalibration offset{};
    bool hasCalibration = false;
    if (leftHandSpellAim)
    {
        offset = g_vrSpellAimVisual;
        hasCalibration = true;
    }
    else if (applyRangedCalibration)
    {
        if (const Item* weapon = stats[clientnum]->weapon)
        {
            if (vrIsAimedProjectileItem(*weapon))
            {
                const auto it = g_vrRangedAimVisual.find(static_cast<int>(weapon->type));
                if (it != g_vrRangedAimVisual.end())
                {
                    offset = it->second;
                    hasCalibration = true;
                }
            }
        }
    }
    if (hasCalibration && std::isfinite(offset.yaw) &&
        std::isfinite(offset.pitch))
    {
        const float maxYaw = leftHandSpellAim ? VR_SPELL_AIM_MAX_YAW : .35f;
        const float maxPitch = leftHandSpellAim ? VR_SPELL_AIM_MAX_PITCH : .35f;
        const float h = std::max(-maxYaw, std::min(maxYaw, offset.yaw));
        const float v = std::max(-maxPitch, std::min(maxPitch, offset.pitch));
        VrGripVector3 direction{
            basis.forward.x + tanf(h) * basis.right.x + tanf(v) * basis.up.x,
            basis.forward.y + tanf(h) * basis.right.y + tanf(v) * basis.up.y,
            basis.forward.z + tanf(h) * basis.right.z + tanf(v) * basis.up.z
        };
        const float length = sqrtf(direction.x * direction.x +
            direction.y * direction.y + direction.z * direction.z);
        if (length > 0.5f)
        {
            out.forward = { direction.x / length,
                direction.y / length, direction.z / length };
            const float projection = out.forward.x * basis.right.x +
                out.forward.y * basis.right.y + out.forward.z * basis.right.z;
            const VrGripVector3 side{
                basis.right.x - projection * out.forward.x,
                basis.right.y - projection * out.forward.y,
                basis.right.z - projection * out.forward.z
            };
            const float sideLen = sqrtf(side.x * side.x +
                side.y * side.y + side.z * side.z);
            if (sideLen > .25f)
            {
                out.right = { side.x / sideLen, side.y / sideLen,
                    side.z / sideLen };
                const float upProjection = out.forward.x * basis.up.x +
                    out.forward.y * basis.up.y + out.forward.z * basis.up.z;
                VrGripVector3 up{ basis.up.x - upProjection * out.forward.x,
                    basis.up.y - upProjection * out.forward.y,
                    basis.up.z - upProjection * out.forward.z };
                const float upSide = up.x * out.right.x +
                    up.y * out.right.y + up.z * out.right.z;
                up.x -= upSide * out.right.x;
                up.y -= upSide * out.right.y;
                up.z -= upSide * out.right.z;
                const float upLen = sqrtf(up.x * up.x + up.y * up.y + up.z * up.z);
                if (upLen > .25f)
                    out.up = { up.x / upLen, up.y / upLen, up.z / upLen };
            }
        }
    }
    // STEP 603: Lower the SAME ray origin used by the five-dot reticle,
    // fired arrows, and ordinary spell missiles by 0.35 game units (~4.4cm).
    // Reticle and projectiles remain coaxial: no HMD-based aiming or visual-
    // only reticle offset that would make shots miss their displayed guide.
    out.origin.y -= 0.35f;
    return true;
}

// Called ONLY from Barony's ranged-projectile creation hook. Non-VR and
// non-local projectiles retain all original gameplay code unchanged.
bool vrApplyControllerRangedAim(int playerIndex, Entity* projectile)
{
    if (!projectile || playerIndex != clientnum || multiplayer == CLIENT)
        return false; // Networking synchronization is a future milestone.
    VrWorldAimRay aim{};
    if (!vrBuildControllerWorldAim(&cameras[clientnum], aim))
        return false;
    const float horiz = sqrtf(aim.forward.x * aim.forward.x +
        aim.forward.z * aim.forward.z);
    if (horiz < 0.05f)
        return false; // Prevent near-vertical zero-horizontal projectiles.

    const float originalSpeed = static_cast<float>(projectile->arrowSpeed);
    if (originalSpeed <= 0.01f)
        return false;
    // Physics engine's z goes DOWN; render basis Y goes UP.
    projectile->x = aim.origin.x;
    projectile->y = aim.origin.z;
    projectile->z = -aim.origin.y;
    projectile->yaw = atan2f(aim.forward.z, aim.forward.x);
    projectile->pitch = -atan2f(aim.forward.y, horiz);
    projectile->arrowSpeed = originalSpeed * horiz;
    projectile->vel_x = aim.forward.x * originalSpeed;
    projectile->vel_y = aim.forward.z * originalSpeed;
    projectile->vel_z = -aim.forward.y * originalSpeed;
    if (g_vrBowRelease.nativeScope) {
        g_vrBowRelease.projectileSeen = true;
        printlog("[VR GA16] FIRED native arrow uid=%u draw=%.3fm hand=%d",
            projectile->getUID(),g_vrBowRelease.draw,g_vrBowDrawHand);
    }
    static unsigned int shotCount = 0;
    std::printf("OpenXR VR: STEP600 CONTROLLER_PROJECTILE #%u "
        "origin=(%.2f %.2f %.2f) yaw=%.2f pitch=%.2f "
        "speed=%.2f vz=%.2f\n",
        ++shotCount, projectile->x, projectile->y, projectile->z,
        projectile->yaw, projectile->pitch, projectile->arrowSpeed,
        projectile->vel_z);
    return true;
}

// STEP612: Give Barony's REAL thrown entities the Quest controller's
// direction AND position, not merely a cosmetic reticle. Entity::attack()
// has already chosen the correct item-type speed, toss arc, damage/charge,
// and special boomerang/chakram model transforms when this is called.
// Keep the original upward toss bias (negative Z in Barony means upward)
// while adding controller pitch to the velocity. Gravity remains native.
// As with the Step600 arrow hook, CLIENT multiplayer needs a future
// network aim packet and is deliberately left on its vanilla path.
bool vrApplyControllerThrownAim(int playerIndex, Entity* projectile)
{
    if (!projectile || playerIndex != clientnum || multiplayer == CLIENT ||
        projectile->behavior != &actThrown ||
        playerIndex < 0 || playerIndex >= MAXPLAYERS ||
        !stats[playerIndex] || !stats[playerIndex]->weapon ||
        !vrIsThrowableItem(*stats[playerIndex]->weapon) ||
        !players[playerIndex] || !players[playerIndex]->entity)
        return false;

    VrWorldAimRay aim{};
    if (!vrBuildControllerWorldAim(&cameras[clientnum], aim))
        return false;
    const float speed = hypotf(static_cast<float>(projectile->vel_x),
        static_cast<float>(projectile->vel_y));
    if (speed < 0.05f)
        return false;
    const float vanillaVertical = static_cast<float>(projectile->vel_z);
    const float newYaw = atan2f(aim.forward.z, aim.forward.x);
    const float originalPlayerYaw = static_cast<float>(
        players[playerIndex]->entity->yaw);

    projectile->x = aim.origin.x;
    projectile->y = aim.origin.z;
    projectile->z = -aim.origin.y;
    // Preserve the boomerang's -PI/2 visual yaw and other unique item
    // rotations relative to player facing instead of overwriting them.
    projectile->yaw += static_cast<real_t>(newYaw - originalPlayerYaw);
    // Preserve each item's native throw speed and native upward arc; only
    // rotate the projectile launch according to the controller AIM pose.
    projectile->vel_x = static_cast<real_t>(speed * aim.forward.x);
    projectile->vel_y = static_cast<real_t>(speed * aim.forward.z);
    projectile->vel_z = static_cast<real_t>(vanillaVertical - speed * aim.forward.y);

    static unsigned int throwCount = 0;
    std::printf("OpenXR VR: STEP612 CONTROLLER_THROW #%u item=%d "
        "origin=(%.2f %.2f %.2f) dir=(%.2f %.2f %.2f) "
        "speed=%.2f vz=%.2f\n", ++throwCount,
        static_cast<int>(stats[playerIndex]->weapon->type),
        static_cast<double>(projectile->x),
        static_cast<double>(projectile->y),
        static_cast<double>(projectile->z),
        static_cast<double>(aim.forward.x),
        static_cast<double>(aim.forward.y),
        static_cast<double>(aim.forward.z),
        static_cast<double>(speed),
        static_cast<double>(projectile->vel_z));
    return true;
}

// STEP 601: Same Quest right-AIM ray as the working Step 600 crossbow,
// but on Barony's separate, ordinary magic-missile creation path.
// The caller gates this to propulsion=MISSILE and !trap; skip delayed and
// special XYZ/arc projectiles so their purpose-built trajectories survive.
// Networked-client spell packets do not carry 6DoF aim yet.
bool vrApplyControllerMagicAim(int playerIndex, Entity* projectile)
{
    if (!projectile || playerIndex != clientnum || multiplayer == CLIENT ||
        projectile->behavior != &actMagicMissile ||
        projectile->actmagicDelayMove > 0 ||
        projectile->actmagicProjectileArc != 0 ||
        projectile->actmagicIsVertical != MAGIC_ISVERTICAL_NONE)
        return false;

    VrWorldAimRay aim{};
    // Barony marks missiles created by magic staffs. Staff attacks belong to
    // the RIGHT controller; learned/spellbook missiles belong to the LEFT.
    // This avoids a queued spell taking over a magic staff's shot trajectory.
    const bool magicStaffShot = projectile->actmagicCastByMagicstaff != 0;
    if (!vrBuildControllerWorldAim(&cameras[clientnum], aim,
        false, magicStaffShot, !magicStaffShot))
        return false;
    const float horizontal = sqrtf(aim.forward.x * aim.forward.x +
        aim.forward.z * aim.forward.z);
    if (horizontal < 0.05f)
        return false;
    const float originalSpeed = hypotf(static_cast<float>(projectile->vel_x),
        static_cast<float>(projectile->vel_y));
    if (originalSpeed < 0.01f)
        return false;

    // Ordinary magic missiles previously moved only in Barony X/Y. Opt in
    // to the engine's native XYZ motion so raising/lowering the controller
    // actually changes projectile elevation, not just its visible pitch.
    projectile->x = aim.origin.x;
    projectile->y = aim.origin.z;
    projectile->z = -aim.origin.y;
    projectile->yaw = atan2f(aim.forward.z, aim.forward.x);
    projectile->vel_x = aim.forward.x * originalSpeed;
    projectile->vel_y = aim.forward.z * originalSpeed;
    projectile->vel_z = -aim.forward.y * originalSpeed;
    projectile->pitch = atan2f(static_cast<float>(projectile->vel_z),
        horizontal * originalSpeed);
    projectile->actmagicIsVertical = MAGIC_ISVERTICAL_XYZ;
    static unsigned int spellCount = 0;
    std::printf("OpenXR VR: STEP601 CONTROLLER_SPELL #%u "
        "xyz=(%.2f %.2f %.2f) yaw=%.2f pitch=%.2f v=(%.2f %.2f %.2f)\n",
        ++spellCount,
        static_cast<double>(projectile->x),
        static_cast<double>(projectile->y),
        static_cast<double>(projectile->z),
        static_cast<double>(projectile->yaw),
        static_cast<double>(projectile->pitch),
        static_cast<double>(projectile->vel_x),
        static_cast<double>(projectile->vel_y),
        static_cast<double>(projectile->vel_z));
    return true;
}

// STEP 638DM: Projectiles need an aiming marker *before* the user begins
// casting, including spells selected in the spell list or on quick-cast.
// Inspect the spell's root propulsion element instead of maintaining a
// hand-written spell-ID list; buffs and healing remain nonprojectile.
static bool vrSpellHasMissilePropulsion(const spell_t* spell)
{
    if (!spell || !spell->elements.first || !spell->elements.first->element)
        return false;
    const spellElement_t* const root = static_cast<const spellElement_t*>(
        spell->elements.first->element);
    return root->elementID == SPELL_ELEMENT_PROPULSION_MISSILE ||
        root->elementID == SPELL_ELEMENT_PROPULSION_MISSILE_NOCOST ||
        std::strcmp(root->element_internal_name, "spell_element_missile") == 0 ||
        std::strcmp(root->element_internal_name, "spell_element_trio") == 0;
}

static bool vrHasReadiedProjectileSpell()
{
    if (clientnum < 0 || clientnum >= MAXPLAYERS || !players[clientnum])
        return false;
    const auto& magic = players[clientnum]->magic;
    return vrSpellHasMissilePropulsion(magic.selectedSpell()) ||
        vrSpellHasMissilePropulsion(magic.quickCastSpell());
}

// STEP 638ED: Show one in-world aiming guide at a time. Weapon guides use
// the right controller except during any active cast. Only actual projectile
// casting (or spell calibration preview) uses the purple left-hand guide.
static bool vrShouldShowLeftSpellReticle()
{
    if (clientnum < 0 || clientnum >= MAXPLAYERS ||
        !players[clientnum] || !stats[clientnum])
        return false;
    if (g_vrArmCalibrationActive && g_vrArmCalibrationEditSpellAim)
        return true;
    const auto& anim = cast_animation[clientnum];
    return (anim.active || anim.active_spellbook) &&
        vrSpellHasMissilePropulsion(anim.spell);
}

static bool vrShouldShowRightWeaponReticle()
{
    if (clientnum < 0 || clientnum >= MAXPLAYERS ||
        !players[clientnum] || !stats[clientnum])
        return false;
    // When calibrating the left spell guide, keep the preview uncluttered.
    if (g_vrArmCalibrationActive && g_vrArmCalibrationEditSpellAim)
        return false;
    const auto& anim = cast_animation[clientnum];
    if (anim.active || anim.active_spellbook)
        return false;  // No white weapon reticle while casting a spell.
    const Item* weapon = stats[clientnum]->weapon;
    return weapon && (vrIsAimedProjectileItem(*weapon) ||
        itemCategory(weapon) == MAGICSTAFF ||
        itemCategory(weapon) == SPELL_CAT);
}

// STEP 629: Reuse the same controller aim for a *selected texture* displayed
// on the head-anchored VR GUI. This is a directional reticle (not a predicted
// ballistic landing point for thrown items).
static bool vrProjectControllerAimReticle(XrTime predictedTime,
    bool spellAim, int& outputX, int& outputY)
{
    // The stereo world guide below uses the calibrated bow ray. Do not
    // display a second flat-GUI bow guide based on an uncalibrated AIM pose.
    if (!spellAim && vrBowDisplaySelected()) return false;
    const int hand = spellAim ? vrSpellControllerHand() : 1;
    const XrSpace aimSpace = hand == 0 ? g_xrLeftAimSpace : g_xrRightAimSpace;
    if (!vrCustomCrosshairModeActive() || intro || gamePaused ||
        !g_xrControllerHands[hand].aimPoseValid ||
        aimSpace == XR_NULL_HANDLE || g_xrGuiViewSpace == XR_NULL_HANDLE)
        return false;
    XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
    const XrResult res = xrLocateSpace(aimSpace,
        g_xrGuiViewSpace, predictedTime, &loc);
    if (XR_FAILED(res) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        return false;
    const XrQuaternionf& q = loc.pose.orientation;
    VrGripVector3 dir = vrGripRotateXR(q, {0.f, 0.f, -1.f});
    // STEP 638DY: The HUD guide and the actual projectile must consume
    // identical yaw/pitch calibration. Previously only the spell HUD guide
    // was calibrated, leaving bows/crossbows visually misaligned.
    VrRangedAimCalibration offset{};
    bool hasCalibration = spellAim;
    if (spellAim)
    {
        offset = g_vrSpellAimVisual;
    }
    else if (clientnum >= 0 && clientnum < MAXPLAYERS && stats[clientnum])
    {
        const Item* weapon = stats[clientnum]->weapon;
        if (weapon && vrIsAimedProjectileItem(*weapon))
        {
            const auto it = g_vrRangedAimVisual.find(static_cast<int>(weapon->type));
            if (it != g_vrRangedAimVisual.end())
            {
                offset = it->second;
                hasCalibration = true;
            }
        }
    }
    if (hasCalibration && std::isfinite(offset.yaw) &&
        std::isfinite(offset.pitch))
    {
        const VrGripVector3 right = vrGripRotateXR(q, {1.f, 0.f, 0.f});
        const VrGripVector3 up = vrGripRotateXR(q, {0.f, 1.f, 0.f});
        const float maxYaw = spellAim ? VR_SPELL_AIM_MAX_YAW : .35f;
        const float maxPitch = spellAim ? VR_SPELL_AIM_MAX_PITCH : .35f;
        const float h = std::max(-maxYaw, std::min(maxYaw, offset.yaw));
        const float v = std::max(-maxPitch, std::min(maxPitch, offset.pitch));
        dir.x += tanf(h) * right.x + tanf(v) * up.x;
        dir.y += tanf(h) * right.y + tanf(v) * up.y;
        dir.z += tanf(h) * right.z + tanf(v) * up.z;
        const float len = sqrtf(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
        if (len < .5f) return false;
        dir.x /= len; dir.y /= len; dir.z /= len;
    }
    // STEP 638DY: Project BOTH controllers forward onto the real GUI plane.
    // The legacy right-hand "-panelZ / pz" factor was negative with the HUD
    // at z=-1, mirroring weapon crosshairs and sometimes hiding them when
    // the projected position fell outside the panel. This matches the
    // already-corrected Step638DS forward intersection for left-hand spells.
    if (dir.z >= -0.001f) return false;
    const float t = (g_xrGuiLayer.pose.position.z - loc.pose.position.z) / dir.z;
    if (t <= 0.02f || t > 6.f) return false;
    const float panelX = loc.pose.position.x + t * dir.x - g_xrGuiLayer.pose.position.x;
    const float panelY = loc.pose.position.y + t * dir.y - g_xrGuiLayer.pose.position.y;
    const float halfW = g_xrGuiLayer.size.width * .5f;
    const float halfH = g_xrGuiLayer.size.height * .5f;
    if (halfW <= .05f || halfH <= .05f ||
        fabsf(panelX) > halfW || fabsf(panelY) > halfH) return false;
    outputX = static_cast<int>((panelX / (2.f * halfW) + .5f) * 1280.f);
    outputY = static_cast<int>((.5f - panelY / (2.f * halfH)) * 720.f);
    return true;
}

static void vrUpdateCustomCrosshairAim(XrTime predictedTime)
{
    // Reset each frame so stale reticles cannot persist when a hand is lost.
    g_vrWeaponAimReticleAvailable = false;
    g_vrSpellAimReticleAvailable = false;
    if (vrShouldShowRightWeaponReticle())
        g_vrWeaponAimReticleAvailable = vrProjectControllerAimReticle(
            predictedTime, false, g_vrWeaponAimReticleX, g_vrWeaponAimReticleY);
    if (vrShouldShowLeftSpellReticle())
        g_vrSpellAimReticleAvailable = vrProjectControllerAimReticle(
            predictedTime, true, g_vrSpellAimReticleX, g_vrSpellAimReticleY);
}

// Draw the selected crosshair artwork only in the stereo world. White
// right-hand weapons and purple left-hand casting share the same source
// image, while GUI blocking/charge cues remain separate.
// glDrawSpriteFromImage is Barony's existing 3D Image renderer (opengl.cpp).
// This explicit declaration also handles source variants where draw.hpp does
// not expose the function yet; it does not introduce another implementation.
extern void glDrawSpriteFromImage(view_t*, Entity*, std::string, int, bool, bool);
// The overload sets tint in the existing spriteUI shader without editing the
// player's selected texture or affecting ordinary in-game sprite rendering.
extern void glDrawSpriteFromImageTinted(view_t*, Entity*, std::string,
    int, bool, bool, const float*);
static void vrDrawControllerAimReticle(view_t* eyeCamera)
{
    if (!eyeCamera || clientnum < 0 || clientnum >= MAXPLAYERS ||
        !players[clientnum])
        return;
    const bool drawWeapon = vrShouldShowRightWeaponReticle();
    const bool drawSpell = vrShouldShowLeftSpellReticle();
    if (!drawWeapon && !drawSpell)
        return;

    // Keep the original crosshair selection and its alpha artwork. Resolve
    // each frame to follow changes made in vanilla's Crosshair settings.
    const char* selectedPath = players[clientnum]->hud.getCrosshairPath();
    if (!selectedPath || !*selectedPath)
        return;
    Image* crosshairImage = Image::get(selectedPath);
    if (!crosshairImage || !crosshairImage->getWidth() ||
        !crosshairImage->getHeight())
        return;
    const std::string artworkPath(selectedPath);
    const float longestDimension = static_cast<float>(std::max(
        crosshairImage->getWidth(), crosshairImage->getHeight()));

    // Projectile rays and equipment calibration remain unchanged.
    const bool equippedProjectile = stats[clientnum] &&
        stats[clientnum]->weapon &&
        vrIsAimedProjectileItem(*stats[clientnum]->weapon);
    Entity* marker = players[clientnum]->hud.weapon;
    if (!marker)
        return;
    const auto ox = marker->x, oy = marker->y, oz = marker->z;
    const auto yaw = marker->yaw, pitch = marker->pitch, roll = marker->roll;
    const auto sx = marker->scalex, sy = marker->scaley, sz = marker->scalez;
    const auto fx = marker->focalx, fy = marker->focaly, fz = marker->focalz;
    const bool overdraw = marker->flags[OVERDRAW];
    const bool bright = marker->flags[BRIGHT];
    const bool invisible = marker->flags[INVISIBLE];
    marker->flags[OVERDRAW] = false;
    marker->flags[BRIGHT] = true;
    marker->flags[INVISIBLE] = false;
    marker->yaw = marker->pitch = marker->roll = 0.0;
    marker->focalx = marker->focaly = marker->focalz = 0.0;
    constexpr float distanceUnits = 3.0f * VR_HAND_UNITS_PER_METER;
    const auto drawGuide = [&](bool spellAim)
    {
        VrWorldAimRay aim{};
        if (!vrBuildControllerWorldAim(eyeCamera, aim, false,
            !spellAim && equippedProjectile, spellAim))
            return;
        const float desiredSize = spellAim ? 1.05f : 1.30f;
        // Single scale factor preserves the selected art's aspect ratio.
        const float scale = desiredSize / longestDimension;
        marker->scalex = marker->scaley = marker->scalez = scale;
        const VrGripVector3 center{
            aim.origin.x + aim.forward.x * distanceUnits,
            aim.origin.y + aim.forward.y * distanceUnits,
            aim.origin.z + aim.forward.z * distanceUnits
        };
        marker->x = center.x;
        marker->y = center.z;
        marker->z = -center.y;
        if (spellAim)
        {
            constexpr float kSpellPurple[4] = { 197.f / 255.f,
                125.f / 255.f, 1.f, 1.f };
            glDrawSpriteFromImageTinted(eyeCamera, marker, artworkPath,
                REALCOLORS, true, false, kSpellPurple);
        }
        else
        {
            glDrawSpriteFromImage(eyeCamera, marker, artworkPath,
                REALCOLORS, true, false);
        }
    };
    if (drawWeapon) drawGuide(false);
    if (drawSpell) drawGuide(true);
    marker->x = ox; marker->y = oy; marker->z = oz;
    marker->yaw = yaw; marker->pitch = pitch; marker->roll = roll;
    marker->scalex = sx; marker->scaley = sy; marker->scalez = sz;
    marker->focalx = fx; marker->focaly = fy; marker->focalz = fz;
    marker->flags[OVERDRAW] = overdraw;
    marker->flags[BRIGHT] = bright;
    marker->flags[INVISIBLE] = invisible;
}

static VrGripBasis3 vrGripEulerBasis(float yaw, float pitch, float roll)
{
    // Match glDrawVoxel() exactly: Ry(-yaw) * Rz(-pitch) * Rx(roll).
    const float cy = cosf(yaw), sy = sinf(yaw);
    const float cp = cosf(pitch), sp = sinf(pitch);
    const float cr = cosf(roll), sr = sinf(roll);
    const VrGripVector3 u0{ cy * sp, cp, sy * sp };
    const VrGripVector3 r0{ -sy, 0.f, cy };
    return {
        { cy * cp, -sp, sy * cp },
        { u0.x * cr + r0.x * sr,
          u0.y * cr + r0.y * sr,
          u0.z * cr + r0.z * sr },
        { r0.x * cr - u0.x * sr,
          r0.y * cr - u0.y * sr,
          r0.z * cr - u0.z * sr }
    };
}

static VrGripVector3 vrGripApplyBasis(const VrGripBasis3& b, VrGripVector3 v)
{
    return {
        b.forward.x * v.x + b.up.x * v.y + b.right.x * v.z,
        b.forward.y * v.x + b.up.y * v.y + b.right.y * v.z,
        b.forward.z * v.x + b.up.z * v.y + b.right.z * v.z
    };
}

static void vrGripBasisToBaronyAngles(const VrGripBasis3& b,
    float fallbackYaw, double& yaw, double& pitch, double& roll)
{
    const float flat = sqrtf(b.forward.x * b.forward.x + b.forward.z * b.forward.z);
    // At a vertical grip, yaw is underdetermined. Use the tracked yaw and
    // compute roll relative to it to prevent arbitrary atan2(0,0) changes.
    const float resolvedYaw = flat > 0.0001f ?
        atan2f(b.forward.z, b.forward.x) : fallbackYaw;
    const float resolvedPitch = atan2f(-b.forward.y, flat);
    const float cy = cosf(resolvedYaw), sy = sinf(resolvedYaw);
    const float cp = cosf(resolvedPitch), sp = sinf(resolvedPitch);
    const VrGripVector3 baseUp{ cy * sp, cp, sy * sp };
    const VrGripVector3 baseRight{ -sy, 0.f, cy };
    const float upDot = b.up.x * baseUp.x + b.up.y * baseUp.y + b.up.z * baseUp.z;
    const float rightDot = b.up.x * baseRight.x + b.up.y * baseRight.y + b.up.z * baseRight.z;
    yaw = resolvedYaw;
    pitch = resolvedPitch;
    roll = atan2f(rightDot, upDot);
}

// STEP638EI: Sample the ACTUAL existing UI frame pixels. We are not
// rebuilding HP/MP or minimap visuals: the vanilla HUD is drawn normally,
// then the corresponding rectangles are copied to world-space textures.
// If a frame is unavailable, we leave the desktop/floating original untouched.
extern void glDrawVrUiTexture(view_t*, Entity*, unsigned int, int, int);
struct VrWearableCapturedPanel
{
    GLuint texture = 0;
    GLuint framebuffer = 0;
    int width = 0, height = 0;
    bool ready = false;
    Uint32 lastUpdatedMs = 0;
    SDL_Rect lastArea{ 0, 0, 0, 0 }; // STEP638FZ7: invalidate changed belt crop.
};
// STEP638FB: The vanilla status effects are not part of hpFrame/mpFrame.
// Barony draws them into StatusEffectQueue's child "effects" frame, including
// effect-specific artwork and number overlays. Locate its ACTUAL visible
// image rectangles, not the very large status-effect parent frame (which
// covers half the desktop and would capture unrelated menus).
static std::vector<SDL_Rect> vrActiveStatusEffectIconRects()
{
    std::vector<SDL_Rect> result;
    if (clientnum < 0 || clientnum >= MAXPLAYERS ||
        !players[clientnum] || Frame::virtualScreenX <= 0 ||
        Frame::virtualScreenY <= 0) return result;

    Frame* const root = StatusEffectQueue[clientnum].getStatusEffectFrame();
    if (!root || root->isDisabled() || root->isInvisible()) return result;
    const auto appendVisibleRect = [&result](const SDL_Rect& r)
    {
        const int l = std::max(0, std::min(Frame::virtualScreenX, r.x));
        const int t = std::max(0, std::min(Frame::virtualScreenY, r.y));
        const int rr = std::max(0, std::min(Frame::virtualScreenX, r.x + r.w));
        const int bb = std::max(0, std::min(Frame::virtualScreenY, r.y + r.h));
        if (rr > l && bb > t)
            result.push_back(SDL_Rect{ l, t, rr - l, bb - t });
    };
    Frame* const icons = root->findFrame("effects");
    if (icons && !icons->isDisabled() && !icons->isInvisible())
    {
        const SDL_Rect origin = icons->getAbsoluteSize();
        for (const auto* img : icons->getImages())
        {
            if (!img || img->disabled || img->path.empty() ||
                img->pos.w <= 0 || img->pos.h <= 0) continue;
            appendVisibleRect(SDL_Rect{ origin.x + img->pos.x,
                origin.y + img->pos.y, img->pos.w, img->pos.h });
        }
    }
    // Automaton boiler flame is another native status-effect image in a
    // separate child frame. Preserve it when that notification is active.
    Frame* const boiler = root->findFrame("automaton hunger notification");
    if (boiler && !boiler->isDisabled() && !boiler->isInvisible())
        appendVisibleRect(boiler->getAbsoluteSize());
    return result;
}

static bool vrGetStatusEffectCaptureArea(SDL_Rect& area)
{
    const auto rects = vrActiveStatusEffectIconRects();
    if (rects.empty()) return false;
    int l = Frame::virtualScreenX, t = Frame::virtualScreenY;
    int r = 0, b = 0;
    for (const auto& rect : rects)
    {
        l = std::min(l, rect.x);
        t = std::min(t, rect.y);
        r = std::max(r, rect.x + rect.w);
        b = std::max(b, rect.y + rect.h);
    }
    // A few pixels of room around the outermost status icons also preserves
    // Barony's built-in per-icon numeric overlays without clipping.
    l = std::max(0, l - 3); t = std::max(0, t - 3);
    r = std::min(Frame::virtualScreenX, r + 3);
    b = std::min(Frame::virtualScreenY, b + 3);
    area = SDL_Rect{ l, t, r - l, b - t };
    return area.w >= 8 && area.h >= 8;
}

// All positions in this section are metres in OpenXR LOCAL space.
static VrGripVector3 vrBowSub(VrGripVector3 a, VrGripVector3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
static VrGripVector3 vrBowAdd(VrGripVector3 a, VrGripVector3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
static VrGripVector3 vrBowMul(VrGripVector3 a, float s) { return {a.x*s,a.y*s,a.z*s}; }
static float vrBowDot(VrGripVector3 a, VrGripVector3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static float vrBowLength(VrGripVector3 a) { return sqrtf(vrBowDot(a,a)); }
static VrGripVector3 vrBowCross(VrGripVector3 a, VrGripVector3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
static VrGripVector3 vrBowPosition(int hand) {
    const auto& p=g_xrControllerHands[hand].gripPose.position;
    return {p.x,p.y,p.z};
}
static VrGripVector3 vrBowLocal(VrGripVector3 p) {
    const int hand=1-g_vrBowDrawHand;
    // Local bow X is forward, Y is up, Z is right.
    const auto& grip=g_vrBowGrip[hand];
    const auto rotation=vrGripEulerBasis(grip.yaw,grip.pitch,grip.roll);
    // Local Z rotation is composed before the existing grip basis, so it
    // remains a real model-axis rotation even near a 90-degree grip pitch.
    const float z=g_vrBowZRotation[hand], cz=cosf(z), sz=sinf(z);
    p={cz*p.x-sz*p.y,sz*p.x+cz*p.y,p.z};
    p=vrGripApplyBasis(rotation,p);
    p=vrGripRotateXR(g_vrBowControllerRotation[hand],p);
    p=vrBowAdd(p,{grip.forward,grip.up,grip.side});
    return vrBowAdd(vrBowPosition(hand),vrGripRotateXR(
        g_xrControllerHands[hand].gripPose.orientation,{p.z,p.y,-p.x}));
}
static bool vrBowTrackingValid(bool requireDrawHand = true) {
    if (!vrBowDisplaySelected() || intro || gamePaused || !g_xrEyeViewsValid ||
        !g_xrSessionRunning || g_xrSessionState != XR_SESSION_STATE_FOCUSED ||
        !players[clientnum] || !players[clientnum]->entity ||
        !players[clientnum]->shootmode) return false;
    for (int h=0;h<2;++h) {
        if (!requireDrawHand && h == g_vrBowDrawHand) continue;
        const auto& hand=g_xrControllerHands[h];
        const auto& p=hand.gripPose.position;
        const auto& q=hand.gripPose.orientation;
        if (!hand.gripPoseValid || !std::isfinite(p.x) || !std::isfinite(p.y) ||
            !std::isfinite(p.z) || !std::isfinite(q.x) || !std::isfinite(q.y) ||
            !std::isfinite(q.z) || !std::isfinite(q.w)) return false;
    }
    return true;
}
// GA15: calibrated native model axes and projectile/reticle axes are the
// same. The native attack still controls cooldown, damage, ammo and gravity.
static bool vrBuildCalibratedBowAim(const view_t* camera, VrWorldAimRay& out)
{
    if (!camera || !vrBowTrackingValid(false)) return false;
    auto origin = vrBowLocal({0,0,0});
    auto forward = vrBowSub(vrBowLocal({1,0,0}),origin);
    const auto up = vrBowSub(vrBowLocal({0,1,0}),origin);
    const auto right = vrBowSub(vrBowLocal({0,0,1}),origin);
    if (g_vrBowRelease.nativeScope) {
        origin = {g_vrBowRelease.origin.x,g_vrBowRelease.origin.y,g_vrBowRelease.origin.z};
        forward = {g_vrBowRelease.forward.x,g_vrBowRelease.forward.y,g_vrBowRelease.forward.z};
    } else if (g_vrBowPreview.drawing && vrBowTrackingValid()) {
        const auto direction = vrBowSub(origin,vrBowPosition(g_vrBowDrawHand));
        const float length = vrBowLength(direction);
        if (length > .05f) forward = vrBowMul(direction,1.f/length);
    }
    const VrGripVector3 head{
        (g_xrViews[0].pose.position.x + g_xrViews[1].pose.position.x)*.5f,
        (g_xrViews[0].pose.position.y + g_xrViews[1].pose.position.y)*.5f,
        (g_xrViews[0].pose.position.z + g_xrViews[1].pose.position.z)*.5f};
    const auto delta = vrGripXRToRender(vrBowSub(origin,head),camera->ang);
    out.origin = {
        static_cast<float>(camera->x)*16.f + delta.x*VR_HAND_UNITS_PER_METER,
        -(static_cast<float>(camera->z)-1.f)*.5f + delta.y*VR_HAND_UNITS_PER_METER,
        static_cast<float>(camera->y)*16.f + delta.z*VR_HAND_UNITS_PER_METER};
    out.forward = vrGripXRToRender(forward,camera->ang);
    out.up = vrGripXRToRender(up,camera->ang);
    out.right = vrGripXRToRender(right,camera->ang);
    return true;
}

static void vrCancelPhysicalBow(const char* reason)
{
    if (g_vrBowPreview.gameplay && (g_vrBowPreview.drawing || g_vrBowRelease.pending))
        printlog("[VR GA16] bow cancelled: %s", reason);
    g_vrBowPreview.drawing = false;
    g_vrBowPreview.armed = false;
    g_vrBowPreview.draw = 0.f;
    g_vrBowRelease.pending = false;
    g_vrBowRelease.nativeScope = false;
}

static bool vrBowGameplayCanInteract()
{
    if (!vrPhysicalBowOwnsAttack(clientnum) || !vrBowGameplayActive() ||
        !vrBowTrackingValid() || !g_xrControllerActionsAttached ||
        players[clientnum]->usingCommand() || vrBowHotbarSelecting() || stats[clientnum]->HP <= 0 ||
        stats[clientnum]->defending || !players[clientnum]->entity->isMobile() ||
        players[clientnum]->movement.isPlayerSwimming() ||
        players[clientnum]->entity->skill[13] != 0 ||
        players[clientnum]->entity->effectShapeshift != NOTHING ||
        stats[clientnum]->getEffectActive(EFF_SHAPESHIFT)) return false;
    auto& cast = cast_animation[clientnum];
    return !cast.active && !cast.active_spellbook &&
        !cast.spellWaitingAttackInput() && !cast.spellIgnoreAttack();
}

static void vrUpdateBowPreview()
{
    auto& s = g_vrBowPreview;
    const Uint32 now = SDL_GetTicks();
    const bool gameplay = vrPhysicalBowOwnsAttack(clientnum) && vrBowGameplayActive();
    const Uint32 bowUid = vrBowEquipped() ? stats[clientnum]->weapon->uid : 0;
    const Uint32 playerUid = vrBowEquipped() && players[clientnum] && players[clientnum]->entity ?
        players[clientnum]->entity->getUID() : 0;
    if ((!vrBowPreviewSelected() && !gameplay) || !vrBowTrackingValid() ||
        (gameplay && !vrBowGameplayCanInteract()) ||
        s.hand != g_vrBowDrawHand || s.gameplay != gameplay ||
        s.bowUid != bowUid || s.playerUid != playerUid ||
        (s.lastSample && now - s.lastSample > 150u)) {
        vrCancelPhysicalBow("context, tracking, equipment, or input gap");
        s.hand = g_vrBowDrawHand; s.gameplay = gameplay;
        s.bowUid = bowUid; s.playerUid = playerUid; s.lastSample = now;
        return;
    }
    s.lastSample = now;
    if (g_vrArmCalibrationActive && g_vrBowEditGrip) {
        vrCancelPhysicalBow("editing grip"); return;
    }
    if (g_vrBowRelease.pending) {
        if (now - g_vrBowRelease.atMs > 150u) vrCancelPhysicalBow("release expired");
        return; // never overwrite an unconsumed release with a new draw
    }
    const auto& controller = g_xrControllerHands[g_vrBowDrawHand];
    const float grip = controller.squeeze;
    const auto origin = vrBowLocal({0,0,0});
    const auto rest = vrBowLocal({-.117f,0,0});
    const auto hand = vrBowPosition(g_vrBowDrawHand);
    const float distance = vrBowLength(vrBowSub(hand,rest));
    if (!s.drawing) {
        // Held grip may acquire the string upon entering reach: no fresh edge.
        // Context/attack guards above still cancel unsafe or stale draws.
        if (grip >= .72f && distance <= .12f &&
            (!gameplay || (controller.trigger < .2f &&
             !(g_vrBowDrawHand == 1 && g_xrVrBackpackGripConsumed)))) {
            s.drawing = true; s.draw = 0.f; s.armed = false;
            if (gameplay) printlog("[VR GA17] held grip acquired string hand=%d",g_vrBowDrawHand);
        }
        return;
    }
    const auto forward = vrBowSub(vrBowLocal({1,0,0}),origin);
    const auto delta = vrBowSub(rest,hand);
    const float back = vrBowDot(delta,forward);
    const float sideways = vrBowLength(vrBowSub(delta,vrBowMul(forward,back)));
    if (distance > .85f || sideways > .30f || back < -.12f) {
        vrCancelPhysicalBow("draw outside reach"); return;
    }
    s.draw = std::max(0.f,std::min(g_vrBowFullDraw,back));
    if (grip < .35f) {
        if (s.draw >= .08f) {
            if (gameplay) {
                auto direction = vrBowSub(origin,hand);
                const float length = vrBowLength(direction);
                if (length < .05f) { vrCancelPhysicalBow("invalid release axis"); return; }
                direction = vrBowMul(direction,1.f/length);
                g_vrBowRelease.pending = true;
                g_vrBowRelease.atMs = now;
                g_vrBowRelease.bowUid = bowUid;
                g_vrBowRelease.playerUid = playerUid;
                g_vrBowRelease.draw = s.draw;
                g_vrBowRelease.origin = {origin.x,origin.y,origin.z};
                g_vrBowRelease.forward = {direction.x,direction.y,direction.z};
                printlog("[VR GA16] bow release queued draw=%.3fm",s.draw);
            } else {
                ++s.releases;
                printlog("[VR GA16] calibration release #%u draw=%.3fm; NO SHOT",s.releases,s.draw);
            }
        } else if (gameplay) printlog("[VR GA16] bow release cancelled: draw below 8cm");
        s.drawing = false; s.armed = false; s.draw = 0.f;
    }
}

// Called once from the native HUD weapon tick. Negative means untouched
// vanilla behavior; 0 cancels/idles; 1 holds; 2 consumes exactly one release.
int vrReadPhysicalBowAttack(int playerIndex, bool nativeAllowed)
{
    if (!vrPhysicalBowOwnsAttack(playerIndex)) return -1;
    if (g_vrArmCalibrationActive) {
        g_vrBowRelease.pending = false;
        g_vrBowRelease.nativeScope = false;
        return 0; // preserve the non-firing calibration preview state
    }
    const Uint32 now = SDL_GetTicks();
    if (!nativeAllowed || !vrBowGameplayCanInteract() ||
        !g_vrBowPreview.gameplay || g_vrBowPreview.hand != g_vrBowDrawHand ||
        now - g_vrBowPreview.lastSample > 150u ||
        g_vrBowPreview.bowUid != stats[clientnum]->weapon->uid ||
        g_vrBowPreview.playerUid != players[clientnum]->entity->getUID()) {
        vrCancelPhysicalBow("native attack blocked or stale draw"); return 0;
    }
    if (g_vrBowRelease.pending) {
        g_vrBowRelease.pending = false;
        if (now - g_vrBowRelease.atMs > 150u ||
            g_vrBowRelease.bowUid != stats[clientnum]->weapon->uid ||
            g_vrBowRelease.playerUid != players[clientnum]->entity->getUID()) return 0;
        g_vrBowRelease.nativeTick = ::ticks;
        return 2;
    }
    return g_vrBowPreview.drawing ? 1 : 0;
}

void vrBeginPhysicalBowShot(int playerIndex)
{
    g_vrBowRelease.nativeScope = vrPhysicalBowOwnsAttack(playerIndex) &&
        g_vrBowRelease.nativeTick == ::ticks;
    g_vrBowRelease.projectileSeen = false;
}
void vrEndPhysicalBowShot(int playerIndex)
{
    if (playerIndex == clientnum && g_vrBowRelease.nativeScope) {
        if (!g_vrBowRelease.projectileSeen)
            printlog("[VR GA16] native attack produced no aimed projectile");
        g_vrBowRelease.nativeScope = false;
    }
}

static bool vrDrawBowPreview(view_t* eyeCamera)
{
    static int lastStatus = -1;
    const auto report = [&](int status) {
        if (lastStatus != status) {
            printlog("[VR GA13R1] bow render status=%d selected=%d paused=%d shoot=%d tracking=%d/%d", status,
                vrBowPreviewSelected()?1:0, gamePaused?1:0,
                clientnum>=0 && clientnum<MAXPLAYERS && players[clientnum] && players[clientnum]->shootmode?1:0,
                g_xrControllerHands[0].gripPoseValid?1:0,g_xrControllerHands[1].gripPoseValid?1:0);
            lastStatus=status;
        }
    };
    if (!vrBowDisplaySelected()) { lastStatus=-1; return false; }
    if (!eyeCamera || !vrBowTrackingValid(false)) { report(1); return false; }
    // Fail closed if the supplied native model layout is not present.
    const voxel_t* bow=models[318];
    const voxel_t* arrow=models[934];
    if (!bow || bow->sizex!=15 || bow->sizey!=3 || bow->sizez!=42 ||
        !arrow || arrow->sizex!=24 || arrow->sizey!=5 || arrow->sizez!=42) { report(2); return false; }
    VrNativeHud::GlState state;
    static GLuint palette=0;
    if (!palette) {
        glGenTextures(1,&palette);glBindTexture(GL_TEXTURE_2D,palette);
        unsigned char rgba[512*4]{};
        for (int i=0;i<512;++i) {
            const auto* model=i<256?bow:arrow;
            for(int c=0;c<3;++c)rgba[i*4+c]=model->palette[i%256][c];
            rgba[i*4+3]=255;
        }
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,512,1,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    }
    Mesh mesh{};
    const auto head=VrGripVector3{
        (g_xrViews[0].pose.position.x+g_xrViews[1].pose.position.x)*.5f,
        (g_xrViews[0].pose.position.y+g_xrViews[1].pose.position.y)*.5f,
        (g_xrViews[0].pose.position.z+g_xrViews[1].pose.position.z)*.5f};
    const auto vertex=[&](VrGripVector3 p,int color) {
        const auto v=vrGripXRToRender(vrBowSub(p,head),eyeCamera->ang);
        auto& pos=mesh.data[static_cast<int>(Mesh::BufferType::Position)];
        pos.push_back(eyeCamera->x*32.f+2.f*VR_HAND_UNITS_PER_METER*v.x);
        pos.push_back(-eyeCamera->z+2.f*VR_HAND_UNITS_PER_METER*v.y);
        pos.push_back(eyeCamera->y*32.f+2.f*VR_HAND_UNITS_PER_METER*v.z);
        auto& uv=mesh.data[static_cast<int>(Mesh::BufferType::TexCoord)];
        uv.push_back((color+.5f)/512.f);uv.push_back(.5f);
    };
    const auto box=[&](VrGripVector3 center,VrGripVector3 x,VrGripVector3 y,VrGripVector3 z,int color) {
        VrGripVector3 points[8];
        for(int i=0;i<8;++i)points[i]=vrBowAdd(center,vrBowAdd(vrBowMul(x,(i&1)?1.f:-1.f),
            vrBowAdd(vrBowMul(y,(i&2)?1.f:-1.f),vrBowMul(z,(i&4)?1.f:-1.f))));
        const int indices[]={0,2,3,0,3,1,4,5,7,4,7,6,0,1,5,0,5,4,2,6,7,2,7,3,0,4,6,0,6,2,1,3,7,1,7,5};
        for(int i:indices)vertex(points[i],color);
    };
    const auto origin=vrBowLocal({0,0,0});
    const auto axis=[&](VrGripVector3 p){return vrBowSub(vrBowLocal(p),origin);};
    const auto bx=axis({.0065f,0,0}),by=axis({0,.0065f,0}),bz=axis({0,0,.0065f});
    for(int x=0;x<15;++x)for(int y=0;y<3;++y)for(int z=0;z<42;++z) {
        const int color=bow->data[z+y*42+x*3*42];
        if(color==255 || (x==1 && y==1 && z>=3 && z<=38))continue;
        box(vrBowLocal({(x-10)*.013f,(21-z)*.013f,(y-1)*.013f}),bx,by,bz,color);
    }
    const auto rest=vrBowLocal({-.117f,0,0});
    const bool previewDrawing = g_vrBowPreview.drawing &&
        (vrBowPreviewSelected() || (vrPhysicalBowOwnsAttack(clientnum) && vrBowGameplayActive()));
    const auto nock=previewDrawing?vrBowPosition(g_vrBowDrawHand):rest;
    const auto segment=[&](VrGripVector3 a,VrGripVector3 b) {
        const auto d=vrBowSub(b,a);
        box(vrBowMul(vrBowAdd(a,b),.5f),vrBowMul(d,.5f),
            axis({.0012f,0,0}),axis({0,0,.0012f}),10);
    };
    segment(vrBowLocal({-.117f,.234f,0}),nock);
    segment(nock,vrBowLocal({-.117f,-.221f,0}));
    if(previewDrawing) {
        auto forward=vrBowSub(origin,nock);
        const float length=vrBowLength(forward);
        if(length>.02f) {
            forward=vrBowMul(forward,1.f/length);
            auto right=vrBowCross(forward,axis({0,1,0}));
            const float rl=vrBowLength(right);
            if(rl>.1f) {
                right=vrBowMul(right,1.f/rl);
                const auto up=vrBowCross(right,forward);
                for(int x=0;x<24;++x)for(int y=0;y<5;++y)for(int z=0;z<42;++z) {
                    const int color=arrow->data[z+y*42+x*5*42];if(color==255)continue;
                    const auto center=vrBowAdd(nock,vrBowAdd(vrBowMul(forward,x*.022f),
                        vrBowAdd(vrBowMul(up,(21-z)*.013f),vrBowMul(right,(y-2)*.013f))));
                    box(center,vrBowMul(forward,.011f),vrBowMul(up,.0065f),vrBowMul(right,.0065f),color+256);
                }
            }
        }
    }
    static GLuint bowVao=0, bowBuffers[2]{};
    if(!bowVao)glGenVertexArrays(1,&bowVao);
    if(!bowBuffers[0])glGenBuffers(2,bowBuffers);
    if(!palette || !bowVao || !bowBuffers[0] || !bowBuffers[1]) { report(3); return false; }
    glBindVertexArray(bowVao);
    for(int i=0;i<2;++i) {
        const auto& data=mesh.data[i];
        glBindBuffer(GL_ARRAY_BUFFER,bowBuffers[i]);
        glBufferData(GL_ARRAY_BUFFER,data.size()*sizeof(float),data.data(),GL_STREAM_DRAW);
        glVertexAttribPointer(i,i==0?3:2,GL_FLOAT,GL_FALSE,0,nullptr);
        glEnableVertexAttribArray(i);
    }
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,palette);
    glEnable(GL_DEPTH_TEST);glDepthMask(GL_TRUE);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);
    Shader::unbind();auto& shader=spriteUIShader;shader.bind();
    const GLfloat identity[]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const GLfloat white[]={1,1,1,1},empty[]={0,0,0,0};
    glUniformMatrix4fv(shader.uniform("uModel"),1,GL_FALSE,identity);
    glUniform4fv(shader.uniform("uLightFactor"),1,white);
    // spriteUIShader multiplies Texture * uLightFactor * uLightColor.
    // Zero alpha here discarded every preview fragment in GA13.
    glUniform4fv(shader.uniform("uLightColor"),1,white);
    glUniform4fv(shader.uniform("uColorAdd"),1,empty);
    const GLfloat cameraPosition[]={static_cast<float>(eyeCamera->x*32.f),static_cast<float>(-eyeCamera->z),static_cast<float>(eyeCamera->y*32.f),1.f};
    glUniform4fv(shader.uniform("uCameraPos"),1,cameraPosition);
    glDrawArrays(GL_TRIANGLES,0,static_cast<GLsizei>(mesh.data[0].size()/3));
    Shader::unbind();report(0);return true;
}

// 0: HP/MP; 1: minimap; 2: status; STEP638FE 3: belt hotbar;
// 4: action prompts; 5: chat messages. Each captured from VANILLA art.
// STEP638FZ: Additional original XP bar texture at index 6.
// Existing indices 0..5 are retained for saved wrist/spatial layout.
// GA4: 5 = independent dungeon text; 7 = native ally/co-op health display.
// GA6: 7/8 capture existing follower/co-op art, 9 original skill-up art.
static VrWearableCapturedPanel g_vrWearableCaptured[10];
// STEP638FZ9: The native alert already drives the forward-facing notification.
// Detect that same VISIBLE level-up Frame before refreshing the belt XP copy.
static std::vector<SDL_Rect> vrTransientProgressAlertRects(
    bool includeSkill = true, bool includeLevel = true);
static SDL_Rect vrSpatialHudRect(int index, bool includeBeltChat);
static bool vrCaptureOriginalHudFrame(int handIndex, SDL_Rect rect)
{
    if (rect.w < 8 || rect.h < 8 || Frame::virtualScreenX < 1 ||
        Frame::virtualScreenY < 1) return false;
    auto& output = g_vrWearableCaptured[handIndex];
    // STEP638FZ9: Keep ONE level-up animation: the native alert preserved
    // on the forward-facing VR GUI in Step638FZ6. The separately mounted
    // XP bar captures its pixels from Barony's normal GUI every 125ms,
    // otherwise replaying the same level-up animation again at the belt.
    // While a visible LEVEL-UP (not ordinary SKILL-UP) alert is active,
    // display the last clean XP panel instead. When it ends, the normal
    // XP texture capture resumes and immediately reflects the new level.
    // Never edit native XP, levels, Frame visibility, or the GUI source.
    if (handIndex == 6 && output.ready && output.texture &&
        !vrTransientProgressAlertRects(false, true).empty())
        return true;
    const Uint32 now = SDL_GetTicks();
    const bool sameArea = output.lastArea.x == rect.x &&
        output.lastArea.y == rect.y && output.lastArea.w == rect.w &&
        output.lastArea.h == rect.h;
    // GA7: Chat is authored by the native GUI, sometimes over the hotbar.
    // Its pixels cannot be surgically removed after composition without
    // erasing underlying slot artwork. When chat overlaps the belt region,
    // retain the most recent CLEAN hotbar frame instead of clearing or
    // painting those chat glyphs into the belt. Capture resumes immediately
    // when chat disappears. Never modify the actual game's GUI source.
    if (handIndex == 3 && output.ready && output.texture)
    {
        const SDL_Rect chat = vrSpatialHudRect(2, false);
        if (chat.w > 0 && chat.h > 0 &&
            rect.x < chat.x + chat.w && rect.x + rect.w > chat.x &&
            rect.y < chat.y + chat.h && rect.y + rect.h > chat.y)
            return true;
    }
    if (output.ready && now - output.lastUpdatedMs < 125u &&
        (handIndex != 3 && handIndex != 5 && handIndex != 7 &&
             handIndex != 8 && handIndex != 9 || sameArea))
        return true;
    const GLuint sourceTexture = vrGetGuiTexture();
    if (!sourceTexture) return false;
    GLint oldRead = 0, oldDraw = 0, oldTexture = 0, oldActive = 0;
    GLint oldReadBuffer = 0, oldDrawBuffer = 0, oldPack = 0, oldUnpack = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDraw);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    glGetIntegerv(GL_READ_BUFFER, &oldReadBuffer);
    glGetIntegerv(GL_DRAW_BUFFER, &oldDrawBuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &oldPack);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldUnpack);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glBindTexture(GL_TEXTURE_2D, sourceTexture);
    GLint sourceW = 0, sourceH = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &sourceW);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &sourceH);
    bool success = false;
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (sourceW > 0 && sourceH > 0)
    {
        // Frame coordinates have a TOP-LEFT origin; OpenGL framebuffer
        // rectangles have a BOTTOM-LEFT origin. Clamp before copying.
        const int left = std::max(0, std::min(sourceW,
            static_cast<int>(rect.x * (double)sourceW / Frame::virtualScreenX)));
        const int right = std::max(0, std::min(sourceW,
            static_cast<int>((rect.x + rect.w) * (double)sourceW / Frame::virtualScreenX)));
        const int bottom = std::max(0, std::min(sourceH,
            static_cast<int>((Frame::virtualScreenY - rect.y - rect.h) *
                (double)sourceH / Frame::virtualScreenY)));
        const int top = std::max(0, std::min(sourceH,
            static_cast<int>((Frame::virtualScreenY - rect.y) *
                (double)sourceH / Frame::virtualScreenY)));
        if (right > left && top > bottom)
        {
            // STEP638FZ7: When the belt grows to include message text,
            // downsample X and Y by the SAME factor. Independent 512px
            // clamps used to stretch/shrink one axis of long message lines.
            const float fit = (handIndex == 3 || handIndex == 5 || handIndex == 7 || handIndex == 8 || handIndex == 9) ? std::min(1.f,
                std::min(512.f / static_cast<float>(rect.w),
                         512.f / static_cast<float>(rect.h))) : 1.f;
            const int w = (handIndex == 3 || handIndex == 5 || handIndex == 7 || handIndex == 8 || handIndex == 9) ? std::max(1,
                static_cast<int>(std::lround(rect.w * fit))) :
                std::max(1, std::min(512, rect.w));
            const int h = (handIndex == 3 || handIndex == 5 || handIndex == 7 || handIndex == 8 || handIndex == 9) ? std::max(1,
                static_cast<int>(std::lround(rect.h * fit))) :
                std::max(1, std::min(512, rect.h));
            if (!output.texture) glGenTextures(1, &output.texture);
            if (!output.framebuffer) glGenFramebuffers(1, &output.framebuffer);
            if (output.texture && output.framebuffer)
            {
                glBindTexture(GL_TEXTURE_2D, output.texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                if (output.width != w || output.height != h)
                {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                        GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                    output.width = w; output.height = h;
                }
                GLuint sourceFbo = 0;
                glGenFramebuffers(1, &sourceFbo);
                if (sourceFbo)
                {
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFbo);
                    glFramebufferTexture2D(GL_READ_FRAMEBUFFER,
                        GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sourceTexture, 0);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, output.framebuffer);
                    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
                        GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output.texture, 0);
                    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
                        glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
                    {
                        if (scissor) glDisable(GL_SCISSOR_TEST);
                        glReadBuffer(GL_COLOR_ATTACHMENT0);
                        glDrawBuffer(GL_COLOR_ATTACHMENT0);
                        // STEP638EX: The right-wrist display is rectangular,
                        // matching Barony's native minimap Frame shape. Copy
                        // its ENTIRE image to the wrist without shrinking it
                        // into a circle or zeroing its corner alpha. Vanilla
                        // drawMinimap() already scales each dungeon into the
                        // native minimap frame via max(map.width, map.height).
                        // Keep the tested source axes, wrist pose, size,
                        // persistence, and source HUD entirely unchanged.
                        // STEP638EZ: The Step638EX right-wrist source blit
                        // reversed its horizontal edges (right -> left).
                        // That mirrored the native dungeon and inverted the
                        // player's arrow rotation: turning left looked right.
                        // Correct only this horizontal capture direction.
                        // Left HP/MP blit and both vertical axes are unchanged;
                        // panel position, rotation, and size stay calibrated.
                        glBlitFramebuffer(left, top, right, bottom,
                            0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                        // GA7: Do not scissor-clear chat pixels from the belt.
                        // The old block erased parts of the selection surround
                        // and icon art when chat covered the same screen area.
                        output.lastUpdatedMs = now;
                        output.lastArea = rect;
                        output.ready = true;
                        success = true;
                        if (scissor) glEnable(GL_SCISSOR_TEST);
                    }
                    glDeleteFramebuffers(1, &sourceFbo);
                }
            }
        }
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldRead));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(oldDraw));
    glReadBuffer(static_cast<GLenum>(oldReadBuffer));
    glDrawBuffer(static_cast<GLenum>(oldDrawBuffer));
    glPixelStorei(GL_PACK_ALIGNMENT, oldPack);
    glPixelStorei(GL_UNPACK_ALIGNMENT, oldUnpack);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture));
    glActiveTexture(static_cast<GLenum>(oldActive));
    return success || output.ready;
}

static void vrDrawWearableHudFoundation(view_t* eyeCamera, int handIndex,
    const Entity* trackedHand, const XrQuaternionf& controllerOrientation)
{
    if (!eyeCamera || !trackedHand || intro || gamePaused ||
        clientnum < 0 || clientnum >= MAXPLAYERS ||
        !players[clientnum] || !players[clientnum]->shootmode)
        return;
    auto& hud = players[clientnum]->hud;
    SDL_Rect area{};
    if (handIndex == 0)
    {
        if (!hud.hpFrame || !hud.mpFrame) return;
        const SDL_Rect hp = hud.hpFrame->getAbsoluteSize();
        const SDL_Rect mp = hud.mpFrame->getAbsoluteSize();
        const int left = std::min(hp.x, mp.x);
        const int top = std::min(hp.y, mp.y);
        const int right = std::max(hp.x + hp.w, mp.x + mp.w);
        const int bottom = std::max(hp.y + hp.h, mp.y + mp.h);
        area = SDL_Rect{ left, top, right-left, bottom-top };
    }
    else
    {
        if (!hud.minimapFrame || hud.minimapFrame->isInvisible()) return;
        area = hud.minimapFrame->getAbsoluteSize();
    }
    if (!vrCaptureOriginalHudFrame(handIndex, area)) return;
    auto& panel = g_vrWearableCaptured[handIndex];
    Entity* marker = hud.weapon;
    if (!marker || !panel.texture || !panel.ready) return;
    // Draw a controller-attached textured surface in each eye. All position,
    // rotation and size adjustments are saved in the existing settings file.
    const auto& v = g_vrWearableHud[handIndex];
    const VrGripBasis3 controller = vrGripXRControllerBasis(
        controllerOrientation, eyeCamera->ang);
    const VrGripVector3 offset = vrGripApplyBasis(controller,
        { v.forward, v.up, v.side });
    const VrGripBasis3 extra = vrGripEulerBasis(v.yaw, v.pitch, v.roll);
    const VrGripBasis3 adjusted{
        vrGripApplyBasis(controller, extra.forward),
        vrGripApplyBasis(controller, extra.up),
        vrGripApplyBasis(controller, extra.right) };
    const double bx = marker->x, by = marker->y, bz = marker->z;
    const double ay = marker->yaw, ap = marker->pitch, ar = marker->roll;
    const double sx = marker->scalex, sy = marker->scaley, sz = marker->scalez;
    const double fx = marker->focalx, fy = marker->focaly, fz = marker->focalz;
    const bool oldOverdraw = marker->flags[OVERDRAW];
    const bool oldInvisible = marker->flags[INVISIBLE];
    marker->x = trackedHand->x + offset.x;
    marker->y = trackedHand->y + offset.z;
    marker->z = trackedHand->z - offset.y;
    vrGripBasisToBaronyAngles(adjusted, static_cast<float>(trackedHand->yaw),
        marker->yaw, marker->pitch, marker->roll);
    const float desiredWidth = handIndex == 0 ? 1.45f : 1.24f;
    const float scale = desiredWidth * v.scale / static_cast<float>(panel.width);
    marker->scalex = marker->scaley = marker->scalez = scale;
    marker->focalx = marker->focaly = marker->focalz = 0.0;
    marker->flags[OVERDRAW] = false;
    marker->flags[INVISIBLE] = false;
    glDrawVrUiTexture(eyeCamera, marker, panel.texture, panel.width, panel.height);
    // The status-effect strip keeps its existing relative forearm placement.

    // STEP638FB: Draw the ORIGINAL status-effect icon strip immediately
    // ABOVE the HP/MP rectangle on the SAME left-forearm plane. It inherits
    // all the existing left-forearm position/rotation/size calibration and
    // does not change the independent, verified right-wrist minimap.
    if (handIndex == 0)
    {
        SDL_Rect effectsRect{};
        auto& effects = g_vrWearableCaptured[2];
        if (vrGetStatusEffectCaptureArea(effectsRect) &&
            vrCaptureOriginalHudFrame(2, effectsRect) &&
            effects.texture && effects.ready)
        {
            // glDrawVrUiTexture() draws an image centered at the marker;
            // its local X/Y are adjusted.forward/adjusted.up respectively.
            // Use the SAME pixels-to-world scale as HP/MP so icon artwork
            // remains proportional and follows the forearm calibrator.
            const float above = 0.5f * (panel.height + effects.height) * scale
                + 0.07f * v.scale;
            const float leftAlign = 0.5f * (effects.width - panel.width) * scale;
            // STEP638FD: Separate local translation/rotation/size for status
            // icons. Zero adjustment reproduces Step638FB; HP/MP, wrist map,
            // projectile aim and hand poses are never changed by this page.
            const auto& status = g_vrStatusEffectsHud;
            const VrGripVector3 statusDelta = vrGripApplyBasis(adjusted,
                { leftAlign + status.forward, above + status.up, status.side });
            marker->x += statusDelta.x;
            marker->y += statusDelta.z;
            marker->z -= statusDelta.y;
            const VrGripBasis3 statusEuler = vrGripEulerBasis(
                status.yaw, status.pitch, status.roll);
            const VrGripBasis3 statusBasis{
                vrGripApplyBasis(adjusted, statusEuler.forward),
                vrGripApplyBasis(adjusted, statusEuler.up),
                vrGripApplyBasis(adjusted, statusEuler.right) };
            vrGripBasisToBaronyAngles(statusBasis,
                static_cast<float>(trackedHand->yaw),
                marker->yaw, marker->pitch, marker->roll);
            marker->scalex = marker->scaley = marker->scalez =
                scale * status.scale;
            glDrawVrUiTexture(eyeCamera, marker, effects.texture,
                effects.width, effects.height);
        }
        else
        {
            // An empty effect queue must not leave stale icons on the arm.
            effects.ready = false;
        }
    }
    marker->x = bx; marker->y = by; marker->z = bz;
    marker->yaw = ay; marker->pitch = ap; marker->roll = ar;
    marker->scalex = sx; marker->scaley = sy; marker->scalez = sz;
    marker->focalx = fx; marker->focaly = fy; marker->focalz = fz;
    marker->flags[OVERDRAW] = oldOverdraw;
    marker->flags[INVISIBLE] = oldInvisible;
}

// STEP638FE: Native hotbar/prompt/message HUD on individual stereo surfaces.
// Reuses Barony's own Frame pixels and original HUD item entity as a transient
// rendering proxy. Menus / gamepad and desktop UI are never reparented.
// STEP638FG: The three parent UI Frames used in FE are full-screen layout
// CONTAINERS, not the displayed widget bounds. Capture the actual live
// hotbar slots, action icons and message text rather than rejecting the
// full-screen parents (or mistakenly copying the entire GUI).
// STEP638FZ7: includeBeltChat=false returns the original belt-only bounds.
// The full belt capture additionally encloses live dungeon message glyphs.
static SDL_Rect vrSpatialHudRect(int index, bool includeBeltChat = true)
{
    SDL_Rect empty{ 0, 0, 0, 0 };
    if (clientnum < 0 || clientnum >= MAXPLAYERS || !players[clientnum] ||
        Frame::virtualScreenX <= 0 || Frame::virtualScreenY <= 0)
        return empty;
    Player* const player = players[clientnum];
    // STEP638FZ5: The calibrated forward plane contains ONLY active world
    // interaction prompts. Never fall back to actionPromptsFrame (its layout
    // overlaps the belt HUD and copies unrelated artwork when idle).
    Frame* root = index == 0 ? player->hotbar.hotbarFrame :
        (index == 1 ? player->hud.worldTooltipFrame :
        (index == 2 ? player->messageZone.chatFrame : nullptr));
    if (index == 1)
    {
        if (!root || root->isDisabled() || root->isInvisible())
            return empty;
        bool hasInteractionText = false;
        const char* fields[] = {
            "prompt text", "prompt cycle text", "prompt callout text",
            "prompt spell target text", "prompt spell cancel text"
        };
        for (const char* name : fields)
        {
            Field* const field = root->findField(name);
            if (field && !field->isDisabled() && !field->isInvisible() &&
                field->getText() && field->getText()[0])
            {
                hasInteractionText = true;
                break;
            }
        }
        // No interaction prompt means NO forward surface, not an action-HUD
        // or hotbar fallback. The draw loop invalidates any old cached image.
        if (!hasInteractionText) return empty;
    }
    if (!root || root->isDisabled() || root->isInvisible()) return empty;
    bool found = false;
    int left = Frame::virtualScreenX, top = Frame::virtualScreenY;
    int right = 0, bottom = 0;
    const auto append = [&](SDL_Rect r)
    {
        // Ignore disabled or unlaid-out child widgets and clamp to the
        // source GUI bounds before blitting its framebuffer.
        const int x1 = std::max(0, std::min(Frame::virtualScreenX, r.x));
        const int y1 = std::max(0, std::min(Frame::virtualScreenY, r.y));
        const int x2 = std::max(0, std::min(Frame::virtualScreenX, r.x + r.w));
        const int y2 = std::max(0, std::min(Frame::virtualScreenY, r.y + r.h));
        if (x2 <= x1 || y2 <= y1) return;
        left = std::min(left, x1); top = std::min(top, y1);
        right = std::max(right, x2); bottom = std::max(bottom, y2);
        found = true;
    };
    if (index == 0)
    {
        // Hotbar_t::processHotbar sizes hotbarFrame to the whole screen.
        // Its ten slot frames carry the correct LIVE positions and art.
        for (int i = 0; i < NUM_HOTBAR_SLOTS; ++i)
        {
            Frame* const slot = player->hotbar.getHotbarSlotFrame(i);
            if (slot && !slot->isDisabled() && !slot->isInvisible())
                append(slot->getAbsoluteSize());
        }
        // GA4: Dungeon messages no longer change this rectangle. The belt
        // must have a stable source crop independent of changing chat text.
        // Its original ten slot frames and decoration still render normally.
        (void)includeBeltChat;
    }
    else if (index == 1)
    {
        // Keep the existing native button glyph, icon and text together.
        // This branch is reached only with an active worldTooltipFrame.
        append(root->getAbsoluteSize());
    }
    else if (index == 2)
    {
        // The chat root is full-screen, and even "message box" can span
        // most of it. Select only its currently enabled message text fields.
        Frame* const box = root->findFrame("message box");
        if (box && !box->isDisabled() && !box->isInvisible())
        {
            for (Field* field : box->getFields())
            {
                if (!field || field->isDisabled() || field->isInvisible() ||
                    !field->getText() || !field->getText()[0]) continue;
                SDL_Rect r = field->getAbsoluteSize();
                // STEP638FZ7: Measure the text object rather than imposing
                // a 45%- or 80%-of-screen cutoff. The native field may be
                // narrower than its currently rendered string, so allowing
                // its measured glyph extent is essential here. Final screen
                // clipping is handled once by append(); messages wider than
                // the actual GUI are bounded by the GUI, not a fixed guess.
                int textWidth = std::max(48, r.w);
                if (auto* text = field->getTextObject())
                    textWidth = std::max(48,
                        static_cast<int>(text->getWidth()) + 24);
                if (field->getHJustify() == Field::justify_t::CENTER)
                    r.x += (r.w - textWidth) / 2;
                r.w = textWidth;
                append(r);
            }
        }
    }
    if (!found) return empty;
    // STEP638FM: The hotbar slots are NOT the complete quickbar artwork.
    // The selected-slot surround and other native decoration extend above
    // their ten 48px frame rectangles. FG's 8px crop cut off the upper art,
    // and FE's floating-GUI mask used that same too-small region, leaving
    // the cropped strip behind on the original HUD. Capture and mask ONE
    // matching, expanded rectangle. The belt world position, dimensions,
    // tracking, rotation and the user's saved calibration are unchanged.
    // Prompts/messages retain their original 12px padding.
    const int sideMargin = index == 0 ? 16 : 12;
    // STEP638FZ2: Capture the VANILLA hotbar's existing selected-item popup,
    // which extends beyond the selected slot surround. The world-space slot
    // position is compensated separately below, so belt calibration stays put.
    const int topMargin = index == 0 ? 110 : 12;
    const int bottomMargin = index == 0 ? 16 : 12;
    left = std::max(0, left - sideMargin);
    top = std::max(0, top - topMargin);
    right = std::min(Frame::virtualScreenX, right + sideMargin);
    bottom = std::min(Frame::virtualScreenY, bottom + bottomMargin);
    SDL_Rect result{ left, top, right - left, bottom - top };
    return result.w >= 8 && result.h >= 8 ? result : empty;
}

// STEP638FZ: Lightweight VR-only text labels. They are rendered on world
// surfaces, not inserted into Barony's GUI Frame tree or message history.
// Retain each texture between stereo eyes and rebuild only when text changes.
struct VrSpatialTextLabel
{
    GLuint texture = 0;
    int width = 0, height = 0;
    std::string text;
};
static bool vrPrepareSpatialTextLabel(VrSpatialTextLabel& label,
    const std::string& text)
{
    if (text.empty()) return false;
    if (label.texture && label.text == text) return true;
    TTF_Font* const font = ttf16 ? ttf16 : (ttf12 ? ttf12 : ttf8);
    if (!font) return false;
    SDL_Color white{ 241, 242, 239, 255 };
    SDL_Surface* line = TTF_RenderUTF8_Blended(font, text.c_str(), white);
    if (!line) return false;
    const int width = std::min(720, std::max(164, line->w + 28));
    const int height = line->h + 20;
    SDL_Surface* canvas = SDL_CreateRGBSurfaceWithFormat(0,
        width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!canvas)
    {
        SDL_FreeSurface(line);
        return false;
    }
    SDL_FillRect(canvas, nullptr,
        SDL_MapRGBA(canvas->format, 12, 20, 27, 215));
    SDL_Rect dst{ (width - line->w)/2, (height - line->h)/2, 0, 0 };
    SDL_BlitSurface(line, nullptr, canvas, &dst);
    SDL_FreeSurface(line);
    std::vector<unsigned char> pixels(static_cast<size_t>(width)*height*4u);
    for (int y = 0; y < height; ++y)
        std::memcpy(pixels.data() + static_cast<size_t>(height-1-y)*width*4,
            static_cast<const unsigned char*>(canvas->pixels) + y*canvas->pitch,
            static_cast<size_t>(width)*4);
    SDL_FreeSurface(canvas);
    GLint oldActive = 0, oldBinding = 0, oldUnpack = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldBinding);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldUnpack);
    if (!label.texture) glGenTextures(1, &label.texture);
    if (label.texture)
    {
        glBindTexture(GL_TEXTURE_2D, label.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        label.width = width; label.height = height;
        label.text = text;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, oldUnpack);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldBinding));
    glActiveTexture(static_cast<GLenum>(oldActive));
    return label.texture != 0;
}
static VrSpatialTextLabel g_vrDungeonCalibrationMessage;

// GA6: Only use EXISTING Barony artwork. The native HUD creates separate
// followers and network-player entry frames, not one "allyStatusFrame".
// Capture their already-composited pixels from the original GUI texture.
// No textures, images, health bars, titles, or text are synthesized.
static bool vrNativeAllyArtArea(int panelIndex, SDL_Rect& result)
{
    if (clientnum < 0 || clientnum >= MAXPLAYERS || !players[clientnum] ||
        Frame::virtualScreenX < 1 || Frame::virtualScreenY < 1)
        return false;
    auto& hud = players[clientnum]->hud;
    const bool followers = panelIndex == 7;
    if (followers ? hud.followerBars.empty() : hud.playerBars.empty()) return false;
    Frame* const root = followers ? hud.allyFollowerFrame : hud.allyPlayerFrame;
    if (!root || root->isDisabled() || root->isInvisible() || root->getOpacity() <= 0.0)
        return false;
    int left = Frame::virtualScreenX, top = Frame::virtualScreenY;
    int right = 0, bottom = 0;
    bool found = false;
    const auto append = [&](const SDL_Rect& rect) {
        const int l = std::max(0, std::min(Frame::virtualScreenX, rect.x));
        const int t = std::max(0, std::min(Frame::virtualScreenY, rect.y));
        const int r = std::max(0, std::min(Frame::virtualScreenX, rect.x + rect.w));
        const int b = std::max(0, std::min(Frame::virtualScreenY, rect.y + rect.h));
        if (r <= l || b <= t) return;
        left = std::min(left, l); top = std::min(top, t);
        right = std::max(right, r); bottom = std::max(bottom, b);
        found = true;
    };
    // Ally bars are generated/animated dynamically in updateAllyBarFrame().
    for (Frame* child : root->getFrames())
    {
        if (!child || child->isDisabled() || child->isInvisible() ||
            child->getOpacity() <= 0.0) continue;
        append(child->getAbsoluteSize());
    }
    if (!found) append(root->getAbsoluteSize());
    if (followers)
    {
        // Infinite-scrolling follower bars use Barony's separate title entry.
        Frame* const title = hud.allyFollowerTitleFrame;
        if (title && !title->isDisabled() && !title->isInvisible())
        {
            for (Frame* child : title->getFrames())
                if (child && !child->isDisabled() && !child->isInvisible())
                    append(child->getAbsoluteSize());
        }
    }
    if (!found) return false;
    left = std::max(0, left - 12); top = std::max(0, top - 10);
    right = std::min(Frame::virtualScreenX, right + 12);
    bottom = std::min(Frame::virtualScreenY, bottom + 12);
    result = SDL_Rect{left, top, right-left, bottom-top};
    return result.w >= 8 && result.h >= 8;
}

static bool vrNativeSkillArtArea(SDL_Rect& result)
{
    if (clientnum < 0 || clientnum >= MAXPLAYERS || !players[clientnum] ||
        !players[clientnum]->hud.skillupFrame ||
        players[clientnum]->hud.skillupFrame->isDisabled()) return false;
    const auto pieces = vrTransientProgressAlertRects(true, false);
    if (pieces.empty()) return false;
    int l = Frame::virtualScreenX, t = Frame::virtualScreenY;
    int r = 0, b = 0;
    for (const auto& piece : pieces)
    {
        l = std::min(l, piece.x); t = std::min(t, piece.y);
        r = std::max(r, piece.x + piece.w);
        b = std::max(b, piece.y + piece.h);
    }
    result = SDL_Rect{l, t, r-l, b-t};
    return result.w >= 8 && result.h >= 8;
}

// STEP638GA9: Only the skill-up notification uses an independent native
// widget target. All other panels still follow their exact GA7 render path.
// No process()/updateSkillUpFrame() call: the ordinary UI owns the queue,
// numbers, images, gleam, timing and opacity. Toggle off for the GA7 fallback.
static ConsoleVariable<bool> cvarVrNativeSkillUp("/vr_native_skillup", true);

static bool vrNativeSkillHasPixels(const VrNativeHud::Surface& target)
{
    // Probe only until the first nonempty draw in a visible notification run.
    // An FBO being complete alone does not prove that Frame::drawForVr drew.
    // Preserve pixel-pack state, including a possible caller-owned PBO.
    GLint readFbo, readBuffer, packBuffer, alignment, rowLength, skipRows, skipPixels;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
    glGetIntegerv(GL_READ_BUFFER, &readBuffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, target.framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    std::vector<unsigned char> pixels(static_cast<size_t>(target.width) * target.height * 4u, 0);
    glReadPixels(0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
    glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
    glReadBuffer(readBuffer);
    for (size_t i = 3; i < pixels.size(); i += 4)
        if (pixels[i]) return true;
    return false;
}

// GA10: A frozen copy of a real native notification is used ONLY while
// editing its calibration page. Never create a notification or mutate its queue.
static VrNativeHud::Surface g_vrSkillPreviewSurface;
static VrWearableCapturedPanel g_vrSkillPreviewPanel;
static int g_vrSkillPreviewPlayer = -1;

static bool vrRememberSkillPreview(const VrNativeHud::Surface& source)
{
    VrNativeHud::GlState state;
    if (!VrNativeHud::allocate(g_vrSkillPreviewSurface, source.width, source.height))
        return false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source.framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, source.width, source.height,
        0, 0, source.width, source.height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    g_vrSkillPreviewPanel.texture = g_vrSkillPreviewSurface.texture;
    g_vrSkillPreviewPanel.framebuffer = g_vrSkillPreviewSurface.framebuffer;
    g_vrSkillPreviewPanel.width = source.width;
    g_vrSkillPreviewPanel.height = source.height;
    g_vrSkillPreviewPanel.ready = true;
    g_vrSkillPreviewPlayer = clientnum;
    return true;
}

static const VrWearableCapturedPanel* vrPrepareSkillUpPanel()
{
    static VrNativeHud::Surface scratch, target;
    static VrWearableCapturedPanel native;
    static SDL_Rect canvasArea{};
    static Frame* owner = nullptr;
    static int canvasWidth = 0, canvasHeight = 0;
    static bool active = false, pixelVerified = false, attempted = false;
    static bool previewCaptured = false;
    static Uint32 lastPreviewAt = 0;
    static XrTime lastAttempt = 0;
    static Uint32 lastCallMs = 0;
    static int lastMode = -1;

    SDL_Rect legacyArea{};
    if (!vrNativeSkillArtArea(legacyArea))
    {
        active = pixelVerified = attempted = native.ready = false;
        previewCaptured = false;
        g_vrWearableCaptured[9].ready = false;
        return nullptr;
    }

    Frame* const frame = players[clientnum]->hud.skillupFrame;
    const Uint32 now = SDL_GetTicks();
    if (!active || owner != frame || canvasWidth != Frame::virtualScreenX ||
        canvasHeight != Frame::virtualScreenY || now - lastCallMs > 500u)
    {
        owner = frame;
        canvasWidth = Frame::virtualScreenX;
        canvasHeight = Frame::virtualScreenY;
        const SDL_Rect root = frame->getAbsoluteSize();
        // Keep a stable native canvas throughout the notification. Repacking
        // tight glyph bounds each frame would cancel the native falling motion
        // or resize the panel as numbers bounce. Allow room for vanilla fadeout.
        const int left = std::max(0, root.x - 8);
        const int top = std::max(0, root.y - 8);
        const int right = std::min(canvasWidth, root.x + root.w + 8);
        const int bottom = std::min(canvasHeight, root.y + root.h + 88);
        canvasArea = SDL_Rect{left, top, right - left, bottom - top};
        active = true;
        pixelVerified = attempted = native.ready = false;
        previewCaptured = false;
    }
    lastCallMs = now;

    if (*cvarVrNativeSkillUp)
    {
        // Share the same texture between eyes, including failed/blank attempts.
        if (!attempted || lastAttempt != g_xrPendingDisplayTime)
        {
            attempted = true;
            lastAttempt = g_xrPendingDisplayTime;
            const bool rendered = VrNativeHud::render(scratch, target, canvasArea,
                canvasWidth, canvasHeight, [frame]() {
                    struct NativeSkillDrawScope
                    {
                        bool previous = drawingGui;
                        NativeSkillDrawScope() { drawingGui = true; Shader::unbind(); }
                        ~NativeSkillDrawScope()
                        {
                            drawingGui = previous;
                            // The outer GL guard restores the raw eye shader.
                            // Invalidate Barony's cached program before that.
                            Shader::unbind();
                        }
                    } scope;
                    frame->drawForVr();
                });
            if (rendered && !pixelVerified)
                pixelVerified = vrNativeSkillHasPixels(target);
            native.texture = target.texture;
            native.framebuffer = target.framebuffer;
            native.width = target.width;
            native.height = target.height;
            native.ready = rendered && pixelVerified;
            if (!rendered) pixelVerified = false;
            // Retain strong real artwork at 4 Hz, never its faded tail.
            // Copying this isolated texture never reads the composited desktop.
            if (native.ready && frame->getOpacity() >= 80.0 &&
                (!previewCaptured || now - lastPreviewAt >= 250u))
            {
                if (vrRememberSkillPreview(target))
                {
                    if (!previewCaptured)
                        printlog("[VR GA10] skill calibration preview ready (%dx%d)", target.width, target.height);
                    previewCaptured = true;
                    lastPreviewAt = now;
                }
            }
        }
        if (native.ready)
        {
            if (lastMode != 1)
                printlog("[VR GA9] skillup: native frame pixels verified (%dx%d); other panels GA7",
                    native.width, native.height);
            lastMode = 1;
            return &native;
        }
    }
    else
    {
        // Enabling again must redraw, even during the same OpenXR frame.
        attempted = pixelVerified = native.ready = false;
    }

    if (lastMode != 0)
        printlog("[VR GA9] skillup: GA7 fallback (%s)",
            *cvarVrNativeSkillUp ? "native blank/unavailable" : "console toggle off");
    lastMode = 0;
    auto& fallback = g_vrWearableCaptured[9];
    if (vrCaptureOriginalHudFrame(9, legacyArea) && fallback.ready && fallback.texture)
        return &fallback;
    return nullptr;
}


// GA11: Only the belt is migrated here. Native hotbar/tooltip owners draw
// into their own transparent canvas; the composited GUI is never sampled.
static ConsoleVariable<bool> cvarVrNativeBelt("/vr_native_belt", true);
static bool g_vrNativeBeltReady = false;
static std::vector<SDL_Rect> g_vrNativeBeltWidgetRects;

static bool vrBeltWidgetVisible(Frame* frame)
{
    for (auto* node = frame; node; node = node->getParent())
        if (node->isDisabled() || node->isInvisible() || node->getOpacity() <= 0.0)
            return false;
    return frame != nullptr;
}

static const VrWearableCapturedPanel* vrPrepareNativeBelt(SDL_Rect reference, SDL_Rect& area)
{
    static VrNativeHud::Surface scratch, target;
    static VrWearableCapturedPanel native;
    static Frame* owner = nullptr;
    static SDL_Rect previousArea{};
    static int canvasWidth = 0, canvasHeight = 0, lastMode = -1;
    static XrTime lastFrame = 0;
    static bool attempted = false, pixelVerified = false;
    area = reference;
    g_vrNativeBeltReady = false;
    g_vrNativeBeltWidgetRects.clear();
    if (reference.w <= 0 || reference.h <= 0)
    {
        native.ready = attempted = pixelVerified = false;
        g_vrWearableCaptured[3].ready = false;
        return nullptr;
    }
    auto& player = *players[clientnum];
    Frame* const root = player.hotbar.hotbarFrame;
    if (*cvarVrNativeBelt && vrBeltWidgetVisible(root))
    {
        std::vector<Frame*> widgets{root};
        const bool tooltipCycle =
            (player.hotbar.hotbarTooltipLastGameTick != 0 &&
                (ticks - player.hotbar.hotbarTooltipLastGameTick) < TICKS_PER_SECOND * 2) ||
            (player.hotbar.useHotbarFaceMenu &&
                player.hotbar.faceMenuButtonHeld != Player::Hotbar_t::GROUP_NONE);
        // Match vanilla's hotbar tooltip context. Inventory/menu tooltips are
        // a different presentation and must never be copied onto the belt.
        if (player.shootmode && tooltipCycle && player.hotbar.animHide <= 0.01 &&
            !FollowerMenu[clientnum].followerMenuIsOpen() &&
            !CalloutMenu[clientnum].calloutMenuIsOpen() &&
            !inputs.getUIInteraction(clientnum)->selectedItem && !player.GUI.isDropdownActive())
        {
            auto& inventory = player.inventoryUI;
            Frame* tooltip = inventory.itemTooltipDisplay.displayingTitleOnlyTooltip ?
                inventory.titleOnlyTooltipFrame : inventory.tooltipFrame;
            if (vrBeltWidgetVisible(tooltip))
            {
                widgets.push_back(tooltip);
                if (vrBeltWidgetVisible(inventory.tooltipPromptFrame))
                    widgets.push_back(inventory.tooltipPromptFrame);
            }
        }
        int left = reference.x, top = reference.y;
        int right = reference.x + reference.w, bottom = reference.y + reference.h;
        const auto append = [&](SDL_Rect r) {
            const int l = std::max(0, r.x - 4), t = std::max(0, r.y - 4);
            const int rr = std::min(Frame::virtualScreenX, r.x + r.w + 4);
            const int bb = std::min(Frame::virtualScreenY, r.y + r.h + 4);
            if (r.w <= 0 || r.h <= 0 || rr <= l || bb <= t) return;
            g_vrNativeBeltWidgetRects.push_back({l,t,rr-l,bb-t});
            left = std::min(left,l); top = std::min(top,t);
            right = std::max(right,rr); bottom = std::max(bottom,bb);
        };
        // Include real child frames, images and fields, not full-screen layout
        // containers. The normal Frame draw still owns clipping/justification.
        const auto visit = [&](auto&& self, Frame* frame, int depth) -> void {
            if (!vrBeltWidgetVisible(frame) || depth > 12) return;
            const SDL_Rect rect = frame->getAbsoluteSize();
            if (rect.w < Frame::virtualScreenX * 9 / 10 ||
                rect.h < Frame::virtualScreenY * 9 / 10)
                append(rect);
            const SDL_Rect scroll = frame->getActualSize();
            for (const auto* image : frame->getImages())
            {
                if (!image || image->disabled || image->path.empty()) continue;
                int w = image->pos.w, h = image->pos.h;
                if (w <= 0 || h <= 0)
                {
                    const auto* artwork = Image::get(image->path.c_str());
                    if (!artwork) continue;
                    if (w <= 0) w = artwork->getWidth();
                    if (h <= 0) h = artwork->getHeight();
                }
                append({rect.x + image->pos.x - scroll.x,
                    rect.y + image->pos.y - scroll.y, w, h});
            }
            for (const auto* field : frame->getFields())
                if (field && !field->isDisabled() && !field->isInvisible() &&
                    field->getText() && field->getText()[0])
                    append(field->getAbsoluteSize());
            for (auto* child : frame->getFrames()) self(self,child,depth+1);
        };
        for (auto* widget : widgets) visit(visit,widget,0);
        area = {left,top,right-left,bottom-top};
        const bool changed = owner != root || canvasWidth != Frame::virtualScreenX ||
            canvasHeight != Frame::virtualScreenY || previousArea.x != area.x ||
            previousArea.y != area.y || previousArea.w != area.w || previousArea.h != area.h;
        if (changed)
        {
            owner = root; canvasWidth = Frame::virtualScreenX; canvasHeight = Frame::virtualScreenY;
            previousArea = area;
            attempted = pixelVerified = native.ready = false;
        }
        if (!attempted || lastFrame != g_xrPendingDisplayTime)
        {
            attempted = true; lastFrame = g_xrPendingDisplayTime;
            const bool rendered = VrNativeHud::render(scratch,target,area,
                canvasWidth,canvasHeight,[&widgets]() {
                    struct DrawScope {
                        bool previous = drawingGui;
                        DrawScope() { drawingGui = true; Shader::unbind(); }
                        ~DrawScope() { drawingGui = previous; Shader::unbind(); }
                    } scope;
                    for (auto* widget : widgets) widget->drawForVr();
                });
            if (rendered && !pixelVerified) pixelVerified = vrNativeSkillHasPixels(target);
            if (!rendered) pixelVerified = false;
            native.texture = target.texture; native.framebuffer = target.framebuffer;
            native.width = target.width; native.height = target.height;
            native.ready = rendered && pixelVerified;
        }
        if (native.ready)
        {
            g_vrNativeBeltReady = true;
            if (lastMode != 1 || changed)
                printlog("[VR GA11] native belt verified: area=(%d %d %d %d) reference=(%d %d %d %d) widgets=%d texture=(%d %d)",
                    area.x,area.y,area.w,area.h,reference.x,reference.y,reference.w,reference.h,
                    static_cast<int>(widgets.size()),native.width,native.height);
            lastMode = 1;
            return &native;
        }
    }
    else attempted = pixelVerified = native.ready = false;

    area = reference;
    g_vrNativeBeltWidgetRects.clear();
    if (lastMode != 0)
        printlog("[VR GA11] belt: retained legacy fallback (%s)",
            *cvarVrNativeBelt ? "native unavailable/blank" : "console toggle off");
    lastMode = 0;
    auto& legacy = g_vrWearableCaptured[3];
    if (vrCaptureOriginalHudFrame(3,reference) && legacy.ready && legacy.texture &&
        legacy.width > 0 && legacy.height > 0)
        return &legacy;
    return nullptr;
}

// GA10: Log only state transitions to diagnose dungeon-entry HUD gaps.
// This observes existing gates; it does not override vanilla visibility.
static void vrLogBeltHudGate(bool hasEye)
{
    static int previous = -1;
    const bool valid = clientnum >= 0 && clientnum < MAXPLAYERS && players[clientnum];
    const auto* player = valid ? players[clientnum] : nullptr;
    const auto* hotbar = player ? player->hotbar.hotbarFrame : nullptr;
    const int state = (hasEye ? 1 : 0) | (intro ? 2 : 0) | (gamePaused ? 4 : 0) |
        (player ? 8 : 0) | (player && player->entity ? 16 : 0) |
        (player && player->shootmode ? 32 : 0) |
        (player && player->hud.weapon ? 64 : 0) | (hotbar ? 128 : 0) |
        (hotbar && hotbar->isDisabled() ? 256 : 0) |
        (hotbar && hotbar->isInvisible() ? 512 : 0) |
        (hotbar && hotbar->getOpacity() > 0.0 ? 1024 : 0);
    if (state == previous) return;
    previous = state;
    printlog("[VR GA10] belt gate: state=%d intro=%d paused=%d player=%d entity=%d shoot=%d marker=%d hotbar=%d disabled=%d invisible=%d opacity=%.1f",
        state, intro ? 1 : 0, gamePaused ? 1 : 0, player ? 1 : 0,
        player && player->entity ? 1 : 0, player && player->shootmode ? 1 : 0,
        player && player->hud.weapon ? 1 : 0, hotbar ? 1 : 0,
        hotbar && hotbar->isDisabled() ? 1 : 0, hotbar && hotbar->isInvisible() ? 1 : 0,
        hotbar ? static_cast<double>(hotbar->getOpacity()) : -1.0);
}

static void vrDrawSpatialHud(view_t* eyeCamera)
{
    vrLogBeltHudGate(eyeCamera != nullptr);
    if (!eyeCamera || intro || gamePaused ||
        clientnum < 0 || clientnum >= MAXPLAYERS ||
        !players[clientnum] || !players[clientnum]->entity ||
        !players[clientnum]->shootmode) return;
    Entity* marker = players[clientnum]->hud.weapon;
    if (!marker) return;
    const double oldX = marker->x, oldY = marker->y, oldZ = marker->z;
    const double oldYaw = marker->yaw, oldPitch = marker->pitch, oldRoll = marker->roll;
    const double oldSX = marker->scalex, oldSY = marker->scaley, oldSZ = marker->scalez;
    const double oldFX = marker->focalx, oldFY = marker->focaly, oldFZ = marker->focalz;
    const bool oldInvisible = marker->flags[INVISIBLE];
    const bool oldOverdraw = marker->flags[OVERDRAW];
    const float yaw = static_cast<float>(eyeCamera->ang);
    const float c = cosf(yaw), s = sinf(yaw);
    // The eye camera shares the Barony world-space coordinate system used
    // for the verified tracked arm panels. Offset the center of the stereo
    // head, not either individual controller or the native inventory frame.
    const float headX = static_cast<float>(eyeCamera->x * 16.0);
    const float headY = static_cast<float>(eyeCamera->y * 16.0);
    const float headZ = static_cast<float>((eyeCamera->z - 1.0) * .5);
    const float defaultForward[3] = { 8.5f, 13.0f, 14.0f };
    const float defaultUp[3] = { -7.5f, -2.2f, -4.8f };
    const float defaultSide[3] = { 0.f, 0.f, -3.8f };
    const float width[3] = { 6.0f, 4.0f, 5.5f };
    for (int index = 0; index < 3; ++index)
    {
        // GA7: No separate head-space dungeon-message surface. Native chat
        // follows the belt below, independently of its fixed capture bounds.
        if (index == 2) continue;
        SDL_Rect area = vrSpatialHudRect(index, false);
        const SDL_Rect beltOnly = area;
        const auto* beltPanel = index == 0 ? vrPrepareNativeBelt(beltOnly, area) : nullptr;
        const bool nativeBelt = index == 0 && g_vrNativeBeltReady;
        // Dungeon messages no longer use a forward calibration page.
        const bool messagePreview = false;
        // Once per change, report whether a native widget rectangle,
        // framebuffer capture and stereo draw are actually available.
        static int lastStatus[3] = { -1, -1, -1 };
        int status = 0;
        auto& img = g_vrWearableCaptured[index + 3];
        // GA11: Select AFTER preparation: the first native/captured draw may
        // have just allocated its texture and dimensions.
        GLuint drawTexture = beltPanel ? beltPanel->texture : img.texture;
        int drawWidth = beltPanel ? beltPanel->width : img.width;
        int drawHeight = beltPanel ? beltPanel->height : img.height;
        if (index == 0)
        {
            if (!beltPanel || !beltPanel->ready || !drawTexture || drawWidth <= 0 || drawHeight <= 0)
                status = 1;
        }
        else if (messagePreview)
        {
            if (!vrPrepareSpatialTextLabel(g_vrDungeonCalibrationMessage,
                "Dungeon messages: calibration preview"))
                status = 3;
            else
            {
                drawTexture = g_vrDungeonCalibrationMessage.texture;
                drawWidth = g_vrDungeonCalibrationMessage.width;
                drawHeight = g_vrDungeonCalibrationMessage.height;
            }
        }
        else
        {
            if (area.w <= 0 || area.h <= 0)
            {
                img.ready = false;
                status = 1; // no visible native widgets in this frame
            }
            else if (!vrCaptureOriginalHudFrame(index + 3, area))
                status = 2; // GUI framebuffer capture unavailable
            if (!status && (!img.ready || !img.texture))
                status = 3; // incomplete capture
        }
        if (index == 0 && lastStatus[index] != (status ? status : 4))
            printlog("[VR GA11] belt surface: status=%d native=%d bounds=(%d %d %d %d) selected=(%u %d %d)",
                status, nativeBelt ? 1 : 0, area.x, area.y, area.w, area.h,
                static_cast<unsigned>(drawTexture), drawWidth, drawHeight);
        if (status)
        {
            if (lastStatus[index] != status)
                std::printf("OpenXR VR STEP638FG SPATIAL %d unavailable code=%d area=(%d %d %d %d)\n",
                    index, status, area.x, area.y, area.w, area.h);
            lastStatus[index] = status;
            continue;
        }
        if (lastStatus[index] != 4)
            std::printf("OpenXR VR STEP638FG SPATIAL %d DRAWING area=(%d %d %d %d) texture=%u\n",
                index, area.x, area.y, area.w, area.h,
                static_cast<unsigned>(drawTexture));
        lastStatus[index] = 4;
        const auto& h = g_vrSpatialHud[index];
        const float forward = defaultForward[index] + h.forward;
        const float sideways = defaultSide[index] + h.side;
        marker->x = headX + forward * c - sideways * s;
        marker->y = headY + forward * s + sideways * c;
        marker->z = headZ - (defaultUp[index] + h.up);
        marker->yaw = yaw + h.yaw;
        marker->pitch = h.pitch;
        marker->roll = h.roll;
        // STEP638FK: Keep the exact Step638FI body-root belt attachment
        // with legacy tracked VR arms and no rendered player body.
        // STEP638FI: the belt follows the real player root yaw and torso,
        // NOT where the player turns their head. This is simulated hips,
        // not actual waist tracking, but brings the belt within arm's reach.
        if (index == 0)
        {
            Entity* playerEntity = players[clientnum]->entity;
            Entity* torso = vrFindNativePlayerLimb(playerEntity, 1);
            if (torso)
            {
                const float bodyYaw = static_cast<float>(playerEntity->yaw);
                const float bc = cosf(bodyYaw), bs = sinf(bodyYaw);
                // Clamp the minimum front clearance to avoid torso clipping.
                // Previously saved negative depth calibration remains valid.
                const float beltForward = std::max(1.6f, 3.4f + h.forward);
                marker->x = playerEntity->x + beltForward * bc - h.side * bs;
                marker->y = playerEntity->y + beltForward * bs + h.side * bc;
                marker->z = torso->z + 2.4f - h.up;
                marker->yaw = bodyYaw + h.yaw;
            }
        }
        // STEP638FZ7: Changing capture bounds must not resize the player's
        // calibrated hotbar. Compute one stable world-space unit per ORIGINAL
        // hotbar pixel, independent of the chat length or 512px downsample.
        const double beltBaseX = marker->x, beltBaseY = marker->y, beltBaseZ = marker->z;
        float size = index == 0 && beltOnly.w > 0 ?
            width[0] * h.scale * static_cast<float>(area.w) /
                (static_cast<float>(beltOnly.w) * drawWidth) :
            width[index] * h.scale / static_cast<float>(drawWidth);
        float beltChatHorizontalShift = 0.f;
        if (index == 0 && beltOnly.w > 0)
        {
            // Keep the ORIGINAL hotbar centered where its calibration put
            // it when the wider text changes the captured rectangle center.
            const float deltaPixels =
                (area.x + area.w * .5f) - (beltOnly.x + beltOnly.w * .5f);
            beltChatHorizontalShift = deltaPixels * width[0] * h.scale /
                static_cast<float>(beltOnly.w);
            const VrGripBasis3 beltBasis = vrGripEulerBasis(
                static_cast<float>(marker->yaw),
                static_cast<float>(marker->pitch),
                static_cast<float>(marker->roll));
            const VrGripVector3 horizontal = vrGripApplyBasis(beltBasis,
                // UI quad lies in local X/Y; entity translations are doubled.
                // Keep the retained legacy capture path unchanged.
                nativeBelt ? VrGripVector3{.5f * beltChatHorizontalShift, 0.f, 0.f} :
                    VrGripVector3{0.f, 0.f, beltChatHorizontalShift});
            marker->x += horizontal.x;
            marker->y += horizontal.z;
            marker->z -= horizontal.y;
        }
        // GA11: Keep a fixed reference plane for the calibrated slots. The
        // native target may grow in any direction to contain a long tooltip.
        // Position its expanded canvas relative to that plane without moving
        // the slots or allowing texture downsampling to change their scale.
        VrGripVector3 nativeReferenceOffset{0.f,0.f,0.f};
        if (index == 0)
        {
            int slotTop = Frame::virtualScreenY;
            for (int slot = 0; slot < NUM_HOTBAR_SLOTS; ++slot)
            {
                Frame* const frame = players[clientnum]->hotbar.getHotbarSlotFrame(slot);
                if (frame && !frame->isDisabled() && !frame->isInvisible())
                    slotTop = std::min(slotTop, frame->getAbsoluteSize().y);
            }
            if (slotTop < Frame::virtualScreenY)
            {
                const int priorCaptureTop = std::max(0, slotTop - 40);
                const VrGripBasis3 surfaceBasis = vrGripEulerBasis(
                    static_cast<float>(marker->yaw),
                    static_cast<float>(marker->pitch),
                    static_cast<float>(marker->roll));
                if (nativeBelt)
                {
                    const float units = width[0] * h.scale / static_cast<float>(beltOnly.w);
                    const float referenceUp = .5f * std::max(0, priorCaptureTop - beltOnly.y) * units;
                    nativeReferenceOffset = vrGripApplyBasis(surfaceBasis,{0.f,referenceUp,0.f});
                    const float centerUp = (beltOnly.y + beltOnly.h * .5f - area.y - area.h * .5f) * units;
                    const auto up = vrGripApplyBasis(surfaceBasis,{0.f,referenceUp + .5f * centerUp,0.f});
                    marker->x += up.x;
                    marker->y += up.z;
                    marker->z -= up.y;
                }
                else
                {
                    // Retained GA10/GA7 alignment for the fallback.
                    const int extraAbove = std::max(0, priorCaptureTop - area.y);
                    const auto up = vrGripApplyBasis(surfaceBasis,{0.f,.5f * extraAbove * size,0.f});
                    marker->x += up.x;
                    marker->y += up.z;
                    marker->z -= up.y;
                }
            }
        }
        marker->scalex = marker->scaley = marker->scalez = size;
        marker->focalx = marker->focaly = marker->focalz = 0.0;
        marker->flags[INVISIBLE] = false;
        marker->flags[OVERDRAW] = false;
        // GA11R1: packing rounds width and height independently. Preserve
        // native pixel scale on both axes, including downsampled canvases.
        if (nativeBelt)
            marker->scaley = width[0] * h.scale * static_cast<float>(area.h) /
                (static_cast<float>(beltOnly.w) * drawHeight);
        glDrawVrUiTexture(eyeCamera, marker, drawTexture, drawWidth, drawHeight);
        if (nativeBelt)
        {
            // Restore the reference plane before drawing existing chat, XP
            // and calibrated skill panels. Their anchors must not depend on
            // which tooltip is open or how far its text extends left/right.
            marker->x = beltBaseX + nativeReferenceOffset.x;
            marker->y = beltBaseY + nativeReferenceOffset.z;
            marker->z = beltBaseZ - nativeReferenceOffset.y;
            drawWidth = beltOnly.w;
            drawHeight = beltOnly.h;
            size = width[0] * h.scale / static_cast<float>(beltOnly.w);
            beltChatHorizontalShift = 0.f;
            marker->scalex = marker->scaley = marker->scalez = size;
        }
        // GA7: Clean belt art remains intact; native messages attach above
        // the belt and never change its frame dimensions.
        // STEP638FZ2: No synthesized item-name label; vanilla belt art
        // remains unchanged. The independent XP copy stays below the slots.
        if (index == 0)
        {
            // GA7: The game's EXISTING dungeon-message artwork is on its
            // own live texture immediately ABOVE the calibrated waist belt.
            // It follows body yaw/position and never changes the hotbar's
            // source crop, width, orientation, or XP location.
            SDL_Rect chatArea = vrSpatialHudRect(2, false);
            auto& chatPanel = g_vrWearableCaptured[5];
            if (chatArea.w > 0 && chatArea.h > 0 &&
                vrCaptureOriginalHudFrame(5, chatArea) &&
                chatPanel.ready && chatPanel.texture && chatPanel.width > 0)
            {
                const double beltX = marker->x, beltY = marker->y,
                    beltZ = marker->z;
                const double beltYaw = marker->yaw, beltPitch = marker->pitch,
                    beltRoll = marker->roll;
                const float messageScale = width[0] * h.scale * .94f /
                    static_cast<float>(chatPanel.width);
                const VrGripBasis3 beltBasis = vrGripEulerBasis(
                    static_cast<float>(beltYaw),
                    static_cast<float>(beltPitch),
                    static_cast<float>(beltRoll));
                const VrGripVector3 up = vrGripApplyBasis(beltBasis,
                    {0.f, .5f * (drawHeight * size +
                        chatPanel.height * messageScale) + .17f, 0.f});
                marker->x += up.x;
                marker->y += up.z;
                marker->z -= up.y;
                marker->scalex = marker->scaley = marker->scalez = messageScale;
                glDrawVrUiTexture(eyeCamera, marker, chatPanel.texture,
                    chatPanel.width, chatPanel.height);
                marker->x = beltX; marker->y = beltY; marker->z = beltZ;
                marker->yaw = beltYaw; marker->pitch = beltPitch;
                marker->roll = beltRoll;
                marker->scalex = marker->scaley = marker->scalez = size;
            }
            else chatPanel.ready = false;
            auto& hud = players[clientnum]->hud;
            auto& xp = g_vrWearableCaptured[6];
            // Capture only the native XP Frame, retaining its original
            // progress values, levels, colors, and artwork.
            if (hud.xpFrame && !hud.xpFrame->isDisabled() &&
                !hud.xpFrame->isInvisible() &&
                vrCaptureOriginalHudFrame(6, hud.xpFrame->getAbsoluteSize()) &&
                xp.texture && xp.ready)
            {
                const auto& cfg = g_vrSpatialHud[3];
                const VrGripBasis3 beltBasis = vrGripEulerBasis(
                    static_cast<float>(marker->yaw),
                    static_cast<float>(marker->pitch),
                    static_cast<float>(marker->roll));
                const VrGripBasis3 xpEuler = vrGripEulerBasis(
                    cfg.yaw, cfg.pitch, cfg.roll);
                const VrGripBasis3 xpBasis{
                    vrGripApplyBasis(beltBasis, xpEuler.forward),
                    vrGripApplyBasis(beltBasis, xpEuler.up),
                    vrGripApplyBasis(beltBasis, xpEuler.right) };
                // Preserve the belt's pixel-to-world ratio. By default the
                // XP image is 75% of the hotbar's width and centered under it.
                const float originalBeltWidth = width[0] * h.scale;
                const float xpSize = originalBeltWidth * .75f * cfg.scale /
                    static_cast<float>(xp.width);
                const float below = -0.5f *
                    (drawHeight * size + xp.height * xpSize) - .10f;
                const VrGripVector3 delta = vrGripApplyBasis(beltBasis,
                    {cfg.forward + .02f, below + cfg.up,
                     cfg.side - beltChatHorizontalShift});
                const double xpX = marker->x + delta.x;
                const double xpY = marker->y + delta.z;
                const double xpZ = marker->z - delta.y;
                marker->x = xpX;
                marker->y = xpY;
                marker->z = xpZ;
                vrGripBasisToBaronyAngles(xpBasis,
                    static_cast<float>(marker->yaw),
                    marker->yaw, marker->pitch, marker->roll);
                marker->scalex = marker->scaley = marker->scalez = xpSize;
                glDrawVrUiTexture(eyeCamera, marker, xp.texture,
                    xp.width, xp.height);
                // GA10: Native skill frame, independently calibrated relative to XP.
                const auto* skill = vrPrepareSkillUpPanel();
                if (!skill && g_vrArmCalibrationActive && !g_vrBodyCalibrationPage &&
                    g_vrArmCalibrationSpatialPage == 5 && g_vrSkillPreviewPanel.ready &&
                    g_vrSkillPreviewPlayer == clientnum)
                    skill = &g_vrSkillPreviewPanel;
                if (skill && skill->ready && skill->texture && skill->width > 0)
                {
                    const auto& skillCfg = g_vrSpatialHud[5];
                    // Larger than GA9, with a closer default. Existing XP and
                    // belt transforms retain their exact saved meanings.
                    const float skillScale = (xp.width * xpSize * 1.44f * skillCfg.scale) /
                        static_cast<float>(skill->width);
                    const VrGripBasis3 xpDrawBasis = vrGripEulerBasis(
                        static_cast<float>(marker->yaw),
                        static_cast<float>(marker->pitch),
                        static_cast<float>(marker->roll));
                    const VrGripVector3 offset = vrGripApplyBasis(xpDrawBasis,
                        {skillCfg.forward, -0.5f * (xp.height * xpSize +
                            skill->height * skillScale) - .04f + .45f + skillCfg.up,
                            skillCfg.side});
                    marker->x += offset.x;
                    marker->y += offset.z;
                    marker->z -= offset.y;
                    const VrGripBasis3 extra = vrGripEulerBasis(
                        skillCfg.yaw, skillCfg.pitch, skillCfg.roll);
                    const VrGripBasis3 skillBasis{
                        vrGripApplyBasis(xpDrawBasis, extra.forward),
                        vrGripApplyBasis(xpDrawBasis, extra.up),
                        vrGripApplyBasis(xpDrawBasis, extra.right)};
                    vrGripBasisToBaronyAngles(skillBasis,
                        static_cast<float>(marker->yaw),
                        marker->yaw, marker->pitch, marker->roll);
                    marker->scalex = marker->scaley = marker->scalez = skillScale;
                    glDrawVrUiTexture(eyeCamera, marker, skill->texture,
                        skill->width, skill->height);
                }
            }
            else xp.ready = false;
        }
    }
    // GA7: Reuse native party artwork while applying a SAVED independent
    // position, rotation and size from calibration spatial page 4.
    const auto& party = g_vrSpatialHud[4];
    float nativePanelTop = 4.7f;
    for (int panelIndex = 7; panelIndex <= 8; ++panelIndex)
    {
        SDL_Rect nativeArea{};
        auto& panel = g_vrWearableCaptured[panelIndex];
        if (!vrNativeAllyArtArea(panelIndex, nativeArea) ||
            !vrCaptureOriginalHudFrame(panelIndex, nativeArea) ||
            !panel.ready || !panel.texture || panel.width <= 0)
        {
            panel.ready = false;
            continue;
        }
        const float scale = 7.2f * party.scale /
            static_cast<float>(panel.width);
        const float height = panel.height * scale;
        const float forward = 14.f + party.forward;
        const float sideways = -8.0f + party.side;
        marker->x = headX + forward * c - sideways * s;
        marker->y = headY + forward * s + sideways * c;
        marker->z = headZ - (nativePanelTop + party.up - height * .5f);
        marker->yaw = yaw + party.yaw;
        marker->pitch = party.pitch;
        marker->roll = party.roll;
        marker->scalex = marker->scaley = marker->scalez = scale;
        marker->focalx = marker->focaly = marker->focalz = 0.0;
        marker->flags[INVISIBLE] = false;
        marker->flags[OVERDRAW] = false;
        glDrawVrUiTexture(eyeCamera, marker, panel.texture,
            panel.width, panel.height);
        nativePanelTop -= height + .35f;
    }
    marker->x = oldX; marker->y = oldY; marker->z = oldZ;
    marker->yaw = oldYaw; marker->pitch = oldPitch; marker->roll = oldRoll;
    marker->scalex = oldSX; marker->scaley = oldSY; marker->scalez = oldSZ;
    marker->focalx = oldFX; marker->focaly = oldFY; marker->focalz = oldFZ;
    marker->flags[INVISIBLE] = oldInvisible;
    marker->flags[OVERDRAW] = oldOverdraw;
}

// STEP638FZ6: Locate the drawable children of the native skill/level-up
// notifications. Their parent HUD frames can be full-screen containers, so
// only protect small, visible widget rectangles from the VR GUI clear.
static std::vector<SDL_Rect> vrTransientProgressAlertRects(
    bool includeSkill, bool includeLevel)
{
    std::vector<SDL_Rect> rects;
    if (clientnum < 0 || clientnum >= MAXPLAYERS || !players[clientnum] ||
        Frame::virtualScreenX <= 0 || Frame::virtualScreenY <= 0)
        return rects;
    const int vw = Frame::virtualScreenX, vh = Frame::virtualScreenY;
    const auto add = [&](const SDL_Rect& source) {
        // GA4: Every visible alert child may grow while stat rows animate.
        // Exclude genuine full-screen layout containers, not the tall or
        // wide populated alert contents, and preserve generous text padding.
        if (source.w <= 0 || source.h <= 0 ||
            source.w >= vw * 9 / 10 || source.h >= vh * 9 / 10) return;
        const int x1 = std::max(0, source.x - 22);
        const int y1 = std::max(0, source.y - 22);
        const int x2 = std::min(vw, source.x + source.w + 22);
        const int y2 = std::min(vh, source.y + source.h + 22);
        if (x2 > x1 && y2 > y1)
            rects.push_back({ x1, y1, x2-x1, y2-y1 });
    };
    const auto visit = [&](auto&& self, Frame* frame, int depth) -> void {
        if (!frame || frame->isDisabled() || frame->isInvisible() || depth > 5) return;
        const SDL_Rect parent = frame->getAbsoluteSize();
        bool drawable = false;
        for (const auto* img : frame->getImages()) {
            if (!img || img->disabled || img->path.empty() ||
                img->pos.w <= 0 || img->pos.h <= 0) continue;
            add({ parent.x + img->pos.x, parent.y + img->pos.y,
                  img->pos.w, img->pos.h });
            drawable = true;
        }
        for (Field* field : frame->getFields()) {
            if (!field || field->isDisabled() || field->isInvisible() ||
                !field->getText() || !field->getText()[0]) continue;
            SDL_Rect r = field->getAbsoluteSize();
            // Native Field bounds may lag behind an animated level-up
            // stat string. Extend to measured glyph width before masking.
            if (auto* glyphs = field->getTextObject()) {
                const int glyphWidth = static_cast<int>(glyphs->getWidth()) + 32;
                if (glyphWidth > r.w && glyphWidth < vw) {
                    if (field->getHJustify() == Field::justify_t::CENTER)
                        r.x -= (glyphWidth-r.w)/2;
                    r.w = glyphWidth;
                }
            }
            r.h = std::max(r.h, 34);
            add(r);
            drawable = true;
        }
        const auto& children = frame->getFrames();
        if (depth > 0 && (drawable || children.empty())) add(parent);
        for (Frame* child : children) self(self, child, depth+1);
        if (depth == 0 && children.empty() && drawable) add(parent);
    };
    auto& hud = players[clientnum]->hud;
    if (includeSkill) visit(visit, hud.skillupFrame, 0);
    if (includeLevel) visit(visit, hud.levelupFrame, 0);
    return rects;
}

// STEP638EL (combined): Hide duplicate flat HP/MP and minimap visuals
// ONLY on the floating VR overlay. Never change Frame::isInvisible() or
// the vanilla GUI source texture: the wearable capture must keep seeing
// the original Barony art, numbers, marker positions, and fog of war.
static void vrMaskOriginalWearableHudOnGui()
{
    if (!g_xrStereoFrameReady || !g_xrGuiFramebuffer || intro || gamePaused ||
        clientnum < 0 || clientnum >= MAXPLAYERS ||
        !players[clientnum] ||
        Frame::virtualScreenX <= 0 || Frame::virtualScreenY <= 0)
        return;
    // STEP638FZ6: Never clear/mask the native death menu even when
    // deathcam shootmode is still true. Leave its input and layout untouched.
    if (players[clientnum]->GUI.isGameoverActive()) return;
    // STEP638FZ5: Menus can set shootmode=false. In that case mask ONLY
    // the duplicate chat from the floating GUI; keep interactive menu,
    // belt and other native widgets intact.
    const bool menuWithoutShootmode = !players[clientnum]->shootmode;

    // STEP638FZ4: Keep the vanilla GUI framebuffer UNCHANGED so the
    // separately calibrated belt, wrist and dungeon-message panels can
    // still sample Barony's original art. Mask only the *OpenXR GUI copy*.
    // During gameplay, all of the ordinary flat HUD must disappear from
    // the headset -- not merely the small rectangles used for captures.
    // Leave actual interactive menus and radial wheels accessible.
    const bool nativeRadialOpen =
        ((FollowerMenu[clientnum].followerToCommand &&
          !FollowerMenu[clientnum].selectMoveTo) ||
         (CalloutMenu[clientnum].bOpen &&
          !CalloutMenu[clientnum].selectMoveTo));
    if (!menuWithoutShootmode && !vrMenuPointerMode() && !nativeRadialOpen)
    {
        GLint oldDrawFramebuffer = 0;
        GLint oldScissorRect[4] = {};
        GLboolean oldColorMask[4] = {};
        GLfloat oldClearColor[4] = {};
        const GLboolean oldScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFramebuffer);
        glGetIntegerv(GL_SCISSOR_BOX, oldScissorRect);
        glGetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, oldClearColor);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_xrGuiFramebuffer);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        // FZ4 cleared the ENTIRE gameplay GUI, including skill-up alerts.
        // Subtract only the visible alert widgets from the clear regions.
        // This retains the original animated popup without restoring a
        // duplicate flat belt, wrist HUD, or dungeon chat surface.
        std::vector<SDL_Rect> clearRects{ { 0, 0, 1280, 720 } };
        // Preserve the vanilla character LEVEL-UP announcement ahead of
        // the player. Skill-up artwork is now rendered ONLY below belt XP.
        for (const SDL_Rect& source : vrTransientProgressAlertRects(false, true))
        {
            const int x1 = std::max(0, std::min(1280,
                static_cast<int>(std::floor(source.x * 1280.0 / Frame::virtualScreenX))));
            const int x2 = std::max(0, std::min(1280,
                static_cast<int>(std::ceil((source.x + source.w) * 1280.0 / Frame::virtualScreenX))));
            const int y1 = std::max(0, std::min(720,
                static_cast<int>(std::floor(source.y * 720.0 / Frame::virtualScreenY))));
            const int y2 = std::max(0, std::min(720,
                static_cast<int>(std::ceil((source.y + source.h) * 720.0 / Frame::virtualScreenY))));
            if (x2 <= x1 || y2 <= y1) continue;
            std::vector<SDL_Rect> next;
            for (const SDL_Rect& r : clearRects)
            {
                const int left = std::max(r.x, x1), right = std::min(r.x+r.w, x2);
                const int top = std::max(r.y, y1), bottom = std::min(r.y+r.h, y2);
                if (right <= left || bottom <= top) { next.push_back(r); continue; }
                if (top > r.y) next.push_back({ r.x, r.y, r.w, top-r.y });
                if (bottom < r.y+r.h) next.push_back({ r.x, bottom, r.w, r.y+r.h-bottom });
                if (left > r.x) next.push_back({ r.x, top, left-r.x, bottom-top });
                if (right < r.x+r.w) next.push_back({ right, top, r.x+r.w-right, bottom-top });
            }
            clearRects.swap(next);
        }
        glEnable(GL_SCISSOR_TEST);
        for (const SDL_Rect& r : clearRects)
        {
            if (r.w <= 0 || r.h <= 0) continue;
            glScissor(r.x, 720-r.y-r.h, r.w, r.h);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glClearColor(oldClearColor[0], oldClearColor[1],
            oldClearColor[2], oldClearColor[3]);
        glColorMask(oldColorMask[0], oldColorMask[1],
            oldColorMask[2], oldColorMask[3]);
        glScissor(oldScissorRect[0], oldScissorRect[1],
            oldScissorRect[2], oldScissorRect[3]);
        if (oldScissorEnabled) glEnable(GL_SCISSOR_TEST);
        else glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
            static_cast<GLuint>(oldDrawFramebuffer));
        return;
    }

    // On radial-menu frames, retain Barony's selectable wheel artwork.
    // Use the previous selective mask to hide migrated HUD components.
    auto& hud = players[clientnum]->hud;
    const bool hideLeft = !menuWithoutShootmode &&
        g_xrControllerHands[0].gripPoseValid &&
        g_vrWearableCaptured[0].ready && hud.hpFrame && hud.mpFrame;
    const bool hideRight = !menuWithoutShootmode &&
        g_xrControllerHands[1].gripPoseValid &&
        g_vrWearableCaptured[1].ready && hud.minimapFrame &&
        !hud.minimapFrame->isInvisible();
    // Independent belt panels can be valid even when a wrist loses tracking.
    const bool hideBelt = !menuWithoutShootmode &&
        (g_vrNativeBeltReady || g_vrWearableCaptured[3].ready);
    // STEP638FZ5: On inventory/radial/other GUI frames, hide the duplicate
    // native message and world-prompt text whenever their calibrated VR
    // surfaces are available. Do NOT require wrist/waist captures first.
    // Previously a missing wrist or belt caused the selective masking
    // path to return early, allowing far-away floating message duplicates.
    const bool hideForwardText = !menuWithoutShootmode &&
        g_vrWearableCaptured[4].ready && vrSpatialHudRect(1).w > 0;
    // Mask the old chat in the distant 2D GUI copy even though its separate
    // spatial capture is retired. Never touch the vanilla source framebuffer;
    // belt capture still contains the original messages above the hotbar.
    const SDL_Rect dungeonChatRect = vrSpatialHudRect(2);
    const bool hideDungeonText = dungeonChatRect.w > 0 && dungeonChatRect.h > 0;
    if (!hideLeft && !hideRight && !hideBelt &&
        !hideForwardText && !hideDungeonText) return;

    // The OpenXR GUI layer is always 1280x720; its content is a scaled copy
    // of the full virtual-screen GUI texture. Convert TOP-LEFT SDL frame
    // rectangles to BOTTOM-LEFT OpenGL scissor rectangles.
    constexpr int guiWidth = 1280;
    constexpr int guiHeight = 720;
    const GLboolean wasScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint oldScissorRect[4] = {};
    GLboolean oldColorMask[4] = {};
    GLfloat oldClearColor[4] = {};
    glGetIntegerv(GL_SCISSOR_BOX, oldScissorRect);
    glGetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, oldClearColor);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_xrGuiFramebuffer);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.f, 0.f, 0.f, 0.f);

    const auto mask = [guiWidth, guiHeight](const SDL_Rect& r)
    {
        if (r.w <= 0 || r.h <= 0) return;
        const double scaleX = static_cast<double>(guiWidth) /
            Frame::virtualScreenX;
        const double scaleY = static_cast<double>(guiHeight) /
            Frame::virtualScreenY;
        const auto clampX = [guiWidth](int x) { return std::max(0, std::min(guiWidth, x)); };
        const auto clampY = [guiHeight](int y) { return std::max(0, std::min(guiHeight, y)); };
        const int left = clampX(static_cast<int>(std::floor(r.x * scaleX)));
        const int right = clampX(static_cast<int>(std::ceil((r.x + r.w) * scaleX)));
        const int bottom = clampY(static_cast<int>(std::floor(
            (Frame::virtualScreenY - r.y - r.h) * scaleY)));
        const int top = clampY(static_cast<int>(std::ceil(
            (Frame::virtualScreenY - r.y) * scaleY)));
        if (right <= left || top <= bottom) return;
        glScissor(left, bottom, right - left, top - bottom);
        glClear(GL_COLOR_BUFFER_BIT);
    };
    // Clear the two source frames separately: do not erase UI widgets that
    // might be between their rectangles on unusual HUD layouts.
    if (hideLeft)
    {
        if (!hud.hpFrame->isInvisible()) mask(hud.hpFrame->getAbsoluteSize());
        if (!hud.mpFrame->isInvisible()) mask(hud.mpFrame->getAbsoluteSize());
    }
    if (hideRight) mask(hud.minimapFrame->getAbsoluteSize());
    // STEP638FZ2: The XP bar lives on the VR BELT, independently of the
    // wrist controller. Keep the original GUI only for desktop capture.
    if (!menuWithoutShootmode &&
        (g_vrNativeBeltReady || g_vrWearableCaptured[3].ready) && g_vrWearableCaptured[6].ready &&
        hud.xpFrame && !hud.xpFrame->isDisabled() &&
        !hud.xpFrame->isInvisible())
        mask(hud.xpFrame->getAbsoluteSize());
    // STEP638FB: Only when status effects have a valid forearm capture,
    // erase their matching vanilla icon rectangles from the floating VR
    // overlay. The original GUI source texture and desktop HUD are NEVER
    // modified, so icons, counters, flashes and tooltips keep updating.
    if (hideLeft && g_vrWearableCaptured[2].ready)
    {
        for (const auto& icon : vrActiveStatusEffectIconRects())
            mask(icon);
    }
    // For interactive radial-menu frames, selectively hide the relocated
    // native HUD widgets while retaining the wheel on the floating GUI.
    // Gameplay frames use the complete transparent-quad branch above.
    // Keep the underlying vanilla GUI source, desktop and all MENU state.
    for (int index = 0; !menuWithoutShootmode && index < 2; ++index)
    {
        if (index == 0 && g_vrNativeBeltReady)
        {
            // Hide only the migrated widgets in the headset GUI copy, never
            // a large bounding box across unrelated radial/menu artwork.
            for (const auto& rect : g_vrNativeBeltWidgetRects) mask(rect);
            continue;
        }
        if (!g_vrWearableCaptured[index + 3].ready) continue;
        const SDL_Rect r = vrSpatialHudRect(index);
        if (r.w > 0 && r.h > 0) mask(r);
    }
    if (hideDungeonText) mask(dungeonChatRect);

    glClearColor(oldClearColor[0], oldClearColor[1],
        oldClearColor[2], oldClearColor[3]);
    glColorMask(oldColorMask[0], oldColorMask[1],
        oldColorMask[2], oldColorMask[3]);
    glScissor(oldScissorRect[0], oldScissorRect[1],
        oldScissorRect[2], oldScissorRect[3]);
    if (!wasScissor) glDisable(GL_SCISSOR_TEST);
}

// STEP638CZ: Retain Barony's existing flame particle animation, but render
// torch and lantern flames at the fully calibrated, tracked OFFHAND model.
// This does not modify particle simulation or any networked game entity.
static bool vrIsLocalHudLightFlame(const Entity* particle, const Entity* shield)
{
    return particle && shield && particle->behavior == &actFlame &&
        particle->sprite == SPRITE_FLAME && particle->flags[OVERDRAW] &&
        particle->parent == shield->getUID() &&
        particle->skill[11] == clientnum;
}

static void vrDrawTrackedOffhandFlames(view_t* eyeCamera, Entity* trackedItem,
    const Item* equipped, double hudX, double hudY, double hudZ,
    double hudRoll)
{
    if (!eyeCamera || !trackedItem || !equipped || !map.entities ||
        (equipped->type != TOOL_TORCH && equipped->type != TOOL_LANTERN))
        return;

    const bool torch = equipped->type == TOOL_TORCH;
    // STEP638CZ first-pass local offsets. Torch tip extends above its grip;
    // the lantern fire sits below its pivot inside the light chamber.
    // Model-local offsets rotate with the final item angles (including the
    // user's saved per-item calibration) and scale with the model itself.
    const float localHeight = (torch ? 2.5f : -1.0f) *
        static_cast<float>(trackedItem->scalex);
    const VrGripBasis3 orientation = vrGripEulerBasis(
        static_cast<float>(trackedItem->yaw),
        static_cast<float>(trackedItem->pitch),
        static_cast<float>(trackedItem->roll));
    const VrGripVector3 offset = vrGripApplyBasis(
        orientation, { 0.0f, localHeight, 0.0f });
    const float anchorX = static_cast<float>(trackedItem->x) + offset.x;
    const float anchorY = static_cast<float>(trackedItem->y) + offset.z;
    const float anchorZ = static_cast<float>(trackedItem->z) - offset.y;

    // Existing actHudShield flame spawns are referenced to its HUD model.
    // Subtract the original fixed offset to preserve each particle's small
    // flicker/rise motion without preserving its screen-fixed position.
    const float sourceY = static_cast<float>(hudY) +
        (torch ? 2.5f * sinf(static_cast<float>(hudRoll)) : 0.0f);
    const float sourceZ = static_cast<float>(hudZ) +
        (torch ? -2.5f * cosf(static_cast<float>(hudRoll)) : 1.0f);
    const auto clampDrift = [](float value) {
        return std::max(-1.25f, std::min(1.25f, value));
    };

    for (node_t* node = map.entities->first; node; node = node->next)
    {
        Entity* flame = static_cast<Entity*>(node->element);
        if (!vrIsLocalHudLightFlame(flame, trackedItem))
            continue;
        const double oldX = flame->x, oldY = flame->y, oldZ = flame->z;
        const double oldScaleX = flame->scalex, oldScaleY = flame->scaley,
            oldScaleZ = flame->scalez;
        const bool oldOverdraw = flame->flags[OVERDRAW];
        const bool oldInvisible = flame->flags[INVISIBLE];
        const bool oldBright = flame->flags[BRIGHT];

        flame->x = anchorX + clampDrift(static_cast<float>(oldX - hudX));
        flame->y = anchorY + clampDrift(static_cast<float>(oldY - sourceY));
        flame->z = anchorZ + clampDrift(static_cast<float>(oldZ - sourceZ));
        flame->scalex = flame->scaley = flame->scalez =
            torch ? 0.30f : 0.22f;
        flame->flags[OVERDRAW] = false;
        flame->flags[INVISIBLE] = false;
        flame->flags[BRIGHT] = true;
        glDrawSprite(eyeCamera, flame, REALCOLORS);

        flame->x = oldX; flame->y = oldY; flame->z = oldZ;
        flame->scalex = oldScaleX;
        flame->scaley = oldScaleY;
        flame->scalez = oldScaleZ;
        flame->flags[OVERDRAW] = oldOverdraw;
        flame->flags[INVISIBLE] = oldInvisible;
        flame->flags[BRIGHT] = oldBright;
    }
}

// Suppress only locally-owned HUD flames during stereo world passes. Their
// original visibility is restored before returning to Barony desktop draws.
static std::vector<std::pair<Entity*, bool>> vrHideOriginalHudLightFlames()
{
    std::vector<std::pair<Entity*, bool>> hidden;
    if (!map.entities || clientnum < 0 || clientnum >= MAXPLAYERS ||
        !stats[clientnum] || !stats[clientnum]->shield ||
        !g_xrControllerHands[0].gripPoseValid)
        return hidden;
    const auto type = stats[clientnum]->shield->type;
    if (type != TOOL_TORCH && type != TOOL_LANTERN)
        return hidden;
    Entity* shield = vrFindLocalHudShield();
    if (!shield)
        return hidden;
    for (node_t* node = map.entities->first; node; node = node->next)
    {
        Entity* flame = static_cast<Entity*>(node->element);
        if (vrIsLocalHudLightFlame(flame, shield))
        {
            hidden.emplace_back(flame, flame->flags[INVISIBLE]);
            flame->flags[INVISIBLE] = true;
        }
    }
    return hidden;
}

// STEP638FT: Cosmetic flail motion now drives native 3D chain voxel sprite 1922.
// This preview does not alter attack traces, damage, charge, server state,
// cooldowns or the actual flail item. The voxel may still include fixed
// chain artwork; future voxel-asset splitting can remove that overlap.
// Simulation is updated at most ONCE per SDL millisecond for stereo parity.
struct VrFlailCosmeticState
{
    bool valid = false;
    Uint32 lastMs = 0;
    VrGripVector3 head{}, velocity{}, previousAnchor{};
};
static VrFlailCosmeticState g_vrFlailCosmetic;
static GLuint g_vrFlailChainTexture = 0;
static GLuint g_vrFlailHeadTexture = 0;

static VrGripVector3 vrFlailSub(VrGripVector3 a, VrGripVector3 b)
{
    return { a.x-b.x, a.y-b.y, a.z-b.z };
}
static float vrFlailLen(VrGripVector3 v)
{
    return sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
}
static VrGripVector3 vrFlailCross(VrGripVector3 a, VrGripVector3 b)
{
    return { a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x };
}
static VrGripVector3 vrFlailNormalize(VrGripVector3 v)
{
    const float length = vrFlailLen(v);
    return length > 0.0001f ?
        VrGripVector3{v.x/length, v.y/length, v.z/length} :
        VrGripVector3{0.f, 1.f, 0.f};
}
static void vrFlailInitializeTextures()
{
    static const int kChainW = 28;
    static const int kChainH = 36;
    static const unsigned char kChainPixels[kChainW * kChainH * 4] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff, 0x2f, 0x2f, 0x2f, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff, 0x27, 0x27, 0x27, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff, 0x33, 0x33, 0x33, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff, 0x2b, 0x2b, 0x2b, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const int kHeadW = 56;
    static const int kHeadH = 56;
    static const unsigned char kHeadPixels[kHeadW * kHeadH * 4] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff,
        0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff,
        0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff,
        0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff,
        0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff,
        0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff,
        0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff,
        0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff, 0x21, 0x21, 0x21, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff,
        0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff, 0x32, 0x32, 0x32, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff,
        0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff,
        0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff,
        0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff,
        0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff,
        0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff,
        0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff,
        0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff,
        0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff, 0x22, 0x22, 0x22, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff, 0x25, 0x25, 0x25, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff, 0x2a, 0x2a, 0x2a, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff, 0x2d, 0x2d, 0x2d, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    if (!g_vrFlailChainTexture)
    {
        glGenTextures(1, &g_vrFlailChainTexture);
        glBindTexture(GL_TEXTURE_2D, g_vrFlailChainTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kChainW, kChainH, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, kChainPixels);
    }
    if (!g_vrFlailHeadTexture)
    {
        glGenTextures(1, &g_vrFlailHeadTexture);
        glBindTexture(GL_TEXTURE_2D, g_vrFlailHeadTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kHeadW, kHeadH, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, kHeadPixels);
    }
}
static void vrDrawFlailCosmeticPhysics(view_t* eyeCamera, Entity* marker)
{
    if (!eyeCamera || !marker || !g_vrFlailVisualEnabled)
        return;
    const float length = g_vrFlailChainLength;
    // All coordinates below use VrGripVector3's (game-X, UP, game-Y) basis.
    // The anchor is the end of the rigid controller-driven flail handle.
    // This offset is an initial visual estimate, not a confirmed voxel pivot.
    const VrGripBasis3 weaponBasis = vrGripEulerBasis(
        static_cast<float>(marker->yaw), static_cast<float>(marker->pitch),
        static_cast<float>(marker->roll));
    const VrGripVector3 tipOffset = vrGripApplyBasis(weaponBasis,
        { g_vrFlailAnchorForward * static_cast<float>(marker->scalex),
          g_vrFlailAnchorUp * static_cast<float>(marker->scalex),
          g_vrFlailAnchorSide * static_cast<float>(marker->scalex) });
    const VrGripVector3 anchor{
        static_cast<float>(marker->x) + tipOffset.x,
        -static_cast<float>(marker->z) + tipOffset.y,
        static_cast<float>(marker->y) + tipOffset.z
    };
    const Uint32 now = SDL_GetTicks();
    if (!g_vrFlailCosmetic.valid || now-g_vrFlailCosmetic.lastMs > 150u ||
        vrFlailLen(vrFlailSub(anchor, g_vrFlailCosmetic.previousAnchor)) > 7.f)
    {
        g_vrFlailCosmetic.head = { anchor.x, anchor.y-length, anchor.z };
        g_vrFlailCosmetic.velocity = {0.f,0.f,0.f};
        g_vrFlailCosmetic.valid = true;
        g_vrFlailCosmetic.lastMs = now;
    }
    else if (now != g_vrFlailCosmetic.lastMs)
    {
        // Fixed-size stable integration; never advance separately per eye.
        const float dt = std::min(0.032f,
            static_cast<float>(now-g_vrFlailCosmetic.lastMs)*0.001f);
        const int steps = 4;
        const float h = dt / steps;
        for (int i = 0; i < steps; ++i)
        {
            auto& head = g_vrFlailCosmetic.head;
            auto& vel = g_vrFlailCosmetic.velocity;
            vel.y -= 78.f*h;
            vel.x *= (1.f-g_vrFlailDamping*h);
            vel.y *= (1.f-g_vrFlailDamping*h);
            vel.z *= (1.f-g_vrFlailDamping*h);
            head.x += vel.x*h;
            head.y += vel.y*h;
            head.z += vel.z*h;
            const VrGripVector3 delta = vrFlailSub(head, anchor);
            const float dist = vrFlailLen(delta);
            if (dist > length && dist > 0.0001f)
            {
                const VrGripVector3 normal = {delta.x/dist,delta.y/dist,delta.z/dist};
                head = {anchor.x+normal.x*length,
                        anchor.y+normal.y*length,
                        anchor.z+normal.z*length};
                // Eliminate outward velocity but retain tangential swing.
                const float outward = vel.x*normal.x+vel.y*normal.y+vel.z*normal.z;
                if (outward > 0.f)
                {
                    vel.x -= outward*normal.x;
                    vel.y -= outward*normal.y;
                    vel.z -= outward*normal.z;
                }
            }
        }
    }
    g_vrFlailCosmetic.lastMs = now;
    g_vrFlailCosmetic.previousAnchor = anchor;
    // STEP638FT: Reuse vanilla's actual first-person flail chain entity.
    // actHudAdditional2() assigns model sprite 1922 (steelflail_chain_FP.vox).
    // The model contains BOTH the chain and weighted head. We draw it once
    // in world space, oriented between the calibrated handle tip and the
    // visually simulated head. No new sprite index, texture, or collision.
    Entity* const chain = vrFindLocalHudAdditional(true);
    if (!chain)
        return;

    const auto& head = g_vrFlailCosmetic.head;
    const VrGripVector3 segment = vrFlailSub(head, anchor);
    const float actualLength = vrFlailLen(segment);
    if (actualLength <= 0.05f)
        return;

    // Keep the native HUD Entity completely intact for the next game tick
    // and for desktop rendering. Only the stereo-eye draw uses this pose.
    const int oldSprite = chain->sprite;
    const double oldX=chain->x, oldY=chain->y, oldZ=chain->z;
    const double oldYaw=chain->yaw, oldPitch=chain->pitch, oldRoll=chain->roll;
    const double oldSX=chain->scalex, oldSY=chain->scaley, oldSZ=chain->scalez;
    const auto oldFX=chain->focalx, oldFY=chain->focaly, oldFZ=chain->focalz;
    const bool oldInvisible=chain->flags[INVISIBLE];
    const bool oldOverdraw=chain->flags[OVERDRAW];
    const bool oldBright=chain->flags[BRIGHT];

    // STEP638FU: The native FP voxel's head faces the opposite way from
    // the forward chain basis used in FT. Reverse its axis so the weighted
    // head hangs AWAY from the grip, with the links attached at the pivot.
    // The subsequent cross products keep a right-handed rotation basis.
    const VrGripVector3 modelUp = vrFlailNormalize(
        VrGripVector3{-segment.x, -segment.y, -segment.z});
    const VrGripVector3 reference = fabsf(modelUp.y) > .90f ?
        VrGripVector3{1.f,0.f,0.f} : VrGripVector3{0.f,1.f,0.f};
    const VrGripVector3 modelRight = vrFlailNormalize(vrFlailCross(reference, modelUp));
    const VrGripVector3 modelForward = vrFlailNormalize(vrFlailCross(modelUp, modelRight));
    const VrGripBasis3 modelBasis{modelForward, modelUp, modelRight};

    // In the supplied 7x7x15 voxel, z=0..8 are the chain, and the
    // weighted head is centered near z=11. The native mesh's nominal
    // vertical center is ~7.5. Place its center 7.5/11 of the way from
    // the chain attachment to the simulated head, so the chain starts at
    // the saved pivot and the original head reaches the simulated endpoint.
    const float centerFraction = 7.5f / 11.f;
    chain->x = anchor.x + segment.x * centerFraction;
    chain->y = anchor.z + segment.z * centerFraction;
    chain->z = -(anchor.y + segment.y * centerFraction);
    vrGripBasisToBaronyAngles(modelBasis, static_cast<float>(oldYaw),
        chain->yaw, chain->pitch, chain->roll);
    chain->sprite = 1922; // Vanilla actHudAdditional2 first-person chain.
    // STEP638FV: Live scale from FLAIL PIVOT. The 1.5 default reproduces
    // FU exactly; glDrawVoxel squares the scale, so take sqrt for the actual
    // proportional rendered-size multiplier. Does not change combat physics.
    const float kFlailVoxelScaleBoost = sqrtf(g_vrFlailModelSize);
    chain->scalex = 0.115f * kFlailVoxelScaleBoost;
    chain->scaley = (actualLength / 11.f) * kFlailVoxelScaleBoost;
    chain->scalez = 0.115f * kFlailVoxelScaleBoost;
    chain->focalx = chain->focaly = chain->focalz = 0.0;
    chain->flags[INVISIBLE] = false;
    chain->flags[OVERDRAW] = false;
    chain->flags[BRIGHT] = true;
    glDrawVoxel(eyeCamera, chain, REALCOLORS);

    chain->sprite=oldSprite;
    chain->x=oldX; chain->y=oldY; chain->z=oldZ;
    chain->yaw=oldYaw; chain->pitch=oldPitch; chain->roll=oldRoll;
    chain->scalex=oldSX; chain->scaley=oldSY; chain->scalez=oldSZ;
    chain->focalx=oldFX; chain->focaly=oldFY; chain->focalz=oldFZ;
    chain->flags[INVISIBLE]=oldInvisible;
    chain->flags[OVERDRAW]=oldOverdraw;
    chain->flags[BRIGHT]=oldBright;

    // Keep the existing bright calibration pivot marker. It remains a
    // cosmetic editing aid only, and does not affect physics or collisions.
    if (g_vrArmCalibrationActive && g_vrArmCalibrationEditFlail)
    {
        GLint oldTexture = 0, oldActive = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
        vrFlailInitializeTextures();
        if (g_vrFlailHeadTexture)
        {
            const double x=marker->x,y=marker->y,z=marker->z;
            const double yaw=marker->yaw,pitch=marker->pitch,roll=marker->roll;
            const double sx=marker->scalex,sy=marker->scaley,sz=marker->scalez;
            const auto fx=marker->focalx,fy=marker->focaly,fz=marker->focalz;
            const bool invisible=marker->flags[INVISIBLE];
            const bool overdraw=marker->flags[OVERDRAW];
            marker->x=anchor.x; marker->y=anchor.z; marker->z=-anchor.y;
            marker->yaw=eyeCamera->ang; marker->pitch=marker->roll=0;
            marker->scalex=.24f/56.f; marker->scaley=.24f/56.f; marker->scalez=1.f;
            marker->focalx=marker->focaly=marker->focalz=0;
            marker->flags[INVISIBLE]=false;
            marker->flags[OVERDRAW]=false;
            glDrawVrUiTexture(eyeCamera, marker, g_vrFlailHeadTexture, 56, 56);
            marker->yaw += 0.5*PI;
            glDrawVrUiTexture(eyeCamera, marker, g_vrFlailHeadTexture, 56, 56);
            marker->x=x; marker->y=y; marker->z=z;
            marker->yaw=yaw; marker->pitch=pitch; marker->roll=roll;
            marker->scalex=sx; marker->scaley=sy; marker->scalez=sz;
            marker->focalx=fx; marker->focaly=fy; marker->focalz=fz;
            marker->flags[INVISIBLE]=invisible;
            marker->flags[OVERDRAW]=overdraw;
        }
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture));
        glActiveTexture(static_cast<GLenum>(oldActive));
    }
}

// Draw Barony's OWN selected HUD equipment voxel as a tracked, world-projected
// model for this eye only. The original entity is restored before returning.
// 'item' may have INVISIBLE=true because the regular flat-HUD path hides it;
// actual equipment existence is checked by the caller instead.
static void vrDrawTrackedHeldVoxel(view_t* eyeCamera, Entity* item,
    const Entity* trackedHand, bool rightHand, const Item* equipped = nullptr,
    const XrQuaternionf* controllerOrientation = nullptr)
{
    if (!item || !trackedHand)
        return;
    // World-model books can replace an empty/invisible HUD half model.
    const bool vrWorldBook = !rightHand && equipped &&
        itemCategory(equipped) == SPELLBOOK;
    if (item->sprite <= 0 && !vrWorldBook)
        return;

    const auto sx = item->x, sy = item->y, sz = item->z;
    const auto syaw = item->yaw, spitch = item->pitch, sroll = item->roll;
    const auto ssx = item->scalex, ssy = item->scaley, ssz = item->scalez;
    const auto sfx = item->focalx, sfy = item->focaly, sfz = item->focalz;
    const bool so = item->flags[OVERDRAW];
    const bool sb = item->flags[BRIGHT];
    const bool si = item->flags[INVISIBLE];

    // STEP 613: HUD spellbooks use first-person pieces with a separate
    // book/page entity, which looks split and stationary in 6DoF. For VR,
    // render the same single world voxel model used by dropped books.
    // Restore the HUD entity's original sprite after each eye.
    const int savedSprite = item->sprite;
    if (vrWorldBook)
    {
        const Sint32 worldSprite = itemModel(equipped);
        if (worldSprite > 0) item->sprite = worldSprite;
    }

    // Keep the equipped item's native relative scale; first-person models
    // with native half-scale retain that distinction, while both are larger
    // compared with the now correctly sized tracked hand.
    const VrWeaponGripProfile grip = vrWeaponGripForItem(equipped);
    // STEP638FO: One complete native world-book voxel, at half the
    // previous VR-only size. Never resize unrelated offhand items.
    const float visualScale = vrWorldBook ? (0.5f * VR_SHIELD_VOXEL_SCALE) :
        (rightHand ? VR_ITEM_VOXEL_SCALE * grip.scaleMultiplier :
            VR_SHIELD_VOXEL_SCALE) *
        ((item->scalex > 0.01 && item->scalex < 4.0) ?
            static_cast<float>(item->scalex) : 1.0f);
    const float reach = rightHand ? grip.forwardOffset : 0.55f;
    const float side = rightHand ? grip.sideOffset : 0.0f;
    const float up = rightHand ? grip.upOffset : 0.12f;
    // STEP 600: BOTH held hands use the same quaternion-derived rigid
    // transform. STEP 599 added 90 degrees to shield Euler pitch, causing
    // the exact rotational artifact previously seen on right-hand weapons.
    // Calibrations are controller-local: controller * itemOffsetBasis.
    const bool rigidHeldItem = controllerOrientation != nullptr;
    if (rigidHeldItem)
    {
        const VrGripBasis3 controller = vrGripXRControllerBasis(
            *controllerOrientation, eyeCamera->ang);
        const VrGripVector3 localGrip = rightHand ?
            VrGripVector3{ grip.forwardOffset,
                grip.upOffset - grip.localGripDownOffset, grip.sideOffset } :
            VrGripVector3{ 0.55f, 0.12f, 0.0f };
        const VrGripVector3 worldGrip = vrGripApplyBasis(controller, localGrip);
        item->x = trackedHand->x + worldGrip.x;
        item->y = trackedHand->y + worldGrip.z;
        item->z = trackedHand->z - worldGrip.y;

        const bool actualShield = !rightHand && equipped &&
            itemCategory(equipped) == ARMOR;
        const VrGripBasis3 calibration = vrGripEulerBasis(
            rightHand ? grip.yawOffset : 0.0f,
            rightHand ? grip.pitchOffset :
            (actualShield ? 0.5f * static_cast<float>(PI) : 0.0f),
            rightHand ? grip.rollOffset : 0.0f);
        const VrGripBasis3 rigidModel{
            vrGripApplyBasis(controller, calibration.forward),
            vrGripApplyBasis(controller, calibration.up),
            vrGripApplyBasis(controller, calibration.right)
        };
        vrGripBasisToBaronyAngles(rigidModel, trackedHand->yaw,
            item->yaw, item->pitch, item->roll);

        static unsigned int diagnosticCount = 0;
        if ((++diagnosticCount % 600u) == 1u)
            std::printf("OpenXR VR: STEP600 %s_RIGID_GRIP type=%d sprite=%d "
                "local=(%.3f %.3f %.3f) world=(%.3f %.3f %.3f) "
                "angles=(%.2f %.2f %.2f)\n",
                rightHand ? "RIGHT" : "LEFT",
                equipped ? static_cast<int>(equipped->type) : -1,
                item->sprite,
                static_cast<double>(localGrip.x),
                static_cast<double>(localGrip.y),
                static_cast<double>(localGrip.z),
                static_cast<double>(worldGrip.x),
                static_cast<double>(worldGrip.y),
                static_cast<double>(worldGrip.z),
                static_cast<double>(item->yaw),
                static_cast<double>(item->pitch),
                static_cast<double>(item->roll));
    }
    else
    {
        // Tracking loss: retain a harmless stable visual fallback.
        item->x = trackedHand->x + cosf(trackedHand->yaw) * reach -
            sinf(trackedHand->yaw) * side;
        item->y = trackedHand->y + sinf(trackedHand->yaw) * reach +
            cosf(trackedHand->yaw) * side;
        item->z = trackedHand->z - up;
        item->yaw = trackedHand->yaw;
        item->pitch = trackedHand->pitch;
        item->roll = trackedHand->roll;
    }
    // STEP 603: Move held MAGIC STAVES 0.60 units (~7.5cm) away from
    // the avatar torso in BODY-forward space. This stays outward even if the
    // controller is rolled or pitched back toward the player. Other weapons,
    // the loaded crossbow bolt, and all projectile velocities are untouched.
    if (rightHand && equipped && itemCategory(equipped) == MAGICSTAFF &&
        clientnum >= 0 && clientnum < MAXPLAYERS && players[clientnum] &&
        players[clientnum]->entity)
    {
        const float bodyYaw = static_cast<float>(players[clientnum]->entity->yaw);
        item->x += 0.60f * cosf(bodyYaw);
        item->y += 0.60f * sinf(bodyYaw);
    }
    // STEP 606: Apply per-ItemType, per-hand visual calibration after the
    // original grip and staff-body reach; the weapon's bolt child inherits it.
    // Controller-only aiming and projectile spawning do not use this transform.
    if (equipped && controllerOrientation)
    {
        const int handIndex = rightHand ? 1 : 0;
        const auto it = g_vrItemVisual[handIndex].find(static_cast<int>(equipped->type));
        if (it != g_vrItemVisual[handIndex].end())
        {
            const VrArmVisualCalibration& visual = it->second;
            const VrGripBasis3 controller = vrGripXRControllerBasis(
                *controllerOrientation, eyeCamera->ang);
            const VrGripVector3 shift = vrGripApplyBasis(controller,
                { visual.forward, visual.up, visual.side });
            item->x += shift.x;
            item->y += shift.z;
            item->z -= shift.y;
            const VrGripBasis3 base = vrGripEulerBasis(
                static_cast<float>(item->yaw), static_cast<float>(item->pitch),
                static_cast<float>(item->roll));
            const VrGripBasis3 extra = vrGripEulerBasis(
                visual.yaw, visual.pitch, visual.roll);
            const VrGripBasis3 adjusted{
                vrGripApplyBasis(base, extra.forward),
                vrGripApplyBasis(base, extra.up),
                vrGripApplyBasis(base, extra.right)
            };
            vrGripBasisToBaronyAngles(adjusted, static_cast<float>(item->yaw),
                item->yaw, item->pitch, item->roll);
        }
    }
    item->scalex = item->scaley = item->scalez = visualScale;
    // CRITICAL: do not restore the source HUD focal offsets here; those are
    // for the flat first-person display and displaced the VR model upward by
    // approximately half an arm's length in the Step 594 headset test.
    // Original focal values are still restored after this one-eye draw.
    item->focalx = item->focaly = item->focalz = 0.0;
    item->flags[OVERDRAW] = false;
    item->flags[BRIGHT] = true;
    item->flags[INVISIBLE] = false;
    glDrawVoxel(eyeCamera, item, REALCOLORS);
    if (rightHand && equipped && equipped->type == STEEL_FLAIL)
        vrDrawFlailCosmeticPhysics(eyeCamera, item);
    if (!rightHand)
        vrDrawTrackedOffhandFlames(eyeCamera, item, equipped,
            sx, sy, sz, sroll);

    // STEP 602: Anchor the *visible, loaded* HUD bolt to the CROSSBOW MODEL.
    // The original HUD bolt is hidden only while each VR eye's world pass
    // draws (below); after that, the original entity is unchanged. This is
    // a purely visual child and does not alter spawned arrow physics.
    if (rightHand && vrIsCrossbowType(equipped))
    {
        Entity* const bolt = vrFindLocalHudArrowModel();
        if (bolt && bolt != item && g_vrLoadedCrossbowBoltForEye)
        {
            // STEP638ES: Vanilla crossbows force HUD bolt sprite 976, even
            // when special quiver ammunition is equipped. For this VR-only
            // weapon-attached visual, reuse Barony's EXISTING matching first-
            // person ammunition models (actHudArrowModel's quiver variants).
            // Keep vanilla 976 for an ordinary/no-quiver loaded crossbow.
            // Restore the source sprite immediately after the stereo draw;
            // the offhand quiver, ammo consumption, damage and fired bolt
            // entity are untouched.
            const int oldBoltSprite = bolt->sprite;
            const Item* const equippedQuiver =
                clientnum >= 0 && clientnum < MAXPLAYERS && stats[clientnum] &&
                stats[clientnum]->shield &&
                itemTypeIsQuiver(stats[clientnum]->shield->type)
                    ? stats[clientnum]->shield : nullptr;
            if (equippedQuiver)
            {
                switch (equippedQuiver->type)
                {
                    case QUIVER_SILVER:      bolt->sprite = 935;  break;
                    case QUIVER_PIERCE:      bolt->sprite = 936;  break;
                    case QUIVER_LIGHTWEIGHT: bolt->sprite = 937;  break;
                    case QUIVER_FIRE:        bolt->sprite = 938;  break;
                    case QUIVER_KNOCKBACK:   bolt->sprite = 939;  break;
                    case QUIVER_CRYSTAL:     bolt->sprite = 940;  break;
                    case QUIVER_HUNTING:     bolt->sprite = 941;  break;
                    case QUIVER_BONE:        bolt->sprite = 2302; break;
                    case QUIVER_BLACKIRON:   bolt->sprite = 2303; break;
                    default:                bolt->sprite = 976;  break;
                }
            }
            const auto bx = bolt->x, by = bolt->y, bz = bolt->z;
            const auto byaw = bolt->yaw, bpitch = bolt->pitch, broll = bolt->roll;
            const auto bsx = bolt->scalex, bsy = bolt->scaley, bsz = bolt->scalez;
            const auto bfx = bolt->focalx, bfy = bolt->focaly, bfz = bolt->focalz;
            const bool bo = bolt->flags[OVERDRAW];
            const bool bb = bolt->flags[BRIGHT];
            const bool bi = bolt->flags[INVISIBLE];

            // Keep the loaded bolt level with the crossbow's top channel.
            // These MODEL-LOCAL offsets stay rigid as the wrist rotates.
            const VrGripBasis3 weaponBasis = vrGripEulerBasis(
                static_cast<float>(item->yaw), static_cast<float>(item->pitch),
                static_cast<float>(item->roll));
            const VrGripVector3 boltOffset = vrGripApplyBasis(
                weaponBasis, { 0.22f, 0.18f, 0.0f });
            bolt->x = item->x + boltOffset.x;
            bolt->y = item->y + boltOffset.z;
            bolt->z = item->z - boltOffset.y;
            bolt->yaw = item->yaw;
            bolt->pitch = item->pitch;
            bolt->roll = item->roll;
            const float nativeBoltScale =
                (bsx > 0.01 && bsx < 4.0) ? static_cast<float>(bsx) : 1.0f;
            const float boltScale = VR_ITEM_VOXEL_SCALE * nativeBoltScale;
            bolt->scalex = bolt->scaley = bolt->scalez = boltScale;
            bolt->focalx = bolt->focaly = bolt->focalz = 0.0;
            bolt->flags[OVERDRAW] = false;
            bolt->flags[BRIGHT] = true;
            bolt->flags[INVISIBLE] = false;
            if (bolt->flags[SPRITE])
                glDrawSprite(eyeCamera, bolt, REALCOLORS);
            else
                glDrawVoxel(eyeCamera, bolt, REALCOLORS);

            bolt->x = bx; bolt->y = by; bolt->z = bz;
            bolt->yaw = byaw; bolt->pitch = bpitch; bolt->roll = broll;
            bolt->scalex = bsx; bolt->scaley = bsy; bolt->scalez = bsz;
            bolt->focalx = bfx; bolt->focaly = bfy; bolt->focalz = bfz;
            bolt->flags[OVERDRAW] = bo;
            bolt->flags[BRIGHT] = bb;
            bolt->flags[INVISIBLE] = bi;
            const int drawnBoltSprite = bolt->sprite;
            bolt->sprite = oldBoltSprite;
            // Log only on a model change, not every eye/frame.
            static int lastReportedBoltSprite = -1;
            if (lastReportedBoltSprite != drawnBoltSprite)
            {
                std::printf("OpenXR VR STEP638ES: Loaded crossbow bolt visual model=%d ammo=%d.\n",
                    drawnBoltSprite,
                    equippedQuiver ? static_cast<int>(equippedQuiver->type) : -1);
                lastReportedBoltSprite = drawnBoltSprite;
            }
        }
    }

    item->x = sx; item->y = sy; item->z = sz;
    item->yaw = syaw; item->pitch = spitch; item->roll = sroll;
    item->scalex = ssx; item->scaley = ssy; item->scalez = ssz;
    item->focalx = sfx; item->focaly = sfy; item->focalz = sfz;
    item->flags[OVERDRAW] = so;
    item->flags[BRIGHT] = sb;
    item->flags[INVISIBLE] = si;
    item->sprite = savedSprite;
}

// STEP 630: Swing motion comes from the tracked hand/weapon relative to
// the HMD, NOT the game camera's world translation. This prevents joystick
// locomotion and room-scale walking with the hand held still from attacking.
struct VrMeleeSwingState
{
    bool previousValid = false;
    bool armed = true;
    bool triggerWasDown = false;
    bool chargeSpentThisHold = false;
    XrTime previousTime = 0;
    const Item* previousWeapon = nullptr;
    VrGripVector3 previousBase{};
    VrGripVector3 previousTip{};
    VrGripVector3 previousRelativeGrip{};
    VrGripVector3 previousRelativeTip{};
    Uint32 chargeStartedAt = 0;
    Uint32 nextAllowedHit = 0;
    // Remember the direction of the most recent intentional hand swing.
    // A meaningful reverse stroke can arm the next attack without requiring
    // the controller AND animated weapon tip to become motionless.
    VrGripVector3 strikeGripDirection{};
    float oppositeTravelMeters = 0.f;
    // Step635: a return movement is preparation, NOT another strike.
    // Arm a new fast stroke only after the hand comes back to the guard
    // and starts moving into another attack, or settles at the guard.
    bool returnedToGuard = false;
    float renewedStrikeTravelMeters = 0.f;
    Uint32 settledAtGuardAt = 0;
};
static VrMeleeSwingState g_vrMeleeSwing;

static void vrResetMeleeTracking()
{
    g_vrMeleeSwing = VrMeleeSwingState{};
    g_vrMeleeChargeVisible = false;
    g_vrMeleeChargeFraction = 0.0f;
    g_vrMeleeLiveTipSpeed = 0.f;
    g_vrMeleeLiveGripSpeed = 0.f;
    g_vrMeleeLiveReady = true;
    g_vrMeleeLiveReverseMeters = 0.f;
}

// STEP633: All strikes (including dry-run previews and misses) share one
// gesture state machine. A measured hand swing is one attack, not an ongoing
// stream of attacks while a weapon is moving.
static void vrMarkMeleeSwing(Uint32 now, float gx, float gy, float gz)
{
    const float handDistance = sqrtf(gx*gx + gy*gy + gz*gz);
    if (handDistance > 0.0001f)
    {
        const float inv = 1.f / handDistance;
        g_vrMeleeSwing.strikeGripDirection = {
            gx*inv, gy*inv, gz*inv
        };
    }
    g_vrMeleeSwing.oppositeTravelMeters = 0.f;
    g_vrMeleeSwing.returnedToGuard = false;
    g_vrMeleeSwing.renewedStrikeTravelMeters = 0.f;
    g_vrMeleeSwing.settledAtGuardAt = 0;
    g_vrMeleeSwing.armed = false;
    // Restrict duplicate contacts inside a single swing, but permit quick
    // deliberate alternating strokes (the prior 260ms blocked many).
    g_vrMeleeSwing.nextAllowedHit = now + 170u;
    g_vrMeleeLiveReady = false;
    g_vrMeleeLiveReverseMeters = 0.f;
}

// Step634: Collision/damage/objects are routed through Barony's own melee
// lineTrace and Entity::attack. VR only recognizes deliberate controller
// swing gestures; it does not supply a custom hit target or range.

// STEP638GA1: Temporary, synchronous VR-only horizontal melee trace aim.
// The actual native Barony attack still resolves all collision, reach,
// friendly-fire, damage and wall blocking using its normal lineTrace.
// This hook is active ONLY while our tracked hand invokes Entity::attack().
static ConsoleVariable<bool> cvarVRSwingTraceAim("/vr_melee_swing_aim", true);
static bool g_vrSwingTraceAimActive = false;
static real_t g_vrSwingTraceYaw = 0;

bool vrGetPhysicalMeleeTraceYaw(int playerIndex, real_t* outYaw)
{
    if (!outYaw || !*cvarVRSwingTraceAim || !g_vrSwingTraceAimActive ||
        !g_xrSessionRunning || g_xrSessionState != XR_SESSION_STATE_FOCUSED ||
        multiplayer == CLIENT || playerIndex != clientnum ||
        playerIndex < 0 || playerIndex >= MAXPLAYERS)
        return false;
    *outYaw = g_vrSwingTraceYaw;
    return true;
}

static void vrProcessPhysicalMelee(XrTime predictedDisplayTime)
{
    // Barony's multiplayer CLIENT attack path requires a new validated
    // target-UID packet; silently faking remote damage is not acceptable.
    const bool validPlayer = g_xrSessionRunning &&
        g_xrSessionState == XR_SESSION_STATE_FOCUSED &&
        g_xrEyeViewsValid && !intro && !gamePaused &&
        multiplayer != CLIENT &&
        clientnum >= 0 && clientnum < MAXPLAYERS &&
        players[clientnum] && players[clientnum]->entity &&
        players[clientnum]->shootmode && stats[clientnum] &&
        map.entities && g_xrControllerHands[1].gripPoseValid;
    vrLoadMeleeTuning();
    if (!validPlayer)
    {
        vrResetMeleeTracking();
        return;
    }

    const Item* weapon = stats[clientnum]->weapon;
    // STEP 638DG: Barony's getWeaponSkill(nullptr) == PRO_UNARMED.
    // Allow that native empty-weapon attack for any class permitted by the
    // original game, without creating VR-specific class or damage rules.
    const bool unarmed = (weapon == nullptr);
    // Keep the native attack-button path for spells waiting for an attack
    // input; a hand swing must not replace that spell interaction.
    if (unarmed && cast_animation[clientnum].spellWaitingAttackInput())
    {
        vrResetMeleeTracking();
        return;
    }
    // STEP638FR: Barony handles the Scepter as a melee strike when its
    // special blast is not ready. Reuse Entity::attack() for that native
    // choice; other magic staves remain excluded from physical melee.
    if (!unarmed && !isMeleeWeapon(*weapon) &&
        weapon->type != TOOL_PICKAXE && weapon->type != TOOL_WHIP &&
        weapon->type != MAGICSTAFF_SCEPTER)
    {
        vrResetMeleeTracking();
        return;
    }
    if (g_vrMeleeSwing.previousWeapon != weapon)
    {
        vrResetMeleeTracking();
        g_vrMeleeSwing.previousWeapon = weapon;
    }
    // Reuse Barony's core attack eligibility restrictions. Casting, blocking,
    // immobility, and menu interactions must not cause weapon strikes.
    if (!players[clientnum]->entity->isMobile() ||
        stats[clientnum]->defending ||
        cast_animation[clientnum].active ||
        cast_animation[clientnum].active_spellbook ||
        vrMenuPointerMode() || g_xrVrPointerActive ||
        g_xrVrPointerTriggerConsumed)
    {
        vrResetMeleeTracking();
        return;
    }

    // The trigger never directly executes a melee attack. It only builds
    // charge while held. Releasing it cancels any unused charge. Each trigger
    // hold can empower a single swing; release and re-press to charge again.
    const Uint32 now = SDL_GetTicks();
    const float analogTrigger = g_xrControllerHands[1].trigger;
    const bool triggerDown = g_vrMeleeSwing.triggerWasDown ?
        analogTrigger >= 0.35f : analogTrigger >= 0.65f;
    if (triggerDown && !g_vrMeleeSwing.triggerWasDown)
    {
        g_vrMeleeSwing.chargeStartedAt = now;
        g_vrMeleeSwing.chargeSpentThisHold = false;
    }
    if (!triggerDown)
    {
        g_vrMeleeSwing.chargeStartedAt = 0;
        g_vrMeleeSwing.chargeSpentThisHold = false;
    }
    g_vrMeleeSwing.triggerWasDown = triggerDown;
    const int maxCharge = std::max(1,
        static_cast<int>(Stat::getMaxAttackCharge(stats[clientnum])));
    const bool charging = triggerDown && !g_vrMeleeSwing.chargeSpentThisHold;
    const int chargeTicks = charging ? std::min(maxCharge,
        static_cast<int>((static_cast<Uint64>(now -
            g_vrMeleeSwing.chargeStartedAt) * TICKS_PER_SECOND) / 1000ULL)) : 0;
    g_vrMeleeChargeVisible = charging;
    g_vrMeleeChargeFraction = charging ?
        static_cast<float>(chargeTicks) / static_cast<float>(maxCharge) : 0.f;

    const XrPosef& gripPose = g_xrControllerHands[1].gripPose;
    const XrVector3f head{
        (g_xrViews[0].pose.position.x + g_xrViews[1].pose.position.x)*.5f,
        (g_xrViews[0].pose.position.y + g_xrViews[1].pose.position.y)*.5f,
        (g_xrViews[0].pose.position.z + g_xrViews[1].pose.position.z)*.5f
    };
    // The exact world transform already used for the displayed weapon.
    const view_t& camera = cameras[clientnum];
    const float c = cosf(camera.ang), s = sinf(camera.ang);
    const float right = gripPose.position.x - head.x;
    const float forward = head.z - gripPose.position.z;
    const float above = gripPose.position.y - head.y;
    VrGripVector3 origin{
        static_cast<float>(camera.x)*16.f +
            VR_HAND_UNITS_PER_METER*(forward*c - right*s),
        -(static_cast<float>(camera.z)-1.f)*.5f +
            VR_HAND_UNITS_PER_METER*above,
        static_cast<float>(camera.y)*16.f +
            VR_HAND_UNITS_PER_METER*(forward*s + right*c)
    };
    // A fist has no held-weapon offset or blade tip. Use the tracked grip
    // directly: rotating a stationary wrist cannot count as a punch.
    const VrWeaponGripProfile profile = unarmed ?
        VrWeaponGripProfile{ 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f } :
        vrWeaponGripForItem(weapon);
    const VrGripBasis3 controller = vrGripXRControllerBasis(
        gripPose.orientation, camera.ang);
    const VrGripVector3 localGrip{profile.forwardOffset,
        profile.upOffset - profile.localGripDownOffset, profile.sideOffset};
    const VrGripVector3 gripOffset = vrGripApplyBasis(controller, localGrip);
    origin.x += gripOffset.x;
    origin.y += gripOffset.y;
    origin.z += gripOffset.z;
    const VrGripBasis3 calibrated = vrGripEulerBasis(
        profile.yawOffset, profile.pitchOffset, profile.rollOffset);
    const VrGripVector3 bladeDir = vrGripApplyBasis(controller,
        calibrated.forward);
    // First approximation of the visible voxel's strike surface. Scale and
    // reach will be calibrated per item following actual headset observations.
    const float lengthUnits = unarmed ? 0.f :
        (weapon->type == TOOL_WHIP ? 8.4f :
            (weapon->type == TOOL_PICKAXE ? 5.2f : 6.4f));
    const VrGripVector3 tip{
        origin.x + bladeDir.x*lengthUnits,
        origin.y + bladeDir.y*lengthUnits,
        origin.z + bladeDir.z*lengthUnits
    };

    // Swing speed is measured in an HMD-relative tracking frame. Do NOT use
    // world-space origin/tip for velocity: those move with player locomotion.
    // Zero camera yaw in the tracking basis so head turning is not interpreted
    // as a blade swing. The calibration and tool length remain consistent with
    // the actual rendered equipment.
    const VrGripBasis3 localController = vrGripXRControllerBasis(
        gripPose.orientation, 0.f);
    const VrGripVector3 localGripOffset = vrGripApplyBasis(
        localController, localGrip);
    const VrGripVector3 localBladeDir = vrGripApplyBasis(
        localController, calibrated.forward);
    const VrGripVector3 relativeGrip{
        VR_HAND_UNITS_PER_METER * (head.z-gripPose.position.z),
        VR_HAND_UNITS_PER_METER * (gripPose.position.y-head.y),
        VR_HAND_UNITS_PER_METER * (gripPose.position.x-head.x)
    };
    const VrGripVector3 relativeTip{
        relativeGrip.x + localGripOffset.x + localBladeDir.x*lengthUnits,
        relativeGrip.y + localGripOffset.y + localBladeDir.y*lengthUnits,
        relativeGrip.z + localGripOffset.z + localBladeDir.z*lengthUnits
    };

    const float dt = static_cast<float>(
        static_cast<double>(predictedDisplayTime - g_vrMeleeSwing.previousTime)*1e-9);
    const bool stable = g_vrMeleeSwing.previousValid && dt > 0.004f &&
        dt < 0.075f;
    const VrGripVector3 previousTip = g_vrMeleeSwing.previousTip;
    const VrGripVector3 previousBase = g_vrMeleeSwing.previousBase;
    const VrGripVector3 prevRelativeTip = g_vrMeleeSwing.previousRelativeTip;
    const VrGripVector3 prevRelativeGrip = g_vrMeleeSwing.previousRelativeGrip;
    g_vrMeleeSwing.previousTip = tip;
    g_vrMeleeSwing.previousBase = origin;
    g_vrMeleeSwing.previousRelativeGrip = relativeGrip;
    g_vrMeleeSwing.previousRelativeTip = relativeTip;
    g_vrMeleeSwing.previousTime = predictedDisplayTime;
    g_vrMeleeSwing.previousValid = true;
    if (!stable) return;

    const float dx = relativeTip.x-prevRelativeTip.x;
    const float dy = relativeTip.y-prevRelativeTip.y;
    const float dz = relativeTip.z-prevRelativeTip.z;
    const float tipSpeed = sqrtf(dx*dx + dy*dy + dz*dz) /
        VR_HAND_UNITS_PER_METER / dt;
    const float gx = relativeGrip.x-prevRelativeGrip.x;
    const float gy = relativeGrip.y-prevRelativeGrip.y;
    const float gz = relativeGrip.z-prevRelativeGrip.z;
    const float gripSpeed = sqrtf(gx*gx + gy*gy + gz*gz) /
        VR_HAND_UNITS_PER_METER / dt;

    // While leaving calibration, don't mistake exit-grip movement for a
    // strike. Resume tracking only after BOTH exit grips have been released.
    if (g_vrMeleeExitGripsSuppressed)
    {
        vrResetMeleeTracking();
        return;
    }
    // STEP632: Tip-only movement caused false attacks. Require BOTH actual
    // hand movement and tip movement. A single shared profile, adjusted in
    // the grip-chord melee calibration page, never edits weapon rules.
    g_vrMeleeLiveTipSpeed = tipSpeed;
    g_vrMeleeLiveGripSpeed = gripSpeed;
    // STEP635: Do not allow the fast BACKSWING to count as a new attack.
    // Step633 re-armed DURING 4.5 cm of reversed movement: a sufficiently
    // fast return could immediately cause another attack. Instead track
    // (1) returning to the guard, then (2) starting the next strike.
    // Relative HMD space suppresses character locomotion as before.
    if (!g_vrMeleeSwing.armed)
    {
        const auto& direction = g_vrMeleeSwing.strikeGripDirection;
        const float travel = sqrtf(gx*gx + gy*gy + gz*gz);
        const float along = gx*direction.x + gy*direction.y +
            gz*direction.z;
        const float calmHandSpeed = std::max(g_vrMeleeTuning.rearmSpeed,
            0.55f*g_vrMeleeTuning.minGripSpeed);
        const float calmTipSpeed = std::max(1.5f*g_vrMeleeTuning.rearmSpeed,
            0.65f*g_vrMeleeTuning.minTipSpeed);
        if (travel > 0.0001f &&
            gripSpeed >= std::max(0.25f,
                0.40f*g_vrMeleeTuning.minGripSpeed) &&
            along <= -0.40f*travel)
        {
            // Opposite motion is strictly a return stroke, never an attack.
            g_vrMeleeSwing.oppositeTravelMeters +=
                -along/VR_HAND_UNITS_PER_METER;
            g_vrMeleeSwing.renewedStrikeTravelMeters = 0.f;
            g_vrMeleeSwing.settledAtGuardAt = 0;
        }
        if (g_vrMeleeSwing.oppositeTravelMeters >= 0.060f)
            g_vrMeleeSwing.returnedToGuard = true;

        if (g_vrMeleeSwing.returnedToGuard)
        {
            // A small, intentional forward movement AFTER the return begins
            // the next gesture. Merely hitting the return threshold does not.
            if (travel > 0.0001f && along >= 0.45f*travel &&
                gripSpeed > calmHandSpeed)
            {
                g_vrMeleeSwing.renewedStrikeTravelMeters +=
                    along/VR_HAND_UNITS_PER_METER;
                if (g_vrMeleeSwing.renewedStrikeTravelMeters >= 0.030f)
                    g_vrMeleeSwing.armed = true;
            }
            else if (along <= 0.f)
            {
                g_vrMeleeSwing.renewedStrikeTravelMeters = 0.f;
            }

            // After returning to guard, deliberately PAUSING permits a new
            // attack from a different direction. The neutral pause is never
            // enough on its own without a preceding return displacement.
            if (gripSpeed <= calmHandSpeed && tipSpeed <= calmTipSpeed)
            {
                if (!g_vrMeleeSwing.settledAtGuardAt)
                    g_vrMeleeSwing.settledAtGuardAt = now;
                else if (now - g_vrMeleeSwing.settledAtGuardAt >= 150u)
                    g_vrMeleeSwing.armed = true;
            }
            else
            {
                g_vrMeleeSwing.settledAtGuardAt = 0;
            }
        }
    }
    g_vrMeleeLiveReady = g_vrMeleeSwing.armed;
    g_vrMeleeLiveReverseMeters = g_vrMeleeSwing.oppositeTravelMeters;
    // Require BOTH the physical hand and the weapon tip to move quickly.
    // Character walking and animated blade motion cannot strike by themselves.
    // Bare-hand strikes require actual hand translation. The normal blade
    // speed test remains unchanged for every equipped melee weapon.
    const bool deliberateSwing = unarmed ?
        gripSpeed >= g_vrMeleeTuning.minGripSpeed :
        (tipSpeed >= g_vrMeleeTuning.minTipSpeed &&
         gripSpeed >= g_vrMeleeTuning.minGripSpeed);
    if (!g_vrMeleeSwing.armed || !deliberateSwing ||
        (g_vrMeleeSwing.nextAllowedHit != 0 &&
         static_cast<Sint32>(now - g_vrMeleeSwing.nextAllowedHit) < 0))
        return;

    // Calibration uses a DRY RUN: record recognition and recovery for tuning
    // but never invoke Barony's damage engine. No attack or charge is spent.
    if (g_vrArmCalibrationActive)
    {
        g_vrMeleeLastPreviewAt = now;
        vrMarkMeleeSwing(now, gx, gy, gz);
        return;
    }

    // IMPORTANT: Spend on swing recognition, not collision. Missing should
    // neither damage scenery nor retain an already-prepared charged attack.
    const int attackCharge = charging ? chargeTicks : 0;
    vrMarkMeleeSwing(now, gx, gy, gz);
    if (charging)
    {
        g_vrMeleeSwing.chargeSpentThisHold = true;
        g_vrMeleeChargeVisible = false;
        g_vrMeleeChargeFraction = 0.f;
    }
    // Preserve Barony's normal attack pose meanings for thrust weapons and
    // use a side-swing when physical motion is primarily lateral.
    const bool thrustWeapon = !unarmed && (weapon->type == RAPIER ||
        weapon->type == IRON_SPEAR ||
        weapon->type == ARTIFACT_SPEAR ||
        weapon->type == BONE_SPEAR ||
        weapon->type == SILVER_GLAIVE ||
        weapon->type == BLACKIRON_TRIDENT ||
        weapon->type == CRYSTAL_SPEAR);
    // Vanilla Entity::attack already turns empty-hand attacks into punch
    // pose 1, awarding the normal Unarmed proficiency and damage modifiers.
    const int attackPose = unarmed ? 1 : (thrustWeapon ? 3 :
        (fabsf(dz) > fabsf(dx)*0.85f &&
         weapon->type != TOOL_PICKAXE &&
         weapon->type != TOOL_WHIP ? 2 : 1));

    // Calling the original attack function WITHOUT a fabricated target is
    // essential. It performs its own line trace and preserves STRIKERANGE,
    // native whip/flail modifiers, walls, sinks, furniture, colliders,
    // monsters, protections, status effects and original whoosh/impact audio.
    // STEP638GA1: Preserve the native hit/damage path but optionally steer
    // its horizontal trace toward the tracked sword's latest swing position.
    // Full swept physical-contact collision remains a future test.
    Entity* const attacker = players[clientnum]->entity;

    // STEP638GA1: Steer the vanilla 2D attack RAY toward the position the
    // tracked weapon actually swept through, not the direction of the HMD.
    // Sample the two consecutive sword-tip positions (or fists) to reduce
    // frame jitter. Never extend the trace or substitute an arbitrary target.
    // The original STRIKERANGE / whip / flail reach remains in Entity::attack.
    const float sweepX = (previousTip.x + tip.x) * 0.5f;
    const float sweepY = (previousTip.z + tip.z) * 0.5f;
    const float fromPlayerX = sweepX - static_cast<float>(attacker->x);
    const float fromPlayerY = sweepY - static_cast<float>(attacker->y);
    const float sweptPlanarLength = hypotf(fromPlayerX, fromPlayerY);
    const bool validSwingAim = std::isfinite(sweptPlanarLength) &&
        sweptPlanarLength > 1.0f;
    if (validSwingAim)
    {
        g_vrSwingTraceYaw = static_cast<real_t>(atan2f(
            fromPlayerY, fromPlayerX));
        g_vrSwingTraceAimActive = true;
    }
    attacker->attack(attackPose, attackCharge, nullptr);
    // Clear immediately: other actors, queued attacks and the next game
    // tick must always take the ordinary combat path.
    g_vrSwingTraceAimActive = false;
}

// STEP 638DM: Read Barony's live spellcasting animation and apply a SMALL,
// purely visual offset to tracked arm models. The controller poses, interaction
// rays, magic projectile origin/direction, and gameplay remain untouched.
// Values below are Barony world-units, not meters; held items use the already
// offset tracked arm as their grip anchor, so they remain attached.
struct VrCastingArmVisualState
{
    Uint32 sampledGameTick = ~static_cast<Uint32>(0);
    Uint32 releaseStartedMs = 0;
    bool wasCasting = false;
    int previousStage = -1;
};
static VrCastingArmVisualState g_vrCastingArmVisual;

static VrGripVector3 vrCastingArmVisualOffset(int handIndex, float yaw)
{
    if (vrBowEquipped() && handIndex != vrSpellControllerHand())
        return {0.f,0.f,0.f};
    if (clientnum < 0 || clientnum >= MAXPLAYERS ||
        !players[clientnum] || !players[clientnum]->entity)
        return { 0.f, 0.f, 0.f };
    const auto& anim = cast_animation[clientnum];
    const bool casting = anim.active || anim.active_spellbook;
    const Uint32 now = SDL_GetTicks();
    // Only sample state transitions once per GAME tick (not once per eye).
    if (g_vrCastingArmVisual.sampledGameTick != ::ticks)
    {
        if ((g_vrCastingArmVisual.wasCasting && !casting) ||
            (g_vrCastingArmVisual.wasCasting && casting &&
             g_vrCastingArmVisual.previousStage == 0 && anim.stage != 0))
        {
            g_vrCastingArmVisual.releaseStartedMs = now;
        }
        if (casting && anim.stage == 0)
            g_vrCastingArmVisual.releaseStartedMs = 0;
        g_vrCastingArmVisual.wasCasting = casting;
        g_vrCastingArmVisual.previousStage = casting ? anim.stage : -1;
        g_vrCastingArmVisual.sampledGameTick = ::ticks;
    }

    float localForward = 0.f;
    float localSide = 0.f;
    float localUp = 0.f;
    if (casting && anim.stage == 0)
    {
        // Vanilla casts circulate the hands via lefthand_movex/y. Mirror the
        // lateral component on the opposite hand; ease in to avoid snapping.
        const float blend = std::min(1.f,
            std::max(0.f, static_cast<float>(anim.circle_count) / 10.f));
        const float direction = handIndex == 0 ? 1.f : -1.f;
        localForward = 0.58f * anim.lefthand_movex * blend;
        localSide = 0.58f * anim.lefthand_movey * direction * blend;
        localUp = 0.20f * sinf(anim.lefthand_angle) * blend;
    }
    else if (g_vrCastingArmVisual.releaseStartedMs &&
        now - g_vrCastingArmVisual.releaseStartedMs < 250u)
    {
        // A short, smooth forward release. This is cosmetic only; the real
        // spell launch remains driven by the original casting state machine.
        const float phase = static_cast<float>(
            now - g_vrCastingArmVisual.releaseStartedMs) / 250.f;
        const float weight = sinf(phase * static_cast<float>(PI));
        localForward = 1.05f * weight;
        localUp = 0.22f * weight;
    }
    const float c = cosf(yaw), si = sinf(yaw);
    return { localForward*c - localSide*si, localUp,
        localForward*si + localSide*c };
}

static void vrRenderTrackedBaronyHands(view_t* eyeCamera)
{
    if (!eyeCamera || intro || !g_xrEyeViewsValid || !g_xrSessionRunning ||
        clientnum < 0 || clientnum >= MAXPLAYERS || !players[clientnum] ||
        !players[clientnum]->entity || !players[clientnum]->hud.arm)
    {
        return;
    }

    Entity* const arm = players[clientnum]->hud.arm;
    // Reuse the sprite which actHudArm() selected for this character, including
    // equipped glove/gauntlet voxels and race-specific bare-hand variants.
    // In vanilla actHudWeapon, equipping a weapon deliberately hides the
    // flat-HUD bare arm. Do NOT return here: the VR hand and its weapon need
    // to render independently of that legacy HUD visibility switch.
    // However, retain actual invisibility and unarmed hidden-hand behavior.
    if (players[clientnum]->entity->isInvisible() ||
        (arm->flags[INVISIBLE] &&
            (!stats[clientnum] || !stats[clientnum]->weapon)))
    {
        return;
    }

    // STEP638FR: Keep hand tracking and wearable HUD rendering active in
    // Shaman animal forms, but suppress both humanoid arm voxel models.
    // Evaluate once for both eyes/hands. Equip visuals are separately gated.
    const bool animalForm = stats[clientnum] &&
        (stats[clientnum]->getEffectActive(EFF_SHAPESHIFT) ||
         players[clientnum]->entity->effectShapeshift != NOTHING);
    // STEP638FX: The Hermit controls the Fowl Spirit, not the body's hands.
    // Keep controller tracking and independent wrist/forearm HUD rendering
    // active, but do not draw humanoid arms or equipped held items.
    const bool hideHumanHands = animalForm || (stats[clientnum] &&
        stats[clientnum]->getEffectActive(EFF_PROJECT_SPIRIT));

    const XrVector3f head{
        (g_xrViews[0].pose.position.x + g_xrViews[1].pose.position.x) * .5f,
        (g_xrViews[0].pose.position.y + g_xrViews[1].pose.position.y) * .5f,
        (g_xrViews[0].pose.position.z + g_xrViews[1].pose.position.z) * .5f
    };
    const float cosine = cosf(eyeCamera->ang);
    const float sine = sinf(eyeCamera->ang);

    // Save *everything* we change, because this is Barony's real HUD entity.
    // Restoring immediately after each draw prevents gameplay side effects.
    const int savedSprite = arm->sprite;
    // STEP 616: Query the 4th/5th ACTUAL player limbs, not bodyparts[4]/[5]
    // (those vector positions refer to first-person HUD helpers locally).
    // The native left arm follows race, sex and equipped-glove variations.
    Entity* const leftArmMesh = vrFindNativePlayerArm(
        players[clientnum]->entity, true);
    Entity* const rightArmMesh = vrFindNativePlayerArm(
        players[clientnum]->entity, false);
    {
        static int loggedHud = -9999, loggedLeft = -9999, loggedRight = -9999;
        const int actualLeft = leftArmMesh ? leftArmMesh->sprite : -1;
        const int actualRight = rightArmMesh ? rightArmMesh->sprite : -1;
        if (loggedHud != savedSprite || loggedLeft != actualLeft ||
            loggedRight != actualRight)
        {
            loggedHud = savedSprite;
            loggedLeft = actualLeft;
            loggedRight = actualRight;
            std::printf("OpenXR VR: STEP616 ARM_MESHES hud=%d native_LEFT=%d native_RIGHT=%d leftAvailable=%s\n",
                savedSprite, actualLeft, actualRight,
                (leftArmMesh && actualLeft > 0) ? "YES" : "NO");
        }
    }
    const auto savedX = arm->x;
    const auto savedY = arm->y;
    const auto savedZ = arm->z;
    const auto savedYaw = arm->yaw;
    const auto savedPitch = arm->pitch;
    const auto savedRoll = arm->roll;
    const auto savedScaleX = arm->scalex;
    const auto savedScaleY = arm->scaley;
    const auto savedScaleZ = arm->scalez;
    const auto savedFocalX = arm->focalx;
    const auto savedFocalY = arm->focaly;
    const auto savedFocalZ = arm->focalz;
    const bool savedOverdraw = arm->flags[OVERDRAW];
    const bool savedBright = arm->flags[BRIGHT];
    const bool savedInvisible = arm->flags[INVISIBLE];

    const bool bowOwnsEquipment = !hideHumanHands && vrBowDisplaySelected();
    const bool bowPreviewDrawn = !hideHumanHands && vrDrawBowPreview(eyeCamera);
    static int lastBowPlacement = -1;
    const int bowPlacement = bowPreviewDrawn ?
        (g_vrBowDrawHand + (g_vrArmCalibrationActive ? 2 : 0)) : -1;
    if (bowPlacement != lastBowPlacement) {
        printlog("[VR GA15] bow placement=%d bowHand=%d spellHand=%d calibration=%d",
            bowPlacement,1-g_vrBowDrawHand,vrSpellControllerHand(),g_vrArmCalibrationActive?1:0);
        lastBowPlacement=bowPlacement;
    }
    bool drewAny = false;
    for (int handIndex = 0; handIndex < 2; ++handIndex)
    {
        const VrControllerHandState& hand = g_xrControllerHands[handIndex];
        if (!hand.gripPoseValid)
        {
            continue;
        }
        const XrPosef& pose = hand.gripPose;
        const float right = pose.position.x - head.x;
        const float forward = head.z - pose.position.z;
        const float above = pose.position.y - head.y;

        // The Barony camera position is in tile coordinates (x16 for an
        // Entity). XR +X points right, XR -Z forward, XR +Y up. The game
        // camera's horizontal facing direction supplies the yaw alignment.
        arm->x = eyeCamera->x * 16.0 +
            VR_HAND_UNITS_PER_METER * (forward * cosine - right * sine);
        arm->y = eyeCamera->y * 16.0 +
            VR_HAND_UNITS_PER_METER * (forward * sine + right * cosine);
        arm->z = (eyeCamera->z - 1.0) * .5 -
            VR_HAND_UNITS_PER_METER * above;

        // Rotate the controller's forward vector (-Z) into Barony's local
        // yaw/pitch axes; estimate roll using the controller's up vector.
        // This preserves tracking of all three rotations for the preview,
        // although voxel pivot/handedness calibration is still necessary.
        const XrQuaternionf& q = pose.orientation;
        const float fx = -2.f * (q.x * q.z + q.y * q.w);
        const float fy = -2.f * (q.y * q.z - q.x * q.w);
        const float fz = -1.f + 2.f * (q.x * q.x + q.y * q.y);
        const float worldFX = -fz * cosine - fx * sine;
        const float worldFY = -fz * sine + fx * cosine;
        const float horizontal = sqrtf(worldFX * worldFX + worldFY * worldFY);
        arm->yaw = atan2f(worldFY, worldFX);
        arm->pitch = atan2f(-fy, horizontal);
        // Roll about the pointing axis, calculated from the controller's up
        // vector after removal of the yaw/pitch direction.
        const float ux = 2.f * (q.x * q.y - q.z * q.w);
        const float uy = 1.f - 2.f * (q.x * q.x + q.z * q.z);
        const float uz = 2.f * (q.y * q.z + q.x * q.w);
        const float worldUX = -uz * cosine - ux * sine;
        const float worldUY = -uz * sine + ux * cosine;
        const float side = -sinf(arm->yaw) * worldUX +
            cosf(arm->yaw) * worldUY;
        const float upright = uy * horizontal +
            (worldUX * cosf(arm->yaw) + worldUY * sinf(arm->yaw)) * (-fy);
        arm->roll = atan2f(side, upright);
        // Add the spellcaster's existing circular windup and brief release
        // to the rendered hand pose. The unmodified controller tracking still
        // controls aiming, and any held equipment follows the visible hand.
        const VrGripVector3 castOffset = vrCastingArmVisualOffset(
            handIndex, eyeCamera->ang);
        arm->x += castOffset.x;
        arm->y += castOffset.z;
        arm->z -= castOffset.y;
        // STEP638FZ8: One shared visual hand anchor for both hand voxel and
        // equipped item. Controller poses, weapon launch rays, melee hitboxes
        // and saved per-ItemType grips remain EXACTLY as before.
        vrLoadBodyFit();
        if (g_vrBodyFit.enabled)
        {
            const VrGripBasis3 handBasis = vrGripXRControllerBasis(q, eyeCamera->ang);
            const VrGripVector3 wristShift = vrGripApplyBasis(handBasis,
                { 0.f, VR_HAND_UNITS_PER_METER *
                    g_vrBodyFit.handOffsetMeters[handIndex], 0.f });
            arm->x += wristShift.x;
            arm->y += wristShift.z;
            arm->z -= wristShift.y;
        }

        // STEP619: Use the ACTUAL first-person left-hand mesh that Barony
        // prepares in actLeftHandMagic(), including glove/race variations.
        // Its entity is hidden between spell casts, but its voxel ID remains
        // usable. Avoid a second right-hand mesh or third-person limb.
        const bool useHudLeft = handIndex == 0 &&
            (g_vrLeftArmMeshMode == 1 || g_vrLeftArmMeshMode == 2);
        const bool useFirstPersonLeft = handIndex == 0 &&
            g_vrLeftArmMeshMode == 3;
        if (handIndex == 0)
        {
            if (useFirstPersonLeft)
            {
                const Entity* const magicLeft =
                    players[clientnum]->hud.magicLeftHand;
                if (magicLeft && magicLeft->sprite > 0)
                {
                    arm->sprite = magicLeft->sprite;
                }
                else if (leftArmMesh && leftArmMesh->sprite > 0)
                {
                    // During initialization only: do not render a duplicate
                    // right hand if the magic-left sprite is not ready.
                    arm->sprite = leftArmMesh->sprite;
                }
                else
                    continue;
            }
            else if (useHudLeft)
            {
                if (savedSprite <= 0)
                    continue;
                arm->sprite = savedSprite;
            }
            else
            {
                if (!leftArmMesh || leftArmMesh->sprite <= 0)
                    continue;
                arm->sprite = leftArmMesh->sprite;
            }
        }
        else
            arm->sprite = savedSprite;

        // Use first-person-sized scale for the genuine first-person left
        // arm too. Retain the existing relative left size calibration.
        const float leftScale = (useHudLeft || useFirstPersonLeft) ?
            (g_vrLeftArmScaleFactor / VR_DEFAULT_LEFT_ARM_SCALE_FACTOR) :
            g_vrLeftArmScaleFactor;
        const float armVisualScale = VR_HAND_VOXEL_SCALE *
            (handIndex == 0 ? leftScale : g_vrRightArmScaleFactor) *
            (g_vrBodyFit.enabled ? g_vrBodyFit.handScale[handIndex] : 1.0f);
        arm->scalex = armVisualScale;
        arm->scaley = armVisualScale;
        arm->scalez = armVisualScale;
        if (useHudLeft && g_vrLeftArmMeshMode == 1)
            arm->scaley = -armVisualScale;
        else if (useHudLeft && g_vrLeftArmMeshMode == 2)
            arm->scalex = -armVisualScale;
        arm->focalx = arm->focaly = arm->focalz = 0.0;
        // Force WORLD projection, not Barony's normal flat HUD projection.
        arm->flags[OVERDRAW] = false;
        arm->flags[BRIGHT] = true; // readable in dark areas for calibration
        arm->flags[INVISIBLE] = false;
        // STEP 605: Preview stored translation/rotation on the arm mesh ONLY.
        // Restore original tracked pose before drawing weapons/shield.
        vrLoadVisualArmCalibration();
        const double oldX = arm->x, oldY = arm->y, oldZ = arm->z;
        const double oldYaw = arm->yaw, oldPitch = arm->pitch, oldRoll = arm->roll;
        const auto& calibration = g_vrArmVisual[handIndex];
        const VrGripBasis3 basis = vrGripXRControllerBasis(q, eyeCamera->ang);
        const VrGripVector3 worldOffset = vrGripApplyBasis(basis,
            { calibration.forward, calibration.up, calibration.side });
        arm->x += worldOffset.x;
        arm->y += worldOffset.z;
        arm->z -= worldOffset.y;
        arm->yaw += calibration.yaw;
        arm->pitch += calibration.pitch;
        arm->roll += calibration.roll;
        if (!hideHumanHands)
            glDrawVoxel(eyeCamera, arm, REALCOLORS);
        arm->x = oldX; arm->y = oldY; arm->z = oldZ;
        arm->yaw = oldYaw; arm->pitch = oldPitch; arm->roll = oldRoll;
        // Render only items actually equipped in the corresponding slot.
        // This fixes Step 590's misleading "weapon drawn" diagnostic when
        // the right hand was actually empty.
        // STEP638FO: Barony suppresses world-model equipment in Shaman
        // animal forms. Mirror that rule only in the tracked first-person
        // drawing; no inventory/gameplay/network state is changed.
        if (!hideHumanHands && !bowOwnsEquipment && stats[clientnum] && handIndex == 1)
        {
            Entity* const held = players[clientnum]->hud.weapon;
            Item* const equipped = stats[clientnum]->weapon;
            if (equipped && held && held != arm && held->sprite > 0 &&
                itemCategory(equipped) != SPELLBOOK)
            {
                // Only crossbows use the right OpenXR AIM orientation, which
                // is the very same pose driving the reticle/projectile.
                // Retain the proven GRIP orientation for axes, sickles,
                // spell staves, bows and every previously working weapon.
                const bool vrIsCrossbow = vrIsCrossbowType(equipped);
                const XrQuaternionf* heldOrientation =
                    (vrIsCrossbow && g_xrControllerHands[1].aimPoseValid) ?
                    &g_xrControllerHands[1].aimPose.orientation :
                    &pose.orientation;
                vrDrawTrackedHeldVoxel(eyeCamera, held, arm, true,
                    equipped, heldOrientation);
                static int lastWeaponSprite = -1;
                static int lastWeaponType = -1;
                if (lastWeaponSprite != held->sprite ||
                    lastWeaponType != static_cast<int>(equipped->type))
                {
                    const VrWeaponGripProfile grip = vrWeaponGripForItem(equipped);
                    std::printf("OpenXR VR: STEP603 RIGHT WEAPON type=%d sprite=%d nativeScale=%.2f pitchDeg=%.1f forward=%.4f side=%.2f up=%.2f localDown=%.3f ALL_WEAPONS_RIGID.\n",
                        static_cast<int>(equipped->type), held->sprite,
                        static_cast<double>(held->scalex),
                        static_cast<double>(grip.pitchOffset * 180.0f / static_cast<float>(PI)),
                        static_cast<double>(grip.forwardOffset),
                        static_cast<double>(grip.sideOffset),
                        static_cast<double>(grip.upOffset),
                        static_cast<double>(grip.localGripDownOffset));
                    lastWeaponSprite = held->sprite;
                    lastWeaponType = static_cast<int>(equipped->type);
                }
            }
        }
        if (!hideHumanHands && !bowOwnsEquipment && stats[clientnum] && handIndex == 0)
        {
            Entity* const held = vrFindLocalHudShield();
            // STEP638FD: Quivers belong to the player's equipment, not
            // physically held left-hand objects. Hide their tracked VR mesh
            // with or WITHOUT a crossbow in the right hand. They remain
            // equipped and still supply vanilla ammo/bonuses to gameplay.
            // The independent right-hand loaded bolt remains untouched.
            const bool quiverEquipped = stats[clientnum]->shield &&
                itemTypeIsQuiver(stats[clientnum]->shield->type);
            if (!quiverEquipped && stats[clientnum]->shield &&
                held && held != arm &&
                (held->sprite > 0 ||
                 itemCategory(stats[clientnum]->shield) == SPELLBOOK) &&
                (!held->flags[INVISIBLE] ||
                 itemCategory(stats[clientnum]->shield) == SPELLBOOK))
            {
                // STEP 604: Arm-only yaw was restored immediately after
                // glDrawVoxel(). The shield still uses its original rigid
                // controller orientation and its tested grip position.
                vrDrawTrackedHeldVoxel(eyeCamera, held, arm, false,
                    stats[clientnum]->shield, &pose.orientation);
                static int lastShieldSprite = -1;
                if (lastShieldSprite != held->sprite)
                {
                    std::printf("OpenXR VR: STEP600 LEFT OFFHAND sprite=%d nativeScale=%.2f SHIELD_RIGID_FORWARD_90_ARM_ROLL_MINUS_90.\n",
                        held->sprite, static_cast<double>(held->scalex));
                    lastShieldSprite = held->sprite;
                }
            }
            else if (stats[clientnum]->shield && !held)
            {
                static bool missingShieldLogged = false;
                if (!missingShieldLogged)
                {
                    std::printf("OpenXR VR: STEP593 shield equipped but actHudShield entity not found.\n");
                    missingShieldLogged = true;
                }
            }
        }
        // STEP 638EE: Draw the two wearable panels AFTER equipped items;
        // source HUD entities are restored immediately after each sprite.
        vrDrawWearableHudFoundation(eyeCamera, handIndex, arm, pose.orientation);
        drewAny = true;
    }

    arm->x = savedX;
    arm->y = savedY;
    arm->z = savedZ;
    arm->yaw = savedYaw;
    arm->pitch = savedPitch;
    arm->roll = savedRoll;
    arm->scalex = savedScaleX;
    arm->scaley = savedScaleY;
    arm->scalez = savedScaleZ;
    arm->focalx = savedFocalX;
    arm->focaly = savedFocalY;
    arm->focalz = savedFocalZ;
    arm->flags[OVERDRAW] = savedOverdraw;
    arm->flags[BRIGHT] = savedBright;
    arm->flags[INVISIBLE] = savedInvisible;
    arm->sprite = savedSprite;

    if (drewAny)
    {
        static unsigned counter = 0;
        if ((++counter % 600) == 1)
        {
            std::printf("OpenXR VR: tracked Barony hand/glove voxels drawn (sprite=%d)\n",
                arm->sprite);
        }
    }
}
// STEP638FX: VR-only camera handoff after the Hermit's Fowl Spirit ends.
// The vanilla ghost camera is authoritative while projecting. On return,
// Barony can still supply an interpolated ghost position while its player
// camera controller resumes. Anchor only the two VR eye cameras to the
// original body until the vanilla camera catches up. No camera/entity
// globals, multiplayer state, or HUD values are altered.
struct VrHermitReturnView
{
    bool active = false;
    double x = 0.0, y = 0.0, z = 0.0, yaw = 0.0, pitch = 0.0;
};
static VrHermitReturnView vrResolveHermitReturnView(const view_t* vanilla)
{
    VrHermitReturnView result{};
    if (!g_vrHermitReturnCameraPending || !vanilla || intro ||
        clientnum < 0 || clientnum >= MAXPLAYERS ||
        !players[clientnum] || !players[clientnum]->entity ||
        !stats[clientnum] ||
        stats[clientnum]->getEffectActive(EFF_PROJECT_SPIRIT))
        return result;

    Entity* const body = players[clientnum]->entity;
    const double bodyX = body->x / 16.0;
    const double bodyY = body->y / 16.0;
    // Same baseline as PlayerMovement_t::handlePlayerCameraPosition();
    // Hermit's signature race is Myconid (-1 camera-height adjustment).
    const double bodyZ = body->z * 2.0 - 2.5 -
        (body->getMonsterTypeFromSprite() == MYCONID ? 1.0 : 0.0);
    const double yawDiff = std::atan2(std::sin(vanilla->ang - body->yaw),
                                      std::cos(vanilla->ang - body->yaw));
    const bool vanillaAtBody =
        std::fabs(vanilla->x - bodyX) < 0.20 &&
        std::fabs(vanilla->y - bodyY) < 0.20 &&
        std::fabs(vanilla->z - bodyZ) < 1.25 &&
        std::fabs(yawDiff) < 0.17 &&
        std::fabs(vanilla->vang - body->pitch) < 0.17;
    if (vanillaAtBody)
    {
        g_vrHermitReturnCameraPending = false;
        std::printf("OpenXR VR STEP638FX: Hermit body camera recovered; normal camera resumed.\n");
        return result;
    }

    result.active = true;
    result.x = bodyX;
    result.y = bodyY;
    result.z = bodyZ;
    result.yaw = body->yaw;
    result.pitch = body->pitch;
    return result;
}
static void vrApplyHermitReturnView(view_t& eye, const VrHermitReturnView& anchor)
{
    if (!anchor.active)
        return;
    eye.x = anchor.x;
    eye.y = anchor.y;
    eye.z = anchor.z;
    eye.ang = anchor.yaw;
    eye.vang = anchor.pitch;
}


void vrRenderStereoTestFrame(view_t* camera)
{
    // Verify that Barony's dungeon camera reaches the stereo renderer.
    static bool cameraLogged = false;

    if (camera != nullptr && !cameraLogged)
    {
        SDL_Log("OpenXR: Dungeon camera received by stereo renderer!");
        cameraLogged = true;
    }
    // Sample ONE return anchor for both eyes to preserve stereo parity.
    const VrHermitReturnView hermitReturnView = vrResolveHermitReturnView(camera);
    // Assume this frame is not ready until rendering succeeds.
    g_xrStereoFrameReady = false;
    // Diagnostic: verify Barony's GUI texture is available to OpenXR.
    static bool guiTextureLogged = false;

    if (!guiTextureLogged)
    {
        const unsigned int guiTexture = vrGetGuiTexture();
        GLint guiWidth = 0;
        GLint guiHeight = 0;

        GLint previousTexture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

        glBindTexture(GL_TEXTURE_2D, guiTexture);

        glGetTexLevelParameteriv(
            GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &guiWidth
        );

        glGetTexLevelParameteriv(
            GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &guiHeight
        );

        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));

        SDL_Log(
            "OpenXR GUI: texture dimensions = %d x %d",
            guiWidth, guiHeight
        );
        // Check whether the GUI texture contains any rendered pixels.
        if (guiWidth > 0 && guiHeight > 0)
        {
            GLint previousReadFramebuffer = 0;
            glGetIntegerv(
                GL_READ_FRAMEBUFFER_BINDING,
                &previousReadFramebuffer
            );

            GLuint diagnosticFramebuffer = 0;
            glGenFramebuffers(1, &diagnosticFramebuffer);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, diagnosticFramebuffer);

            glFramebufferTexture2D(
                GL_READ_FRAMEBUFFER,
                GL_COLOR_ATTACHMENT0,
                GL_TEXTURE_2D,
                guiTexture,
                0
            );
            const GLenum guiFramebufferStatus =
                glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);

            SDL_Log(
                "OpenXR GUI: framebuffer status = 0x%X",
                static_cast<unsigned int>(guiFramebufferStatus)
            );

            if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)
                == GL_FRAMEBUFFER_COMPLETE)
            {
                unsigned char pixel[4] = {};

                glReadPixels(
                    guiWidth / 2,
                    guiHeight / 2,
                    1, 1,
                    GL_RGBA,
                    GL_UNSIGNED_BYTE,
                    pixel
                );

                SDL_Log(
                    "OpenXR GUI: center pixel RGBA = %u, %u, %u, %u",
                    pixel[0], pixel[1], pixel[2], pixel[3]
                );
            }

            glBindFramebuffer(
                GL_READ_FRAMEBUFFER,
                static_cast<GLuint>(previousReadFramebuffer)
            );

            glDeleteFramebuffers(1, &diagnosticFramebuffer);
        }

        SDL_Log(
            "OpenXR GUI: framebuffer texture ID = %u",
            guiTexture
        );

        guiTextureLogged = true;
    }
    // Only render when the VR session and swapchain are ready.
    if (!g_xrSessionRunning ||
        g_xrSession == XR_NULL_HANDLE ||
        g_xrSwapchain == XR_NULL_HANDLE)
    {
        return;
    }
    // Create the VR depth-stencil renderbuffer once.
    if (g_xrDepthRenderbuffer == 0 &&
        g_glGenRenderbuffers &&
        g_glBindRenderbuffer &&
        g_glRenderbufferStorage)
    {
        g_glGenRenderbuffers(1, &g_xrDepthRenderbuffer);

        g_glBindRenderbuffer(
            GL_RENDERBUFFER,
            g_xrDepthRenderbuffer
        );

        g_glRenderbufferStorage(
            GL_RENDERBUFFER,
            GL_DEPTH32F_STENCIL8,
            static_cast<GLsizei>(g_xrEyeWidth),
            static_cast<GLsizei>(g_xrEyeHeight)
        );

        g_glBindRenderbuffer(GL_RENDERBUFFER, 0);

        std::printf("OpenXR: VR depth-stencil renderbuffer created!\n");
    }
    // Request the next available stereo swapchain image.
    XrSwapchainImageAcquireInfo acquireInfo{};
    acquireInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;

    uint32_t imageIndex = 0;

    XrResult result = xrAcquireSwapchainImage(
        g_xrSwapchain,
        &acquireInfo,
        &imageIndex
    );

    if (XR_FAILED(result))
    {
        std::printf(
            "OpenXR: Failed to acquire stereo image (%d)\n",
            static_cast<int>(result)
        );
        return;
    }
    // Wait until the acquired image is ready for rendering.
    XrSwapchainImageWaitInfo waitInfo{};
    waitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    waitInfo.timeout = XR_INFINITE_DURATION;

    result = xrWaitSwapchainImage(
        g_xrSwapchain,
        &waitInfo
    );

    if (XR_FAILED(result))
    {
        std::printf(
            "OpenXR: Failed to wait for stereo image (%d)\n",
            static_cast<int>(result)
        );
    }

    // Remember Barony's current framebuffer so we can restore it.
    GLint previousFramebuffer = 0;

    glGetIntegerv(
        GL_FRAMEBUFFER_BINDING,
        &previousFramebuffer
    );
    // Remember Barony's original OpenGL clear color.
    GLfloat previousClearColor[4] = {};

    glGetFloatv(
        GL_COLOR_CLEAR_VALUE,
        previousClearColor
    );
    // Remember whether Barony has the scissor test enabled.
    GLboolean previousScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    // Remember whether OpenGL blending is enabled.
    GLboolean previousBlendEnabled = glIsEnabled(GL_BLEND);
    // Remember which color channels Barony allows OpenGL to write.
    GLboolean previousColorMask[4] = {};

    glGetBooleanv(
        GL_COLOR_WRITEMASK,
        previousColorMask
    );
    // Remember Barony's original viewport.
    GLint previousViewport[4] = {};

    glGetIntegerv(
        GL_VIEWPORT,
        previousViewport
    );
    // Switch OpenGL to our VR framebuffer.
    g_glBindFramebuffer(
        GL_FRAMEBUFFER,
        g_xrFramebuffer
    );
    // Explicitly direct VR rendering into color attachment 0.
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    // Attach the VR depth-stencil renderbuffer.
    if (g_xrDepthRenderbuffer != 0 &&
        g_glFramebufferRenderbuffer)
    {
        g_glFramebufferRenderbuffer(
            GL_FRAMEBUFFER,
            GL_DEPTH_STENCIL_ATTACHMENT,
            GL_RENDERBUFFER,
            g_xrDepthRenderbuffer
        );
    }
    // Allow VR rendering to cover the entire eye texture.
    glDisable(GL_SCISSOR_TEST);
    // Disable blending while rendering the stereo test images.
    glDisable(GL_BLEND);
    // Allow VR rendering to write red, green, blue, and alpha.
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    // Set the viewport to the full VR eye resolution.
    glViewport(
        0,
        0,
        static_cast<GLsizei>(g_xrEyeWidth),
        static_cast<GLsizei>(g_xrEyeHeight)
    );
    // Attach the left-eye texture layer from the acquired swapchain image.
    g_glFramebufferTextureLayer(
        GL_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        g_xrSwapchainImages[imageIndex].image,
        0,
        0
    );
    // Verify that the left-eye framebuffer is ready.
    GLenum framebufferStatus = g_glCheckFramebufferStatus(
        GL_FRAMEBUFFER
    );

    if (framebufferStatus != GL_FRAMEBUFFER_COMPLETE)
    {
        std::printf(
            "OpenXR: Left-eye framebuffer incomplete (0x%X)\n",
            static_cast<unsigned int>(framebufferStatus)
        );
    }
    // Render a solid blue test image into the left-eye texture.
    if (framebufferStatus == GL_FRAMEBUFFER_COMPLETE)
    {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    // Create a separate camera for VR so the desktop camera stays unchanged.
    view_t vrLeftCamera{};
    std::unique_ptr<bool[]> vrLeftVismap(
        new bool[map.width * map.height]
    );
    if (camera != nullptr)
    {
        // Position the left eye slightly to the left of the player camera.
        // Calculate the left-eye position directly in this camera's scope.
        vrLeftCamera.x = camera->x;
        vrLeftCamera.y = camera->y;
        vrLeftCamera.z = camera->z;
        vrLeftCamera.z = camera->z;
        // Future left-eye positional offset for stereoscopic rendering.
// Keep the original position until the eye-space conversion is ready.
        // Read the left eye's tracked position in OpenXR space (meters).
        const float leftEyeOffsetX = g_xrViews[0].pose.position.x;
        const float leftEyeOffsetY = g_xrViews[0].pose.position.y;
        // Horizontal separation between the tracked eyes, in meters.
        const float eyeSeparationX =
            g_xrViews[1].pose.position.x -
            g_xrViews[0].pose.position.x;
        // Calculate the midpoint between the tracked eyes.
        const float eyeCenterX =
            (g_xrViews[0].pose.position.x +
                g_xrViews[1].pose.position.x) * 0.5f;
        // Left eye's horizontal offset from the midpoint, in meters.
        const float leftEyeRelativeX =
            g_xrViews[0].pose.position.x - eyeCenterX;
        // Right eye's horizontal offset from the midpoint, in meters.
        const float rightEyeRelativeX =
            g_xrViews[1].pose.position.x - eyeCenterX;
        // Initial stereo scale: 1 meter = 1 Barony camera unit.
// This is a temporary calibration value.
        const float vrMetersToCameraUnits = 1.0f;

        const float leftEyeCameraOffset =
            leftEyeRelativeX * vrMetersToCameraUnits;
        // Convert the right eye's centered offset to Barony camera units.
        const float rightEyeCameraOffset =
            rightEyeRelativeX * vrMetersToCameraUnits;
        // Calculate eye separation independently of headset rotation.
        const float eyeDeltaX =
            g_xrViews[1].pose.position.x - g_xrViews[0].pose.position.x;

        const float eyeDeltaY =
            g_xrViews[1].pose.position.y - g_xrViews[0].pose.position.y;

        const float eyeDeltaZ =
            g_xrViews[1].pose.position.z - g_xrViews[0].pose.position.z;

        const float eyeSeparationMeters = sqrtf(
            eyeDeltaX * eyeDeltaX +
            eyeDeltaY * eyeDeltaY +
            eyeDeltaZ * eyeDeltaZ
        );
        // Left eye sits half the measured IPD to the left
// of the headset's center.
        const float leftEyeStereoOffset =
            -0.5f * eyeSeparationMeters * vrMetersToCameraUnits;
        // Right eye sits half the measured IPD to the right
// of the headset's center.
        const float rightEyeStereoOffset =
            0.5f * eyeSeparationMeters * vrMetersToCameraUnits;
        // Calculate the sideways direction relative to the
        // player's current facing angle.
        const float vrCameraRightX =
            camera != nullptr ? -sinf(camera->ang) : 0.0f;
        // Y component of the camera's sideways direction.
        const float vrCameraRightY =
            camera != nullptr ? cosf(camera->ang) : 0.0f;
        // Left eye's X offset in Barony world coordinates.
        const float leftEyeWorldOffsetX =
            leftEyeStereoOffset * vrCameraRightX;

        // Left eye's Y offset in Barony world coordinates.
        const float leftEyeWorldOffsetY =
            leftEyeStereoOffset * vrCameraRightY;

        // Temporarily disable stereo positioning for projection testing.
        vrLeftCamera.x = camera->x;
        vrLeftCamera.y = camera->y;
        // Right eye's X offset in Barony world coordinates.
        const float rightEyeWorldOffsetX =
            rightEyeStereoOffset * vrCameraRightX;
        // Right eye's Y offset in Barony world coordinates.
        const float rightEyeWorldOffsetY =
            rightEyeStereoOffset * vrCameraRightY;
        // Log the tracked eye separation once for verification.
        static bool eyeSeparationLogged = false;

        if (g_xrEyeViewsValid && !eyeSeparationLogged)
        {
            SDL_Log(
                "OpenXR: Eye separation X = %.4f meters",
                eyeSeparationX
            );

            eyeSeparationLogged = true;
        }
        vrLeftCamera.ang = camera->ang;
        vrLeftCamera.vang = camera->vang;
        vrLeftCamera.winx = 0;
        vrLeftCamera.winy = 0;
        vrLeftCamera.winw = static_cast<Sint32>(g_xrEyeWidth);
        vrLeftCamera.winh = static_cast<Sint32>(g_xrEyeHeight);
        vrLeftCamera.vismap = vrLeftVismap.get();
        vrLeftCamera.globalLightModifier = camera->globalLightModifier;
        vrLeftCamera.globalLightModifierEntities = camera->globalLightModifierEntities;
        vrLeftCamera.globalLightModifierActive = camera->globalLightModifierActive;
        vrLeftCamera.luminance = camera->luminance;
        vrApplyHermitReturnView(vrLeftCamera, hermitReturnView);
    }
    // Use the headset eye resolution for left-eye dungeon rendering.
    vrCameraViewportEnabled = true;
    // Apply headset rotation while rendering both VR eyes.
    vrCameraTrackingEnabled = true;
    // STEP638CZ: prevent duplicate screen-fixed light particles in VR only.
    const auto vrOriginalHudFlames = vrHideOriginalHudLightFlames();
    if (camera != nullptr)
    {
        occlusionCulling(map, vrLeftCamera);
    }
    // Future insertion point for rendering Barony's left-eye dungeon view.
    // // VR dungeon rendering will use Barony's existing camera and draw functions.
    // glBeginCamera(camera, false, map);
    // glDrawWorld(camera, REALCOLORS);
    // drawEntities3D(camera, REALCOLORS);
    // glEndCamera(camera, false, map);
    // Left-eye rendering activation will go here.
    // // Render Barony's dungeon into the left-eye VR texture.
    if (camera != nullptr &&
        framebufferStatus == GL_FRAMEBUFFER_COMPLETE)
    {
        // Build the Quest 3's actual left-eye projection matrix.
        vrBuildEyeProjection(
            g_xrViews[0].fov,
            CLIPNEAR,
            CLIPFAR,
            vrEyeProjectionMatrix
        );

        vrEyeProjectionEnabled = true;
        glBeginCamera(&vrLeftCamera, false, map);
        glDrawWorld(&vrLeftCamera, REALCOLORS);
        Entity* vrHudArm = (clientnum >= 0 && clientnum < MAXPLAYERS &&
            players[clientnum]) ? players[clientnum]->hud.arm : nullptr;
        Entity* vrHudWeapon = (clientnum >= 0 && clientnum < MAXPLAYERS &&
            players[clientnum]) ? players[clientnum]->hud.weapon : nullptr;
        Entity* vrHudShield = vrFindLocalHudShield();
        Entity* vrHudBolt = vrFindLocalHudArrowModel();
        // STEP638FT: The vanilla flail chain is its own OVERDRAW entity,
        // actHudAdditional2(), independent of the HUD weapon handle.
        // Hide it only in this VR eye when its physics-driven voxel is ready.
        Entity* vrHudFlailChain = g_vrFlailVisualEnabled &&
            g_xrControllerHands[1].gripPoseValid &&
            clientnum >= 0 && clientnum < MAXPLAYERS && stats[clientnum] &&
            stats[clientnum]->weapon &&
            stats[clientnum]->weapon->type == STEEL_FLAIL ?
            vrFindLocalHudAdditional(true) : nullptr;
        const bool vrHudFlailChainWasInvisible = vrHudFlailChain ?
            vrHudFlailChain->flags[INVISIBLE] : true;
        // STEP 638DJ: The vanilla spell windup hands are screen-fixed HUD
        // geometry. Hide them only for this stereo eye's world pass; keep
        // actLeftHandMagic/actRightHandMagic updating casting state and sprites.
        Entity* vrHudMagicLeft = (clientnum >= 0 && clientnum < MAXPLAYERS &&
            players[clientnum]) ? players[clientnum]->hud.magicLeftHand : nullptr;
        Entity* vrHudMagicRight = (clientnum >= 0 && clientnum < MAXPLAYERS &&
            players[clientnum]) ? players[clientnum]->hud.magicRightHand : nullptr;
        const bool vrMagicLeftWasInvisible = vrHudMagicLeft ?
            vrHudMagicLeft->flags[INVISIBLE] : true;
        const bool vrMagicRightWasInvisible = vrHudMagicRight ?
            vrHudMagicRight->flags[INVISIBLE] : true;
        const bool vrTrackedSpellbook = clientnum >= 0 &&
            clientnum < MAXPLAYERS && stats[clientnum] &&
            stats[clientnum]->shield &&
            itemCategory(stats[clientnum]->shield) == SPELLBOOK &&
            g_xrControllerHands[0].gripPoseValid;
        Entity* vrHudBookExtra = vrTrackedSpellbook ?
            vrFindLocalHudAdditional(false) : nullptr;
        Entity* vrHudBookExtra2 = vrTrackedSpellbook ?
            vrFindLocalHudAdditional(true) : nullptr;
        const bool vrBookExtraWasInvisible = vrHudBookExtra ?
            vrHudBookExtra->flags[INVISIBLE] : true;
        const bool vrBookExtra2WasInvisible = vrHudBookExtra2 ?
            vrHudBookExtra2->flags[INVISIBLE] : true;
        const bool vrHudBoltWasInvisible = vrHudBolt ?
            vrHudBolt->flags[INVISIBLE] : true;
        const bool vrTrackCrossbowBolt =
            g_xrControllerHands[1].gripPoseValid &&
            clientnum >= 0 && clientnum < MAXPLAYERS &&
            stats[clientnum] &&
            vrCrossbowBoltShouldBeVisible(vrHudBolt, vrHudWeapon,
                stats[clientnum]->weapon, stats[clientnum]->shield);
        // STEP 638EE: The original HUD entity is hidden below, but its
        // pre-suppression visibility must drive the crossbow-mounted child.
        g_vrLoadedCrossbowBoltForEye = vrTrackCrossbowBolt;
        const bool vrHudShieldWasInvisible = vrHudShield ?
            vrHudShield->flags[INVISIBLE] : false;
        const bool vrHudShieldWasDithered = vrHudShield ?
            vrHudShield->flags[INVISIBLE_DITHER] : false;
        const bool vrHudArmWasInvisible = vrHudArm ?
            vrHudArm->flags[INVISIBLE] : false;
        const bool vrHudWeaponWasInvisible = vrHudWeapon ?
            vrHudWeapon->flags[INVISIBLE] : false;
        // Prevent duplicate original screen-fixed arm/weapon in stereo.
        if (vrHudArm) vrHudArm->flags[INVISIBLE] = true;
        // Only suppress a conventional HUD item if its tracked replacement
        // has a valid controller pose and a drawable model this frame.
        if (vrHudWeapon && vrHudWeapon->sprite > 0 &&
            g_xrControllerHands[1].gripPoseValid &&
            clientnum >= 0 && clientnum < MAXPLAYERS &&
            stats[clientnum] && stats[clientnum]->weapon)
            vrHudWeapon->flags[INVISIBLE] = true;
        if (vrHudShield &&
            (vrHudShield->sprite > 0 || vrTrackedSpellbook) &&
            g_xrControllerHands[0].gripPoseValid &&
            clientnum >= 0 && clientnum < MAXPLAYERS &&
            stats[clientnum] && stats[clientnum]->shield &&
            (!vrHudShieldWasInvisible || vrTrackedSpellbook))
        {
            vrHudShield->flags[INVISIBLE] = true;
            vrHudShield->flags[INVISIBLE_DITHER] = false;
        }
        // STEP 602: The original HUD bolt is a separate screen-fixed model.
        // Replace it with the tracked crossbow-child draw after the world pass.
        if (vrHudBolt && (vrTrackCrossbowBolt || vrBowDisplaySelected()))
            vrHudBolt->flags[INVISIBLE] = true;
        // Suppress the leftover fixed book page/half for this VR eye only.
        if (vrHudBookExtra) vrHudBookExtra->flags[INVISIBLE] = true;
        if (vrHudBookExtra2) vrHudBookExtra2->flags[INVISIBLE] = true;
        if (vrHudFlailChain) vrHudFlailChain->flags[INVISIBLE] = true;
        // Suppress only the untracked vanilla casting hand models.
        // Spell sparks and the game's underlying animation manager continue.
        if (vrHudMagicLeft) vrHudMagicLeft->flags[INVISIBLE] = true;
        if (vrHudMagicRight) vrHudMagicRight->flags[INVISIBLE] = true;
        drawEntities3D(&vrLeftCamera, REALCOLORS);
        if (vrHudMagicLeft) vrHudMagicLeft->flags[INVISIBLE] = vrMagicLeftWasInvisible;
        if (vrHudMagicRight) vrHudMagicRight->flags[INVISIBLE] = vrMagicRightWasInvisible;
        if (vrHudBookExtra) vrHudBookExtra->flags[INVISIBLE] = vrBookExtraWasInvisible;
        if (vrHudBookExtra2) vrHudBookExtra2->flags[INVISIBLE] = vrBookExtra2WasInvisible;
        if (vrHudFlailChain) vrHudFlailChain->flags[INVISIBLE] = vrHudFlailChainWasInvisible;
        if (vrHudBolt) vrHudBolt->flags[INVISIBLE] = vrHudBoltWasInvisible;
        if (vrHudArm) vrHudArm->flags[INVISIBLE] = vrHudArmWasInvisible;
        if (vrHudWeapon) vrHudWeapon->flags[INVISIBLE] = vrHudWeaponWasInvisible;
        if (vrHudShield)
        {
            vrHudShield->flags[INVISIBLE] = vrHudShieldWasInvisible;
            vrHudShield->flags[INVISIBLE_DITHER] = vrHudShieldWasDithered;
        }
        vrRenderTrackedBaronyHands(&vrLeftCamera);
        vrDrawSpatialHud(&vrLeftCamera);
        g_vrLoadedCrossbowBoltForEye = false;
        // STEP 638EA: Submit selected in-world aim art into EACH stereo eye.
        // Step 638DZ implemented the helper but never called it; the HUD
        // reticles are still rendered separately on the OpenXR GUI layer.
        vrDrawControllerAimReticle(&vrLeftCamera);
        // STEP 638ED: Stereo world guides only; GUI keeps block/charge cues.
        glEndCamera(&vrLeftCamera, false, map);
    }
    // The VR camera is prepared; keep the stereo test active.
    static bool leftEyeCameraReadyLogged = false;
    if (camera != nullptr && !leftEyeCameraReadyLogged)
    {
        SDL_Log("OpenXR: Left-eye dungeon camera prepared: %d x %d",
            vrLeftCamera.winw, vrLeftCamera.winh);
        leftEyeCameraReadyLogged = true;
    }
    // The left-eye swapchain texture is currently attached to our framebuffer.
    // Switch the framebuffer to the right-eye texture layer.
    g_glFramebufferTextureLayer(
        GL_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        g_xrSwapchainImages[imageIndex].image,
        0,
        1
    );
    // Verify that the right-eye framebuffer is ready.
    GLenum rightFramebufferStatus = g_glCheckFramebufferStatus(
        GL_FRAMEBUFFER
    );

    if (rightFramebufferStatus != GL_FRAMEBUFFER_COMPLETE)
    {
        std::printf(
            "OpenXR: Right-eye framebuffer incomplete (0x%X)\n",
            static_cast<unsigned int>(rightFramebufferStatus)
        );
    }
    // Render a solid red test image into the right-eye texture.
    if (rightFramebufferStatus == GL_FRAMEBUFFER_COMPLETE)
    {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    // Create a separate right-eye camera.
    view_t vrRightCamera{};
    std::unique_ptr<bool[]> vrRightVismap(
        new bool[map.width * map.height]
    );
    if (camera != nullptr)
    {
        // Position the right eye half an IPD to the right.
        const float eyeDeltaX =
            g_xrViews[1].pose.position.x - g_xrViews[0].pose.position.x;
        const float eyeDeltaY =
            g_xrViews[1].pose.position.y - g_xrViews[0].pose.position.y;
        const float eyeDeltaZ =
            g_xrViews[1].pose.position.z - g_xrViews[0].pose.position.z;

        const float eyeSeparationMeters = sqrtf(
            eyeDeltaX * eyeDeltaX +
            eyeDeltaY * eyeDeltaY +
            eyeDeltaZ * eyeDeltaZ
        );

        const float rightEyeOffset = 0.5f * eyeSeparationMeters;

        // Temporarily disable the right-eye position offset for diagnosis.
        vrRightCamera.x = camera->x;
        vrRightCamera.y = camera->y;

        vrRightCamera.z = camera->z;
        // Future right-eye positional offset for stereoscopic rendering.
// Keep the original position until the eye-space conversion is ready.
        // Read the right eye's tracked position in OpenXR space (meters).
        const float rightEyeOffsetX = g_xrViews[1].pose.position.x;
        const float rightEyeOffsetY = g_xrViews[1].pose.position.y;

        vrRightCamera.ang = camera->ang;
        vrRightCamera.vang = camera->vang;
        vrRightCamera.winx = 0;
        vrRightCamera.winy = 0;
        vrRightCamera.winw = static_cast<Sint32>(g_xrEyeWidth);
        vrRightCamera.winh = static_cast<Sint32>(g_xrEyeHeight);
        vrRightCamera.vismap = vrRightVismap.get();
        vrRightCamera.globalLightModifier = camera->globalLightModifier;
        vrRightCamera.globalLightModifierEntities = camera->globalLightModifierEntities;
        vrRightCamera.globalLightModifierActive = camera->globalLightModifierActive;
        vrRightCamera.luminance = camera->luminance;
        vrApplyHermitReturnView(vrRightCamera, hermitReturnView);
    }
    // Future insertion point for rendering Barony's right-eye dungeon view.
    // Render Barony's dungeon into the right-eye VR texture.
    if (camera != nullptr &&
        rightFramebufferStatus == GL_FRAMEBUFFER_COMPLETE)
    {
        // Build the Quest 3's actual right-eye projection matrix.
        vrBuildEyeProjection(
            g_xrViews[1].fov,
            CLIPNEAR,
            CLIPFAR,
            vrEyeProjectionMatrix
        );

        vrEyeProjectionEnabled = true;
        if (camera != nullptr)
        {
            occlusionCulling(map, vrRightCamera);
        }
        glBeginCamera(&vrRightCamera, false, map);
        glDrawWorld(&vrRightCamera, REALCOLORS);
        Entity* vrHudArm = (clientnum >= 0 && clientnum < MAXPLAYERS &&
            players[clientnum]) ? players[clientnum]->hud.arm : nullptr;
        Entity* vrHudWeapon = (clientnum >= 0 && clientnum < MAXPLAYERS &&
            players[clientnum]) ? players[clientnum]->hud.weapon : nullptr;
        Entity* vrHudShield = vrFindLocalHudShield();
        Entity* vrHudBolt = vrFindLocalHudArrowModel();
        // STEP638FT: The vanilla flail chain is its own OVERDRAW entity,
        // actHudAdditional2(), independent of the HUD weapon handle.
        // Hide it only in this VR eye when its physics-driven voxel is ready.
        Entity* vrHudFlailChain = g_vrFlailVisualEnabled &&
            g_xrControllerHands[1].gripPoseValid &&
            clientnum >= 0 && clientnum < MAXPLAYERS && stats[clientnum] &&
            stats[clientnum]->weapon &&
            stats[clientnum]->weapon->type == STEEL_FLAIL ?
            vrFindLocalHudAdditional(true) : nullptr;
        const bool vrHudFlailChainWasInvisible = vrHudFlailChain ?
            vrHudFlailChain->flags[INVISIBLE] : true;
        // STEP 638DJ: The vanilla spell windup hands are screen-fixed HUD
        // geometry. Hide them only for this stereo eye's world pass; keep
        // actLeftHandMagic/actRightHandMagic updating casting state and sprites.
        Entity* vrHudMagicLeft = (clientnum >= 0 && clientnum < MAXPLAYERS &&
            players[clientnum]) ? players[clientnum]->hud.magicLeftHand : nullptr;
        Entity* vrHudMagicRight = (clientnum >= 0 && clientnum < MAXPLAYERS &&
            players[clientnum]) ? players[clientnum]->hud.magicRightHand : nullptr;
        const bool vrMagicLeftWasInvisible = vrHudMagicLeft ?
            vrHudMagicLeft->flags[INVISIBLE] : true;
        const bool vrMagicRightWasInvisible = vrHudMagicRight ?
            vrHudMagicRight->flags[INVISIBLE] : true;
        const bool vrTrackedSpellbook = clientnum >= 0 &&
            clientnum < MAXPLAYERS && stats[clientnum] &&
            stats[clientnum]->shield &&
            itemCategory(stats[clientnum]->shield) == SPELLBOOK &&
            g_xrControllerHands[0].gripPoseValid;
        Entity* vrHudBookExtra = vrTrackedSpellbook ?
            vrFindLocalHudAdditional(false) : nullptr;
        Entity* vrHudBookExtra2 = vrTrackedSpellbook ?
            vrFindLocalHudAdditional(true) : nullptr;
        const bool vrBookExtraWasInvisible = vrHudBookExtra ?
            vrHudBookExtra->flags[INVISIBLE] : true;
        const bool vrBookExtra2WasInvisible = vrHudBookExtra2 ?
            vrHudBookExtra2->flags[INVISIBLE] : true;
        const bool vrHudBoltWasInvisible = vrHudBolt ?
            vrHudBolt->flags[INVISIBLE] : true;
        const bool vrTrackCrossbowBolt =
            g_xrControllerHands[1].gripPoseValid &&
            clientnum >= 0 && clientnum < MAXPLAYERS &&
            stats[clientnum] &&
            vrCrossbowBoltShouldBeVisible(vrHudBolt, vrHudWeapon,
                stats[clientnum]->weapon, stats[clientnum]->shield);
        // STEP 638EE: The original HUD entity is hidden below, but its
        // pre-suppression visibility must drive the crossbow-mounted child.
        g_vrLoadedCrossbowBoltForEye = vrTrackCrossbowBolt;
        const bool vrHudShieldWasInvisible = vrHudShield ?
            vrHudShield->flags[INVISIBLE] : false;
        const bool vrHudShieldWasDithered = vrHudShield ?
            vrHudShield->flags[INVISIBLE_DITHER] : false;
        const bool vrHudArmWasInvisible = vrHudArm ?
            vrHudArm->flags[INVISIBLE] : false;
        const bool vrHudWeaponWasInvisible = vrHudWeapon ?
            vrHudWeapon->flags[INVISIBLE] : false;
        // Prevent duplicate original screen-fixed arm/weapon in stereo.
        if (vrHudArm) vrHudArm->flags[INVISIBLE] = true;
        // Only suppress a conventional HUD item if its tracked replacement
        // has a valid controller pose and a drawable model this frame.
        if (vrHudWeapon && vrHudWeapon->sprite > 0 &&
            g_xrControllerHands[1].gripPoseValid &&
            clientnum >= 0 && clientnum < MAXPLAYERS &&
            stats[clientnum] && stats[clientnum]->weapon)
            vrHudWeapon->flags[INVISIBLE] = true;
        if (vrHudShield &&
            (vrHudShield->sprite > 0 || vrTrackedSpellbook) &&
            g_xrControllerHands[0].gripPoseValid &&
            clientnum >= 0 && clientnum < MAXPLAYERS &&
            stats[clientnum] && stats[clientnum]->shield &&
            (!vrHudShieldWasInvisible || vrTrackedSpellbook))
        {
            vrHudShield->flags[INVISIBLE] = true;
            vrHudShield->flags[INVISIBLE_DITHER] = false;
        }
        // STEP 602: Hide the flat-HUD bolt in the right eye as well.
        if (vrHudBolt && (vrTrackCrossbowBolt || vrBowDisplaySelected()))
            vrHudBolt->flags[INVISIBLE] = true;
        if (vrHudBookExtra) vrHudBookExtra->flags[INVISIBLE] = true;
        if (vrHudBookExtra2) vrHudBookExtra2->flags[INVISIBLE] = true;
        if (vrHudFlailChain) vrHudFlailChain->flags[INVISIBLE] = true;
        // Restore original visibility before drawing the next part of VR.
        if (vrHudMagicLeft) vrHudMagicLeft->flags[INVISIBLE] = true;
        if (vrHudMagicRight) vrHudMagicRight->flags[INVISIBLE] = true;
        drawEntities3D(&vrRightCamera, REALCOLORS);
        if (vrHudMagicLeft) vrHudMagicLeft->flags[INVISIBLE] = vrMagicLeftWasInvisible;
        if (vrHudMagicRight) vrHudMagicRight->flags[INVISIBLE] = vrMagicRightWasInvisible;
        if (vrHudBookExtra) vrHudBookExtra->flags[INVISIBLE] = vrBookExtraWasInvisible;
        if (vrHudBookExtra2) vrHudBookExtra2->flags[INVISIBLE] = vrBookExtra2WasInvisible;
        if (vrHudFlailChain) vrHudFlailChain->flags[INVISIBLE] = vrHudFlailChainWasInvisible;
        if (vrHudBolt) vrHudBolt->flags[INVISIBLE] = vrHudBoltWasInvisible;
        if (vrHudArm) vrHudArm->flags[INVISIBLE] = vrHudArmWasInvisible;
        if (vrHudWeapon) vrHudWeapon->flags[INVISIBLE] = vrHudWeaponWasInvisible;
        if (vrHudShield)
        {
            vrHudShield->flags[INVISIBLE] = vrHudShieldWasInvisible;
            vrHudShield->flags[INVISIBLE_DITHER] = vrHudShieldWasDithered;
        }
        vrRenderTrackedBaronyHands(&vrRightCamera);
        vrDrawSpatialHud(&vrRightCamera);
        g_vrLoadedCrossbowBoltForEye = false;
        // STEP 638EA: Same in-world selected reticles for the right eye.
        vrDrawControllerAimReticle(&vrRightCamera);
        // STEP 638ED: Stereo world guides only; GUI keeps block/charge cues.
        glEndCamera(&vrRightCamera, false, map);
    }
    // // Right-eye dungeon rendering will use the same functions as the left eye.
    // glBeginCamera(camera, false, map);
    // glDrawWorld(camera, REALCOLORS);
    // drawEntities3D(camera, REALCOLORS);
    // glEndCamera(camera, false, map);
    static bool rightEyeCameraReadyLogged = false;
    if (camera != nullptr && !rightEyeCameraReadyLogged)
    {
        SDL_Log("OpenXR: Right-eye dungeon camera prepared: %d x %d",
            vrRightCamera.winw, vrRightCamera.winh);
        rightEyeCameraReadyLogged = true;
    }
    // The right-eye swapchain texture is currently attached to our framebuffer.
    // Restore the original HUD flame visibility before desktop rendering.
    for (const auto& previous : vrOriginalHudFlames)
        previous.first->flags[INVISIBLE] = previous.second;
    // Restore normal desktop viewport behavior.
    vrCameraViewportEnabled = false;
    // Restore normal desktop camera behavior after VR rendering.
    vrCameraTrackingEnabled = false;
    vrEyeProjectionEnabled = false;
    // Restore Barony's original framebuffer.
    g_glBindFramebuffer(
        GL_FRAMEBUFFER,
        static_cast<GLuint>(previousFramebuffer)
    );
    // Restore Barony's original OpenGL clear color.
    glClearColor(
        previousClearColor[0],
        previousClearColor[1],
        previousClearColor[2],
        previousClearColor[3]
    );
    // Restore Barony's original scissor-test setting.
    if (previousScissorEnabled == GL_TRUE)
    {
        glEnable(GL_SCISSOR_TEST);
    }
    else
    {
        glDisable(GL_SCISSOR_TEST);
    }
    // Restore Barony's original OpenGL blending state.
    if (previousBlendEnabled == GL_TRUE)
    {
        glEnable(GL_BLEND);
    }
    else
    {
        glDisable(GL_BLEND);
    }
    // Restore Barony's original color-write settings.
    glColorMask(
        previousColorMask[0],
        previousColorMask[1],
        previousColorMask[2],
        previousColorMask[3]
    );
    // Restore Barony's original viewport.
    glViewport(
        previousViewport[0],
        previousViewport[1],
        previousViewport[2],
        previousViewport[3]
    );

    // Release the acquired image back to OpenXR.
    XrSwapchainImageReleaseInfo releaseInfo{};
    releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;

    result = xrReleaseSwapchainImage(
        g_xrSwapchain,
        &releaseInfo
    );

    if (XR_FAILED(result))
    {
        std::printf(
            "OpenXR: Failed to release stereo image (%d)\n",
            static_cast<int>(result)
        );
    }
    // Mark the frame ready only if both eye framebuffers were complete
    // and OpenXR successfully released the image.
    if (XR_SUCCEEDED(result) &&
        framebufferStatus == GL_FRAMEBUFFER_COMPLETE &&
        rightFramebufferStatus == GL_FRAMEBUFFER_COMPLETE)
    {
        g_xrStereoFrameReady = true;
    }
}
// Forward declaration of Barony's camera type.
struct view_t;
// Entry point for rendering Barony's dungeon in stereo VR.
// The rendering implementation will be added in later steps.
void vrRenderDungeonStereo(view_t* camera)
{
    // Verify that Barony passes a valid camera.
    if (camera == nullptr)
    {
        SDL_Log("VR Dungeon: Camera pointer is NULL!");
        return;
    }
    // Do not render stereo views without valid headset tracking.
    if (!g_xrEyeViewsValid)
    {
        return;
    }
    // Log only once to avoid flooding the console.
    static bool cameraConfirmed = false;

    if (!cameraConfirmed)
    {
        SDL_Log("VR Dungeon: Camera pointer received successfully!");
        cameraConfirmed = true;
    }
}