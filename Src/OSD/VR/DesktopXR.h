#ifndef INCLUDED_DESKTOPXR_H
#define INCLUDED_DESKTOPXR_H

// OpenXR layer for desktop Supermodel (PC VR: Virtual Desktop / Quest Link / SteamVR).
//
// The OpenXR loader (openxr_loader.dll / libopenxr_loader.so.1) is loaded at run time, so
// the emulator still starts without it when VR is not requested.
//
// Frame model, every headset frame:
//   PollEvents() -> BeginFrame() -> [run 0..2 emulator frames, or New3D::RedrawVR()]
//   -> DrawHud() -> DrawEyesIfMissing() -> EndFrame()
// New3D draws its scene once per eye through IVRFrameTarget (BeginEye/EndEye).
//
// Layers: a projection layer (3D world + 2D background at infinity) and a quad layer with
// the 2D top layer (score, text, the game's own crosshair) placed exactly over the game's
// screen frustum at hudDistance metres.

#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>

#include <GL/glew.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Graphics/New3D/VRHooks.h"

struct DesktopXRInput
{
  // Controller aim projected onto the game screen plane (0..1 = the screen, y down; values
  // outside 0..1 point beyond the original screen). Index 0 = right hand.
  bool  aimValid[2] = {false, false};
  bool  onScreen[2] = {false, false};
  float aimX[2]     = {0.5f, 0.5f};
  float aimY[2]     = {0.5f, 0.5f};

  bool trigger[2]    = {false, false};
  bool grip[2]       = {false, false};
  bool thumbClick[2] = {false, false};
  bool buttonA = false, buttonB = false, buttonX = false, buttonY = false;
  bool menu = false;

  // Controller rays (local space, metres) and where they hit the options menu (0..1, y down).
  bool       rayValid[2] = {false, false};
  XrVector3f rayOrigin[2] = {{0, 0, 0}, {0, 0, 0}};
  XrVector3f rayDir[2] = {{0, 0, -1}, {0, 0, -1}};
  bool  menuHit[2] = {false, false};
  bool  menuGrab[2] = {false, false};   // this hand is carrying the menu (grip held)
  float menuU[2] = {0, 0}, menuV[2] = {0, 0}, menuDist[2] = {0, 0};
};

class DesktopXR final : public New3D::IVRFrameTarget
{
public:
  struct Config
  {
    float worldUnitsPerMeter = 1.0f;  // Model 3 world units per real metre (stereo scale)
    bool  positionalTracking = true;  // false = rotation + IPD only
    float hudDistance        = 20.0f; // metres from the origin to the 2D/HUD plane (far, so it
                                      // matches the depth of the 3D world: no double vision)
    float resolutionScale    = 1.0f;  // multiplier on the runtime's recommended eye size
    int   maxEyeWidth        = 0;     // optional cap on pixels per eye (0 = none: the resolution scale decides)
    float refreshHz          = 1000.0f; // headset refresh: the highest rate offered up to this, 0 = runtime default
    bool  threadedWait       = true;   // xrWaitFrame on its own thread (emulator never waits)
    bool  hudInEye           = true;   // draw the 2D layers into the eye images (exact registration
                                       // with the 3D) instead of a separate compositor quad layer
    int   hudLock            = 2;      // 2D layer (score, timer, text): 0 = fixed in the room,
                                       // 1 = follows the head, 2 = follows the head while the
                                       // game shows 3D (play, cutscenes), fixed for 2D menus
    bool  hideP2Prompt       = true;   // leave out player 2's "PRESS START BUTTON" during play
    bool  staticFill         = true;   // extend the game's hit "static" and full-screen tints over the whole view
    bool  backdropFill       = true;   // continue the game's background (sky) past its 4:3 screen
    int   vignette           = 2;      // comfort vignette during fast camera turns, 0 off .. 3 strong
    int   hudFilter          = 1;      // 2D layer scaling: 0 soft, 1 sharp, 2 smooth edges
    float aimNudgeX          = 0.f;    // aim fine-tune, degrees
    float aimNudgeY          = 0.f;
    bool  cinema             = false;  // cinema mode: the flat game on a big screen
    int   cinemaSize         = 1;      // 0 small, 1 medium, 2 large
    int   cinemaDepth        = 2;      // 3D in cinema mode: 0 off (flat), 1 shallow, 2 full
    int   cinemaBezel        = 1;      // cinema screen in a recessed frame whose inner walls reflect the picture: 0 off, 1 on
    int   cinemaGlow         = 1;      // light from the picture spilling around the cinema screen: 0 off, 1 subtle, 2 strong
  };

