#ifndef INCLUDED_VRMENU_H
#define INCLUDED_VRMENU_H

// In-headset options menu (PC VR). Drawn with Dear ImGui into an off-screen texture that the
// VR layer shows as a panel in front of the player; the controller is the mouse.

#include <GL/glew.h>
#include <string>

// Every option the menu can change. Saved to Config/Supermodel.ini when changed.
struct VRSettings
{
  int   gunHand     = 1;      // the hand that holds the gun: 1 = right, 2 = left
  int   vignette    = 2;      // comfort vignette during fast camera turns: 0 off .. 3 strong
  bool  cinema      = false;  // play on a big screen in a dark room
  int   cinemaSize  = 1;      // 0 small, 1 medium, 2 large
  int   cinemaDepth = 2;      // 3D depth in cinema mode: 0 off, 1 shallow, 2 full
  int   cinemaBezel = 1;      // cinema screen in a frame whose inner walls reflect the picture
  int   cinemaGlow  = 1;      // light from the picture around the cinema screen: 0 off, 1 subtle, 2 strong
  int   hudLock     = 2;      // 2D layer: 0 room, 1 head, 2 head during play
  bool  hideP2      = true;   // hide player 2's PRESS START BUTTON during play
  bool  staticFill  = true;   // hit static over the whole view
  int   gunModel    = 1;      // gun model in your hand: 0 off, 1 the game's gun, 2 your own (Assets/VRGunModel.obj),
                              // 3 the arcade gun (Assets/VRGameGun.obj), 4 the game's pistol
  int   gunColour   = 0;      // the arcade gun's colour: 0 blue, 1 red
  bool  gunColours  = false;  // an arcade gun file is there (set by the program, not saved)
  bool  gunPistol   = false;  // the game has a second gun in its ROM, a pistol (VRGunModel = 4;
                              // "Game" is then called "Rifle"); set by the program, not saved
  int   recoil      = 1;      // controller vibration while firing: 0 off, 1 light, 2 strong
  float aimNudgeX   = 0.0f;   // aim fine-tune, degrees (+ = right)
  float aimNudgeY   = 0.0f;   // aim fine-tune, degrees (+ = up)
  float renderScale = 1.0f;   // eye resolution, applied on the next start (0.5 - 2.0)
  int   eyeRecW = 0, eyeRecH = 0;   // the runtime's recommended eye size (set by the program, not saved)
  int   eyeMaxW = 0, eyeMaxH = 0;   // the runtime's largest eye size (set by the program, not saved)
  int   eyeCapW = 0;                // VRMaxEyeWidth from the ini, 0 = none (set by the program, not saved)
  int   hudFilter   = 1;      // 2D layer scaling: 0 soft, 1 sharp, 2 smooth edges
};

struct VRMenuResult
{
  bool changed  = false;   // a setting changed (save it)
  bool resume   = false;
  bool recenter = false;
  bool quit     = false;
};

class VRMenu
{
public:
  static constexpr int kSupersample = 2;   // menu texture pixels per layout pixel
  ~VRMenu();
  // GL context must be current. assetDir: folder with the optional menu artwork
  // (VRMenuFrame.tga: a frame picture whose inner rectangle holds the options) and font
  // (VRMenuFont.ttf). Without them the menu is a plain dark panel with the default font.
  bool   Init(const std::string &assetDir);
  int    Width() const { return m_w; }
  int    Height() const { return m_h; }
  void   Shutdown();
  // Draws the menu into Texture(). pointer: position on the panel (0..1, y down), valid = the
  // controller points at it; press = trigger held.
  VRMenuResult Render(VRSettings &s, float pointerU, float pointerV, bool valid, bool press, double dt);
  GLuint Texture() const { return m_tex; }

private:
  void *m_ctx = nullptr;         // ImGuiContext
  GLuint m_tex = 0, m_fbo = 0;
  GLuint m_frameTex = 0;         // artwork (0 = plain panel)
  int    m_w = 960, m_h = 720;   // menu image size
  float  m_inner[4] = {0, 0, 0, 0};   // where the options go (x0, y0, x1, y1, menu pixels)
  bool m_ready = false;
};

#endif