  ~DesktopXR();

  // GL context must be current. Returns false (with LastError()) if VR is unavailable.
  bool Init(const Config& config);
  void Shutdown();
  const std::string& LastError() const { return m_error; }
  Config& Settings() { return m_config; }   // options the menu can change at run time

  // Options menu: texture from VRMenu, shown as a panel in front of the head while open.
  void SetMenu(GLuint tex, bool open, float aspect = 0.75f);   // aspect = height / width
  bool MenuOpen() const { return m_menuOpen; }
  // Controller vibration on one hand (0 = right), amplitude 0..1.
  void Haptic(int hand, float amplitude, float seconds);
  std::string RuntimeName() const { return m_runtimeName; }

  bool PollEvents();          // false = the runtime asked us to quit
  bool IsSessionRunning() const { return m_sessionRunning; }
  bool InPlay() const { return m_inPlay; }        // gameplay on screen (full-height 3D scene)
  bool IsFocused() const { return m_focused; }   // the app is in front and gets input (headset in use)

  // xrWaitFrame + xrBeginFrame + locate views/controllers. True if eyes should be drawn.
  // EndFrame() must always follow.
  bool   BeginFrame();
  void   EndFrame();
  double FrameDeltaSeconds() const { return m_frameDelta; }
  double DisplayPeriodSeconds() const { return m_displayPeriod; }
  // Threaded wait: start waiting for the next headset frame in the background, and poll it
  // (true = the frame is ready and BeginFrame() will not block). Without the thread: always true.
  void RequestWait();
  bool WaitForFrame(int maxMicroseconds);
  // Wakes a WaitForFrame() early (e.g. the emulator finished a frame); thread-safe.
  void NotifyEvent();
  int64_t PredictedDisplayTime() const { return (int64_t)m_frameState.predictedDisplayTime; }

  void SetGameFrustum(float left, float right, float top, float bottom);
  void SetBackdropTexture(GLuint tex) { m_backdropTex = tex; }
  // The game's background drawn past its screen edges (0 = none): the texture and the game-screen
  // pixel rectangle it covers (x0, y0, x1, y1; the screen itself is 0..496 x 0..384).
  void SetBackdropWide(GLuint tex, float x0, float y0, float x1, float y1)
  { m_wideTex = tex; m_wideRect[0] = x0; m_wideRect[1] = y0; m_wideRect[2] = x1; m_wideRect[3] = y1; }
  void SuppressEyes(bool suppress) { m_suppressEyes = suppress; }
  void DrawEyesIfMissing();

  // Draws the 2D top layer (496x384 RGBA, rows top-first; 0 = none) plus optional pointer
  // dots for each aiming hand into the HUD quad.
  // keepFirstRow/keepEndRow (0..384): outside these rows black pixels of the 2D layer are made
  // transparent - removes the game's letterbox bars in cutscenes but keeps text in them.
  // subTex/subUv: small 3D viewports rendered flat (menus), shown on the panel under the 2D layer.
  void DrawHud(GLuint topLayerTex, bool pointers, int keepFirstRow = 0, int keepEndRow = 384,
               GLuint subTex = 0, const float* subUv = nullptr);

  // Re-centre: the current head position/yaw becomes the origin facing the game screen.
  void Recenter();

  const DesktopXRInput& Input() const { return m_input; }

  // New3D::IVRFrameTarget
  int   EyeCount() const override { return 2; }
  bool  BeginEye(int eye, New3D::VREye& outEye) override;
  void  EndEye(int eye) override;
  float CullTangent() const override { return 100.f; }
  void  SetScreen(const New3D::VRScreenInfo& info) override;
  int   FlatViewCount() const override;
  bool  BeginFlat(int view, New3D::VREye& outTarget) override;
  void  EndFlat(int view) override;
  bool  WantsFlat() const override { return m_config.cinema; }

  // 2D top layer texture (496x384, rows top-first) and HUD options, for the in-eye HUD.
  void SetHudSource(GLuint topLayerTex, bool pointers, bool hideLetterbox)
  {
    m_hudTopTex = topLayerTex;
    m_hudPointers = pointers;
    m_hideLetterbox = hideLetterbox;
  }

  // Gun model drawn in the hands: 0 off, 1 the game's gun, 2 a custom OBJ (Assets/VRGunModel.obj),
  // 3 the arcade gun OBJ (Assets/VRGameGun.obj).
  // The game's gun is drawn inside the 3D scene; in cinema mode, and for the custom model, the
  // VR layer draws it itself, in the room.
  void SetGunMode(int mode) { m_gunMode = mode; }
  bool LoadCustomGun(const std::string& objPath, int slot = 1);   // false = no file / nothing usable in it
  // An arcade cabinet gun as a model file (Assets/VRGameGun.obj): gun mode 3 ("Arcade"),
  // drawn in the colour set below.
  bool LoadGameGun(const std::string& objPath) { return LoadCustomGun(objPath, 2); }
  void SetGunColour(int colour) { m_gunColour = colour; }   // arcade gun: 0 blue, 1 red
  // The runtime's recommended and largest eye image sizes (after Init).
  void EyeSizes(int& recW, int& recH, int& maxW, int& maxH) const { recW = m_eyeRecW; recH = m_eyeRecH; maxW = m_eyeMaxW; maxH = m_eyeMaxH; }
  bool CustomGunLoaded() const { return !m_customGun.empty() || m_gunCount[1] > 0; }

  int EyeWidth()  const { return m_eyes[0].width; }
  int EyeHeight() const { return m_eyes[0].height; }

private:
  struct Swapchain
  {
    XrSwapchain handle = XR_NULL_HANDLE;
    int width = 0, height = 0;
    std::vector<uint32_t> images;     // GL texture names
    std::vector<GLuint> fbos;
    uint32_t acquired = UINT32_MAX;
    bool releasedOnce = false;
  };

  bool Fail(const std::string& what);
  bool Check(XrResult r, const char* what);
  bool LoadLoader();
  bool CreateInstance();
  bool CreateSession();
  bool CreateSwapchain(Swapchain& sc, int w, int h);
  void DestroySwapchain(Swapchain& sc);
  bool CreateActions();
  bool CreateShaders();
  void SyncInput();
  bool AcquireImage(Swapchain& sc);
  void ReleaseImage(Swapchain& sc);
  void ComputeEye(int eye, New3D::VREye& out) const;
  void DrawBackdrop(const New3D::VREye& eye);
  void DrawHudInEye(const New3D::VREye& eye);
  void DrawStaticFill(const New3D::VREye& eye, const float panelMvp[16]);
  void DrawTintFill(const New3D::VREye& eye, const float panelMvp[16]);
  void DrawPointerRings(const float mvp[16], float L, float R, float T, float B);
  void CinemaRect(float& L, float& R, float& T, float& B) const;
  bool PointerHand(int hand) const;
  void DrawMenuInEye(const New3D::VREye& eye);
  void DrawVignette(const New3D::VREye& eye);
  void DrawCinemaInEye(const New3D::VREye& eye);
  void DrawHandGuns(const New3D::VREye& eye);
  bool HandMatrix(int hand, float m[16]) const;   // controller aim pose, world units
  int  EffectiveGunMode() const;
  void UpdateComfort();
  float CinemaScale() const;
  void EyeProjection(const New3D::VREye& eye, float proj[16]) const;
  void HandleSessionState(XrSessionState state);
  void RecreateLocalSpace(const XrPosef& origin);
  void StartWaitThread();
  void StopWaitThread();

  Config m_config;
  std::string m_error;
  std::string m_runtimeName;
  void* m_loaderLib = nullptr;

  XrInstance m_instance = XR_NULL_HANDLE;
  XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
  XrSession  m_session  = XR_NULL_HANDLE;
  XrSpace    m_stageSpace = XR_NULL_HANDLE;   // raw LOCAL space
  XrSpace    m_localSpace = XR_NULL_HANDLE;   // LOCAL with our recentre offset
  XrSpace    m_viewSpace  = XR_NULL_HANDLE;
  std::atomic<bool> m_sessionRunning{false};
  std::atomic<bool> m_focused{false};
  bool m_exitRequested  = false;
  bool m_recenterPending = true;
  bool m_hasRefreshRate = false;

  int64_t m_colorFormat = 0;

  Swapchain m_eyes[2];
  Swapchain m_hud;
  XrView    m_views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  XrPosef   m_headPose{{0, 0, 0, 1}, {0, 0, 0}};
  bool      m_viewsValid = false;
  bool      m_eyeDrawn[2] = {false, false};
  bool      m_hudDrawn = false;
  bool      m_suppressEyes = false;

  XrFrameState m_frameState{XR_TYPE_FRAME_STATE};

  std::thread             m_waitThread;
  std::mutex              m_waitMutex;
  std::condition_variable m_waitCv;
  bool                    m_waitRequested = false, m_waitDone = false, m_waitQuit = false;
  bool                    m_extEvent = false;
  XrFrameState            m_waitState{XR_TYPE_FRAME_STATE};
  XrResult                m_waitResult = XR_SUCCESS;
  bool   m_frameBegun = false;
  double m_displayPeriod = 1.0 / 90.0;
  double m_frameDelta = 1.0 / 90.0;
  XrTime m_lastDisplayTime = 0;

  float m_gameL = -0.6f, m_gameR = 0.6f, m_gameT = 0.465f, m_gameB = -0.465f;

  XrActionSet m_actionSet = XR_NULL_HANDLE;
  XrAction m_aimPose = XR_NULL_HANDLE, m_trigger = XR_NULL_HANDLE, m_grip = XR_NULL_HANDLE;
  XrAction m_a = XR_NULL_HANDLE, m_b = XR_NULL_HANDLE, m_x = XR_NULL_HANDLE, m_y = XR_NULL_HANDLE;
  XrAction m_menu = XR_NULL_HANDLE, m_thumbClick = XR_NULL_HANDLE, m_haptic = XR_NULL_HANDLE;
  XrPath   m_hands[2] = {XR_NULL_PATH, XR_NULL_PATH};  // 0 = right, 1 = left
  XrSpace  m_aimSpaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  DesktopXRInput m_input;

  GLuint m_backdropTex = 0;
  GLuint m_wideTex = 0;
  float  m_wideRect[4] = { 0, 0, 496, 384 };
  GLuint m_hudTopTex = 0;
  bool   m_hudPointers = false;
  bool   m_hideLetterbox = true;
  New3D::VRScreenInfo m_screen;
  bool   m_hudOnHead = false;   // VRHudLock 2: current choice, switched after a short steady period
  int    m_hudSwitchFrames = 0;
  bool   m_scene3D = false;     // a 3D scene in the main view this frame (backdrop fill)
  bool   m_inPlay = false;      // gameplay on screen (for VRHideP2Prompt), with the same kind of delay
  int    m_playSwitchFrames = 0;
  New3D::VREye m_curEye[2];
  GLuint m_quadProgram = 0;     // textured quad (backdrop + HUD)
  GLint  m_uMvp = -1, m_uTex = -1, m_uRect = -1, m_uMode = -1, m_uKeep = -1, m_uSubUv = -1, m_uGlow = -1;
  GLuint m_dotProgram = 0;      // pointer dots on the HUD
  GLint  m_uDotCenter = -1, m_uDotRadius = -1, m_uDotColor = -1, m_uDotAspect = -1;
  GLuint m_vao = 0;
  GLuint m_staticDetectProgram = 0, m_staticProgram = 0, m_staticTex = 0, m_staticFbo = 0;
  GLint  m_uStaticMvp = -1, m_uStaticRect = -1, m_uStaticGame = -1, m_uStaticAmount = -1, m_uStaticSeed = -1;
  XrTime m_staticMeasuredFrame = 0;
  GLuint m_tintDetectProgram = 0, m_tintProgram = 0, m_tintTex = 0, m_tintFbo = 0;
  GLint  m_uTintMvp = -1, m_uTintRect = -1, m_uTintGame = -1, m_uTintTex = -1;
  XrTime m_tintMeasuredFrame = 0;
  GLint  m_uUvXform = -1;
  GLint  m_uFilter = -1;
  GLuint m_vignetteProgram = 0;
  GLint  m_uVigTan = -1, m_uVigSize = -1, m_uVigRadius = -1, m_uVigSoft = -1;
  float  m_cameraTurn = 0.f;   // game camera turn per game frame (degrees)
  float  m_vignette = 0.f;     // current vignette amount 0..1
  static constexpr int   kCinemaW = 1488, kCinemaH = 1152;
  static constexpr float kCinemaDistance = 4.0f;   // metres
  static constexpr float kCinemaTanHalfHeight = 0.30f;   // cinema screen: half height (tangent, before CinemaScale)
  GLuint m_cinemaTex[2] = {0, 0}, m_cinemaFbo[2] = {0, 0};   // one per eye in 3D cinema
  int    m_gunMode = 0;
  int    m_eyeRecW = 0, m_eyeRecH = 0, m_eyeMaxW = 0, m_eyeMaxH = 0;
  std::vector<float> m_customGun;              // pending upload: pos, normal, colour per vertex
  std::vector<float> m_gameGunFile;            // the same for the game's gun from a file (slot 2)
  bool   m_gameGunFileLoaded = false;
  int    m_gunColour = 0;                      // game gun from file: 0 blue, 1 red
  GLint  m_uGunTint = -1;
  GLint  m_uGunTex = -1, m_uGunTextured = -1, m_uGunCull = -1;
  GLuint m_gunProgram = 0, m_gunVao[3] = {0, 0, 0}, m_gunVbo[3] = {0, 0, 0};   // 0 = game gun (ROM), 1 = custom, 2 = game gun (file)
  GLsizei m_gunCount[3] = {0, 0, 0};
  unsigned m_gunVersion = ~0u;
  struct GunPart { int first = 0, count = 0; GLuint tex = 0; bool alphaTest = false; };
  std::vector<GunPart> m_gunParts;             // game gun (ROM): one draw per mesh, textured ones with their texture
  GLint  m_uGunMvp = -1, m_uGunModel = -1, m_uGunEye = -1, m_uGunScreen = -1, m_uGunUseScreen = -1, m_uGunSpec = -1;
  float  m_customSpec = 0.5f, m_customShine = 90.f;   // Ks / Ns of the custom model's material
  float  m_gameFileSpec = 0.35f, m_gameFileShine = 40.f;   // ... and of the game gun from a file
  GLuint m_gunDepth = 0;
  bool   m_gunDepthValid = false;   // m_gunDepth holds this eye's guns (the menu is depth-tested against it)
  int    m_gunDepthW = 0, m_gunDepthH = 0;
  bool   m_cinemaValid[2] = {false, false};
  float  m_ipd = 0.064f;   // eye distance from the headset (metres)
  GLuint m_lineProgram = 0;
  GLuint m_bezelProgram = 0;
  GLint  m_uBzMvp = -1, m_uBzPos = -1, m_uBzSu = -1, m_uBzTex = -1, m_uBzKind = -1, m_uBzAspect = -1, m_uBzLight = -1, m_uBzShade = -1;
  void   DrawCinemaBezel(const float* mvp, GLuint tex);
  GLint  m_uLineMvp = -1, m_uLineA = -1, m_uLineB = -1, m_uLineColor = -1;

  static constexpr float kMenuDistance = 1.4f;   // metres
  float      kMenuWidth = 1.30f, kMenuHeight = 0.975f; // panel size in metres (from the menu's aspect)
  bool       m_menuOpen = false;
  GLuint     m_menuTex = 0;
  float      m_menuYaw = 0.f;
  XrVector3f m_menuCenter{0.f, 0.f, -kMenuDistance};
  int        m_menuGrabHand = -1;     // hand holding the menu with its grip button (-1 = none)
  float      m_menuGrabDist = 0.f;    // distance along that hand's ray to the grabbed point
  float      m_menuGrabX = 0.f, m_menuGrabY = 0.f;   // grabbed point relative to the panel centre (metres)
  bool       m_prevGrip[2] = {false, false};
  XrPosef    m_handPose[2] = {};      // controller aim poses this frame (local space)
  bool       m_handPoseValid[2] = {false, false};
  int        m_gunHands = 0;          // bit per hand (0 right, 1 left) that holds the gun model
  int        m_pointerHand = -1;      // the hand whose aim gets the pointer ring (-1: both)
public:
  void SetGunHands(int mask) { m_gunHands = mask; }
  void SetPointerHand(int hand) { m_pointerHand = hand; }
  int  HandPoses(New3D::VRHandPose* out, int max) const override;
private:
};

#endif
