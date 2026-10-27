#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <unknwn.h>
#endif

#include "DesktopXR.h"

#ifdef _WIN32
  #define XR_USE_PLATFORM_WIN32
#else
  #include <dlfcn.h>
  #include <GL/glxew.h>
  #define XR_USE_PLATFORM_XLIB
#endif
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>

// Supermodel's logger (OSD/Logger.h is not included: its INT64 typedef clashes with X11's).
extern void InfoLog(const char *fmt, ...);
#define ErrorLog(fmt, ...) InfoLog("ERROR: " fmt, ##__VA_ARGS__)

// ---------------------------------------------------------------------------------------------
// Dynamically loaded OpenXR entry points (XR_NO_PROTOTYPES is set in the header).
// ---------------------------------------------------------------------------------------------

#define XR_GLOBAL_FUNCS(X) \
  X(xrEnumerateInstanceExtensionProperties) \
  X(xrCreateInstance)

#define XR_INSTANCE_FUNCS(X) \
  X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrGetSystem) X(xrResultToString) \
  X(xrPollEvent) X(xrCreateSession) X(xrDestroySession) X(xrBeginSession) X(xrEndSession) \
  X(xrRequestExitSession) X(xrCreateReferenceSpace) X(xrDestroySpace) X(xrLocateSpace) \
  X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain) \
  X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) \
  X(xrReleaseSwapchainImage) X(xrEnumerateViewConfigurationViews) X(xrWaitFrame) \
  X(xrBeginFrame) X(xrEndFrame) X(xrLocateViews) X(xrStringToPath) X(xrCreateActionSet) \
  X(xrDestroyActionSet) X(xrCreateAction) X(xrSuggestInteractionProfileBindings) \
  X(xrCreateActionSpace) X(xrAttachSessionActionSets) X(xrSyncActions) \
  X(xrGetActionStateBoolean) X(xrGetActionStateFloat) X(xrGetOpenGLGraphicsRequirementsKHR) \
  X(xrApplyHapticFeedback)

namespace {

PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
#define XR_DECLARE(name) PFN_##name name = nullptr;
XR_GLOBAL_FUNCS(XR_DECLARE)
XR_INSTANCE_FUNCS(XR_DECLARE)
#undef XR_DECLARE
PFN_xrRequestDisplayRefreshRateFB pfnRequestDisplayRefreshRateFB = nullptr;
PFN_xrEnumerateDisplayRefreshRatesFB pfnEnumerateDisplayRefreshRatesFB = nullptr;
PFN_xrGetDisplayRefreshRateFB pfnGetDisplayRefreshRateFB = nullptr;

void QuatToMat3(const XrQuaternionf& q, float m[9])
{
  const float x = q.x, y = q.y, z = q.z, w = q.w;
  m[0] = 1 - 2 * (y * y + z * z); m[3] = 2 * (x * y - w * z);     m[6] = 2 * (x * z + w * y);
  m[1] = 2 * (x * y + w * z);     m[4] = 1 - 2 * (x * x + z * z); m[7] = 2 * (y * z - w * x);
  m[2] = 2 * (x * z - w * y);     m[5] = 2 * (y * z + w * x);     m[8] = 1 - 2 * (x * x + y * y);
}

XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v)
{
  float m[9];
  QuatToMat3(q, m);
  return {m[0] * v.x + m[3] * v.y + m[6] * v.z,
          m[1] * v.x + m[4] * v.y + m[7] * v.z,
          m[2] * v.x + m[5] * v.y + m[8] * v.z};
}

GLuint CompileShader(GLenum type, const char* src)
{
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024] = {};
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    ErrorLog("VR: shader compile failed: %s", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

GLuint LinkProgram(const char* vsSrc, const char* fsSrc)
{
  GLuint vs = CompileShader(GL_VERTEX_SHADER, vsSrc);
  GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsSrc);
  if (!vs || !fs)
    return 0;
  GLuint p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  glLinkProgram(p);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

// Textured quad spanning uRect (left, right, top, bottom tangents at z = -1), through uMvp.
// Mode 0: opaque backdrop. Mode 1: premultiplied-alpha HUD layer.
const char* kQuadVS = R"(#version 330 core
uniform mat4 uMvp;
uniform vec4 uRect;
uniform vec4 uUvXform;   // vUv * xy + zw (the backdrop drawn beyond the screen); (1, 1, 0, 0) otherwise
out vec2 vUv;
void main() {
  int id = gl_VertexID;
  bool left = (id == 0 || id == 2);
  bool bottom = (id < 2);
  vUv = vec2(left ? 0.0 : 1.0, bottom ? 1.0 : 0.0) * uUvXform.xy + uUvXform.zw;
  gl_Position = uMvp * vec4(left ? uRect.x : uRect.y, bottom ? uRect.w : uRect.z, -1.0, 1.0);
})";

const char* kQuadFS = R"(#version 330 core
uniform sampler2D uTex;
uniform int uMode;
uniform vec2 uKeep;   // HUD: v range of the 3D view; black outside it becomes see-through
uniform vec4 uSubUv;  // mode 2: game area inside the small-viewport texture (u0, v0, u1, v1)
uniform int uFilter;  // HUD: 0 soft, 1 sharp, 2 smooth edges
uniform vec2 uGlow;   // mode 5: strength, screen width / height
in vec2 vUv;
out vec4 fragColor;
// One texel of the HUD layer, premultiplied; letterbox black becomes fully transparent.
vec4 hudTexel(ivec2 t, ivec2 size) {
  t = clamp(t, ivec2(0), size - 1);
  vec4 c = texelFetch(uTex, t, 0);
  float row = (float(t.y) + 0.5) / float(size.y);
  if ((row < uKeep.x || row > uKeep.y) && max(c.r, max(c.g, c.b)) < 0.06) return vec4(0.0);
  return vec4(c.rgb * c.a, c.a);
}
bool same(vec4 x, vec4 y) { vec4 d = abs(x - y); return max(max(d.r, d.g), max(d.b, d.a)) < 0.08; }
// Scale2x: one texel of the 2x image, rounding off diagonal staircases
vec4 scale2x(ivec2 t2, ivec2 size) {
  ivec2 t = ivec2(floor(vec2(t2) * 0.5));
  ivec2 q = t2 - t * 2;
  vec4 P = hudTexel(t, size);
  vec4 A = hudTexel(t + ivec2(0, -1), size), D = hudTexel(t + ivec2(0, 1), size);
  vec4 C = hudTexel(t + ivec2(-1, 0), size), B = hudTexel(t + ivec2(1, 0), size);
  if (q.x == 0 && q.y == 0) return (same(C, A) && !same(C, D) && !same(A, B)) ? A : P;
  if (q.x == 1 && q.y == 0) return (same(A, B) && !same(A, C) && !same(B, D)) ? B : P;
  if (q.x == 0 && q.y == 1) return (same(D, C) && !same(D, B) && !same(C, A)) ? C : P;
  return (same(B, D) && !same(B, A) && !same(D, C)) ? D : P;
}
void main() {
  if (uMode == 0) { fragColor = vec4(texture(uTex, vUv).rgb, 1.0); return; }
  if (uMode == 3) { fragColor = vec4(texture(uTex, vec2(vUv.x, 1.0 - vUv.y)).rgb, 1.0); return; }
  if (uMode == 4) { fragColor = texture(uTex, vec2(vUv.x, 1.0 - vUv.y)); return; }   // premultiplied, flipped
  if (uMode == 5) {
    // Cinema screen glow: the picture's edge colours, heavily blurred (small mip levels),
    // spilling onto the dark surround and fading with the distance from the screen edge.
    // uSubUv = where the screen sits inside this bigger quad (u0, v0, u1, v1).
    vec2 su = (vUv - uSubUv.xy) / (uSubUv.zw - uSubUv.xy);
    vec2 e = clamp(su, 0.0, 1.0);
    vec2 off = (su - e) * vec2(uGlow.y, 1.0);          // in screen heights
    float dist = length(off);
    if (dist <= 0.0) { fragColor = vec4(0.0); return; }
    vec2 uv = vec2(e.x, 1.0 - e.y);
    float lod = log2(float(textureSize(uTex, 0).y)) - 3.0;   // about 8 blocks per screen height
    vec3 near = textureLod(uTex, uv, lod).rgb;
    vec3 wide = textureLod(uTex, uv, lod + 1.5).rgb;
    vec3 c = mix(near, wide, clamp(dist * 6.0, 0.0, 1.0));
    float fall = exp(-dist * 7.5) * (1.0 - smoothstep(0.30, 0.42, dist));
    fragColor = vec4(c * fall * uGlow.x, 0.0);        // added to what's behind
    return;
  }
  if (uMode == 2) {
    // flat-rendered small viewports (GL texture, bottom-up); already alpha-weighted
    vec4 c = texture(uTex, vec2(mix(uSubUv.x, uSubUv.z, vUv.x), mix(uSubUv.w, uSubUv.y, vUv.y)));
    fragColor = vec4(c.rgb, c.a);
    return;
  }
  // Filtering done by hand, after the letterbox keying, so no dark seams remain.
  //   uFilter 0: soft (bilinear)
  //   uFilter 1: sharp - square pixels with anti-aliased edges
  //   uFilter 2: smooth edges - pixel-art upscale (Scale2x) first, then sharp
  ivec2 size = textureSize(uTex, 0);
  float scale = (uFilter == 2) ? 2.0 : 1.0;
  vec2 p = vUv * vec2(size) * scale - 0.5;
  ivec2 i = ivec2(floor(p));
  vec2 f = p - floor(p);
  if (uFilter >= 1) {
    vec2 w = max(fwidth(p), vec2(1e-4));
    f = clamp((f - 0.5) / w + 0.5, 0.0, 1.0);
  }
  vec4 a, b;
  if (uFilter == 2) {
    a = mix(scale2x(i, size), scale2x(i + ivec2(1, 0), size), f.x);
    b = mix(scale2x(i + ivec2(0, 1), size), scale2x(i + ivec2(1, 1), size), f.x);
  } else {
    a = mix(hudTexel(i, size), hudTexel(i + ivec2(1, 0), size), f.x);
    b = mix(hudTexel(i + ivec2(0, 1), size), hudTexel(i + ivec2(1, 1), size), f.x);
  }
  fragColor = mix(a, b, f.y);
})";

// Pointer dot: ring + centre dot around uCenter (NDC), radius in NDC height units.
const char* kDotVS = R"(#version 330 core
uniform vec2 uCenter;
uniform float uRadius;
uniform float uAspect;
out vec2 vLocal;
void main() {
  int id = gl_VertexID;
  vec2 corner = vec2((id == 1 || id == 3) ? 1.0 : -1.0, (id >= 2) ? 1.0 : -1.0);
  vLocal = corner;
  gl_Position = vec4(uCenter + corner * vec2(uRadius / uAspect, uRadius), 0.0, 1.0);
})";

const char* kDotFS = R"(#version 330 core
uniform vec3 uColor;
in vec2 vLocal;
out vec4 fragColor;
void main() {
  float r = length(vLocal);
  float aa = fwidth(r) * 1.5;
  float ring = smoothstep(0.62 - aa, 0.62, r) * (1.0 - smoothstep(0.92, 0.92 + aa, r));
  float dot = 1.0 - smoothstep(0.16, 0.16 + aa, r);
  float a = max(ring, dot) * 0.9;
  fragColor = vec4(uColor * a, a);
})";

} // namespace

// ---------------------------------------------------------------------------------------------
// Setup / teardown
// ---------------------------------------------------------------------------------------------

DesktopXR::~DesktopXR()
{
  Shutdown();
}

bool DesktopXR::Fail(const std::string& what)
{
  m_error = what;
  ErrorLog("VR: %s", what.c_str());
  return false;
}

bool DesktopXR::Check(XrResult r, const char* what)
{
  if (XR_SUCCEEDED(r))
    return true;
  if (r == XR_ERROR_INSTANCE_LOST || r == XR_ERROR_SESSION_LOST)
  {
    // The VR runtime went away (headset disconnected, VR app closed): keep playing in the
    // window instead of spinning on a dead session.
    m_sessionRunning = false;
    m_frameBegun = false;
  }
  char text[XR_MAX_RESULT_STRING_SIZE] = {};
  if (xrResultToString && m_instance != XR_NULL_HANDLE)
    xrResultToString(m_instance, r, text);
  else
    std::snprintf(text, sizeof(text), "%d", (int)r);
  return Fail(std::string(what) + " failed: " + text);
}

bool DesktopXR::LoadLoader()
{
#ifdef _WIN32
  HMODULE lib = LoadLibraryA("openxr_loader.dll");
  if (!lib)
    return Fail("openxr_loader.dll not found next to supermodel.exe");
  m_loaderLib = lib;
  xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(lib, "xrGetInstanceProcAddr"));
#else
  void* lib = dlopen("libopenxr_loader.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!lib)
    lib = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
  if (!lib)
    return Fail("libopenxr_loader.so.1 not found");
  m_loaderLib = lib;
  xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(lib, "xrGetInstanceProcAddr"));
#endif
  if (!xrGetInstanceProcAddr)
    return Fail("OpenXR loader has no xrGetInstanceProcAddr");
#define XR_LOAD_GLOBAL(name) xrGetInstanceProcAddr(XR_NULL_HANDLE, #name, reinterpret_cast<PFN_xrVoidFunction*>(&name));
  XR_GLOBAL_FUNCS(XR_LOAD_GLOBAL)
#undef XR_LOAD_GLOBAL
  if (!xrEnumerateInstanceExtensionProperties || !xrCreateInstance)
    return Fail("OpenXR loader is missing core functions");
  return true;
}

bool DesktopXR::Init(const Config& config)
{
  m_config = config;
  if (!LoadLoader() || !CreateInstance() || !CreateSession() || !CreateActions() || !CreateShaders())
    return false;

  uint32_t viewCount = 0;
  xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
  std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  if (viewCount < 2 ||
      !Check(xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                               viewCount, &viewCount, views.data()), "xrEnumerateViewConfigurationViews"))
    return false;

  m_eyeRecW = (int)views[0].recommendedImageRectWidth;
  m_eyeRecH = (int)views[0].recommendedImageRectHeight;
  m_eyeMaxW = (int)views[0].maxImageRectWidth;
  m_eyeMaxH = (int)views[0].maxImageRectHeight;
  float scale = std::clamp(m_config.resolutionScale, 0.25f, 2.0f);
  if (m_config.maxEyeWidth > 0 && views[0].recommendedImageRectWidth * scale > (float)m_config.maxEyeWidth)
    scale = (float)m_config.maxEyeWidth / (float)views[0].recommendedImageRectWidth;
  for (int eye = 0; eye < 2; eye++) {
    int w = (int)std::lround(views[eye].recommendedImageRectWidth * scale);
    int h = (int)std::lround(views[eye].recommendedImageRectHeight * scale);
    w = std::clamp(w, 64, (int)views[eye].maxImageRectWidth);
    h = std::clamp(h, 64, (int)views[eye].maxImageRectHeight);
    if (!CreateSwapchain(m_eyes[eye], w, h))
      return false;
  }
  // HUD at 3x the Model 3 resolution (496x384), so text stays crisp on the quad.
  if (!CreateSwapchain(m_hud, 496 * 3, 384 * 3))
    return false;

  InfoLog("VR: runtime '%s', eyes %dx%d (scale %.2f of %ux%u), HUD %dx%d", m_runtimeName.c_str(),
          m_eyes[0].width, m_eyes[0].height, scale, views[0].recommendedImageRectWidth,
          views[0].recommendedImageRectHeight, m_hud.width, m_hud.height);
  New3D::SetVRFrameTarget(this);
  return true;
}

bool DesktopXR::CreateInstance()
{
  uint32_t extCount = 0;
  if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr), "xrEnumerateInstanceExtensionProperties"))
    return false;
  std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
  xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
  auto has = [&](const char* name) {
    return std::any_of(exts.begin(), exts.end(), [&](const XrExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
  };

  if (!has(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME))
    return Fail("the active OpenXR runtime does not support OpenGL (XR_KHR_opengl_enable). "
                "Use Virtual Desktop (VDXR), Meta Quest Link or SteamVR as the OpenXR runtime.");
  std::vector<const char*> enabled = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
  m_hasRefreshRate = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
  if (m_hasRefreshRate)
    enabled.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);

  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  std::strncpy(ci.applicationInfo.applicationName, "Supermodel VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
  ci.applicationInfo.applicationVersion = 1;
  std::strncpy(ci.applicationInfo.engineName, "Supermodel", XR_MAX_ENGINE_NAME_SIZE - 1);
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ci.enabledExtensionCount = (uint32_t)enabled.size();
  ci.enabledExtensionNames = enabled.data();
  XrResult r = xrCreateInstance(&ci, &m_instance);
  if (XR_FAILED(r)) {
    char msg[128];
    std::snprintf(msg, sizeof(msg), "xrCreateInstance failed (%d) - is the headset connected and the VR app "
                  "(Virtual Desktop / Quest Link / SteamVR) running?", (int)r);
    return Fail(msg);
  }

#define XR_LOAD_INSTANCE(name) xrGetInstanceProcAddr(m_instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&name));
  XR_INSTANCE_FUNCS(XR_LOAD_INSTANCE)
#undef XR_LOAD_INSTANCE
  if (m_hasRefreshRate) {
    xrGetInstanceProcAddr(m_instance, "xrRequestDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&pfnRequestDisplayRefreshRateFB));
    xrGetInstanceProcAddr(m_instance, "xrEnumerateDisplayRefreshRatesFB", reinterpret_cast<PFN_xrVoidFunction*>(&pfnEnumerateDisplayRefreshRatesFB));
    xrGetInstanceProcAddr(m_instance, "xrGetDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetDisplayRefreshRateFB));
  }
#define XR_CHECK_LOADED(name) if (!name) return Fail("OpenXR runtime is missing " #name);
  XR_INSTANCE_FUNCS(XR_CHECK_LOADED)
#undef XR_CHECK_LOADED

  XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
  if (XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &props)))
    m_runtimeName = props.runtimeName;

  XrSystemGetInfo sys{XR_TYPE_SYSTEM_GET_INFO};
  sys.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  r = xrGetSystem(m_instance, &sys, &m_systemId);
  if (XR_FAILED(r))
    return Fail("no headset found by the OpenXR runtime '" + m_runtimeName + "' - connect the Quest first");
  return true;
}

bool DesktopXR::CreateSession()
{
  XrGraphicsRequirementsOpenGLKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
  if (!Check(xrGetOpenGLGraphicsRequirementsKHR(m_instance, m_systemId, &reqs), "xrGetOpenGLGraphicsRequirementsKHR"))
    return false;

  XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO};
#ifdef _WIN32
  XrGraphicsBindingOpenGLWin32KHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
  binding.hDC = wglGetCurrentDC();
  binding.hGLRC = wglGetCurrentContext();
  if (!binding.hDC || !binding.hGLRC)
    return Fail("no current OpenGL context");
#else
  XrGraphicsBindingOpenGLXlibKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR};
  binding.xDisplay = glXGetCurrentDisplay();
  binding.glxContext = glXGetCurrentContext();
  binding.glxDrawable = glXGetCurrentDrawable();
  if (!binding.xDisplay || !binding.glxContext)
    return Fail("no current GLX context");
  int fbConfigId = 0;
  glXQueryContext(binding.xDisplay, binding.glxContext, GLX_FBCONFIG_ID, &fbConfigId);
  const int attribs[] = {GLX_FBCONFIG_ID, fbConfigId, None};
  int n = 0;
  GLXFBConfig* cfgs = glXChooseFBConfig(binding.xDisplay, DefaultScreen(binding.xDisplay), attribs, &n);
  if (cfgs && n > 0) {
    binding.glxFBConfig = cfgs[0];
    XVisualInfo* vi = glXGetVisualFromFBConfig(binding.xDisplay, cfgs[0]);
    if (vi) {
      binding.visualid = (uint32_t)vi->visualid;
      XFree(vi);
    }
  }
  if (cfgs)
    XFree(cfgs);
#endif
  si.next = &binding;
  si.systemId = m_systemId;
  if (!Check(xrCreateSession(m_instance, &si, &m_session), "xrCreateSession"))
    return false;

  XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rs.poseInReferenceSpace.orientation.w = 1.0f;
  rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  if (!Check(xrCreateReferenceSpace(m_session, &rs, &m_stageSpace), "create LOCAL space") ||
      !Check(xrCreateReferenceSpace(m_session, &rs, &m_localSpace), "create LOCAL space"))
    return false;
  rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  if (!Check(xrCreateReferenceSpace(m_session, &rs, &m_viewSpace), "create VIEW space"))
    return false;

  // Supermodel writes gamma-encoded colours. An sRGB swapchain with GL_FRAMEBUFFER_SRGB off
  // stores them unchanged and the compositor then reads them as sRGB, which is correct.
  uint32_t fmtCount = 0;
  xrEnumerateSwapchainFormats(m_session, 0, &fmtCount, nullptr);
  std::vector<int64_t> formats(fmtCount);
  xrEnumerateSwapchainFormats(m_session, fmtCount, &fmtCount, formats.data());
  for (int64_t f : {(int64_t)GL_SRGB8_ALPHA8, (int64_t)GL_RGBA8}) {
    if (std::find(formats.begin(), formats.end(), f) != formats.end()) {
      m_colorFormat = f;
      break;
    }
  }
  if (!m_colorFormat)
    return Fail("no usable OpenGL swapchain format");
  return true;
}

bool DesktopXR::CreateSwapchain(Swapchain& sc, int w, int h)
{
  XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  ci.format = m_colorFormat;
  ci.sampleCount = 1;
  ci.width = (uint32_t)w;
  ci.height = (uint32_t)h;
  ci.faceCount = 1;
  ci.arraySize = 1;
  ci.mipCount = 1;
  if (!Check(xrCreateSwapchain(m_session, &ci, &sc.handle), "xrCreateSwapchain"))
    return false;
  sc.width = w;
  sc.height = h;

  uint32_t count = 0;
  xrEnumerateSwapchainImages(sc.handle, 0, &count, nullptr);
  std::vector<XrSwapchainImageOpenGLKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
  if (!Check(xrEnumerateSwapchainImages(sc.handle, count, &count,
                                        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())), "xrEnumerateSwapchainImages"))
    return false;

  sc.images.resize(count);
  sc.fbos.resize(count);
  glGenFramebuffers((GLsizei)count, sc.fbos.data());
  for (uint32_t i = 0; i < count; i++) {
    sc.images[i] = images[i].image;
    glBindFramebuffer(GL_FRAMEBUFFER, sc.fbos[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[i].image, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      char msg[64];
      std::snprintf(msg, sizeof(msg), "swapchain framebuffer incomplete (0x%x)", status);
      return Fail(msg);
    }
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return true;
}

void DesktopXR::DestroySwapchain(Swapchain& sc)
{
  if (!sc.fbos.empty())
    glDeleteFramebuffers((GLsizei)sc.fbos.size(), sc.fbos.data());
  if (sc.handle != XR_NULL_HANDLE && xrDestroySwapchain)
    xrDestroySwapchain(sc.handle);
  sc = Swapchain{};
}

bool DesktopXR::CreateActions()
{
  XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(asci.actionSetName, "gameplay");
  std::strcpy(asci.localizedActionSetName, "Gameplay");
  asci.priority = 0;
  if (!Check(xrCreateActionSet(m_instance, &asci, &m_actionSet), "xrCreateActionSet"))
    return false;

  xrStringToPath(m_instance, "/user/hand/right", &m_hands[0]);
  xrStringToPath(m_instance, "/user/hand/left", &m_hands[1]);

  auto make = [&](XrAction& out, XrActionType type, const char* name, const char* label, bool perHand) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = type;
    std::strcpy(ci.actionName, name);
    std::strcpy(ci.localizedActionName, label);
    if (perHand) {
      ci.countSubactionPaths = 2;
      ci.subactionPaths = m_hands;
    }
    return Check(xrCreateAction(m_actionSet, &ci, &out), name);
  };
  if (!make(m_aimPose, XR_ACTION_TYPE_POSE_INPUT, "aim", "Aim", true) ||
      !make(m_trigger, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Fire", true) ||
      !make(m_grip, XR_ACTION_TYPE_FLOAT_INPUT, "grip", "Grip", true) ||
      !make(m_a, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", "Start", false) ||
      !make(m_b, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", "Coin", false) ||
      !make(m_x, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x", "Coin (left)", false) ||
      !make(m_y, XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y", "Start (left)", false) ||
      !make(m_menu, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu", false) ||
      !make(m_thumbClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumb_click", "Thumbstick click", true) ||
      !make(m_haptic, XR_ACTION_TYPE_VIBRATION_OUTPUT, "recoil", "Recoil", true))
    return false;

  auto path = [&](const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(m_instance, s, &p);
    return p;
  };

  // Quest Touch controllers (Virtual Desktop, Link and SteamVR all expose this profile).
  const XrActionSuggestedBinding touch[] = {
    {m_aimPose,    path("/user/hand/right/input/aim/pose")},
    {m_aimPose,    path("/user/hand/left/input/aim/pose")},
    {m_trigger,    path("/user/hand/right/input/trigger/value")},
    {m_trigger,    path("/user/hand/left/input/trigger/value")},
    {m_grip,       path("/user/hand/right/input/squeeze/value")},
    {m_grip,       path("/user/hand/left/input/squeeze/value")},
    {m_a,          path("/user/hand/right/input/a/click")},
    {m_b,          path("/user/hand/right/input/b/click")},
    {m_x,          path("/user/hand/left/input/x/click")},
    {m_y,          path("/user/hand/left/input/y/click")},
    {m_menu,       path("/user/hand/left/input/menu/click")},
    {m_thumbClick, path("/user/hand/right/input/thumbstick/click")},
    {m_thumbClick, path("/user/hand/left/input/thumbstick/click")},
    {m_haptic,     path("/user/hand/right/output/haptic")},
    {m_haptic,     path("/user/hand/left/output/haptic")},
  };
  XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
  suggested.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
  suggested.suggestedBindings = touch;
  suggested.countSuggestedBindings = (uint32_t)(sizeof(touch) / sizeof(touch[0]));
  if (!Check(xrSuggestInteractionProfileBindings(m_instance, &suggested), "suggest touch bindings"))
    return false;

  // Generic fallback for any other controller.
  const XrActionSuggestedBinding simple[] = {
    {m_aimPose, path("/user/hand/right/input/aim/pose")},
    {m_aimPose, path("/user/hand/left/input/aim/pose")},
    {m_trigger, path("/user/hand/right/input/select/click")},
    {m_trigger, path("/user/hand/left/input/select/click")},
    {m_a,       path("/user/hand/right/input/menu/click")},
    {m_b,       path("/user/hand/left/input/menu/click")},
    {m_haptic,  path("/user/hand/right/output/haptic")},
    {m_haptic,  path("/user/hand/left/output/haptic")},
  };
  suggested.interactionProfile = path("/interaction_profiles/khr/simple_controller");
  suggested.suggestedBindings = simple;
  suggested.countSuggestedBindings = (uint32_t)(sizeof(simple) / sizeof(simple[0]));
  xrSuggestInteractionProfileBindings(m_instance, &suggested);   // optional

  for (int h = 0; h < 2; h++) {
    XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    sci.action = m_aimPose;
    sci.subactionPath = m_hands[h];
    sci.poseInActionSpace.orientation.w = 1.0f;
    if (!Check(xrCreateActionSpace(m_session, &sci, &m_aimSpaces[h]), "xrCreateActionSpace"))
      return false;
  }

  XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attach.countActionSets = 1;
  attach.actionSets = &m_actionSet;
  return Check(xrAttachSessionActionSets(m_session, &attach), "xrAttachSessionActionSets");
}

// Comfort vignette: darkens the view outside a circle around the eye's centre (in degrees).
const char* kVignetteFS = R"(#version 330 core
uniform vec4 uTan;       // eye field of view: tan left, right, up, down
uniform vec2 uSize;      // eye image size
uniform float uRadius;   // open area (radians from the view direction)
uniform float uSoft;     // width of the fade (radians)
out vec4 fragColor;
void main() {
  vec2 uv = gl_FragCoord.xy / uSize;
  vec2 t = vec2(mix(uTan.x, uTan.y, uv.x), mix(uTan.w, uTan.z, uv.y));
  float ang = atan(length(t));
  float a = smoothstep(uRadius, uRadius + uSoft, ang);
  fragColor = vec4(0.0, 0.0, 0.0, a);
})";

// Cinema bezel: a recessed frame around the screen. Its four inner walls slope from the
// screen's edge out towards the viewer and reflect a blurred, mirrored copy of the picture;
// the flat front of the frame is dark plastic lit softly by the picture.
// Each draw is one quad: uPos = corners in model space, uSu = the same corners in "screen
// coordinates" (0..1 across the picture, outside it beyond the edge).
const char* kBezelVS = R"(#version 330 core
uniform mat4 uMvp;
uniform vec3 uPos[4];
uniform vec2 uSu[4];
out vec2 vSu;
void main() { vSu = uSu[gl_VertexID]; gl_Position = uMvp * vec4(uPos[gl_VertexID], 1.0); }
)";
const char* kBezelFS = R"(#version 330 core
uniform sampler2D uTex;
uniform int uKind;        // 0 inner wall (reflective), 1 front of the cabinet
uniform vec2 uAspect;     // screen width / height, recess depth (wall width) in screen heights
uniform float uLight;     // 0..1 how much the picture lights the cabinet
uniform float uShade;     // per wall: top walls catch less light than bottom ones
in vec2 vSu;
out vec4 fragColor;
vec3 pic(vec2 su, float lod) { return textureLod(uTex, vec2(su.x, 1.0 - su.y), lod).rgb; }
// a soft blur: several taps of a small mip level around a point
vec3 soft(vec2 su, float lod, float r) {
  vec3 c = vec3(0.0);
  for (int i = 0; i < 8; i++) {
    float a = float(i) * 0.785398;
    c += pic(clamp(su + r * vec2(cos(a), sin(a) * uAspect.x), 0.0, 1.0), lod);
  }
  return (c + pic(su, lod) * 2.0) / 10.0;
}
float grain(vec2 p) { return fract(sin(dot(floor(p), vec2(12.9898, 78.233))) * 43758.5453); }
void main() {
  vec2 e = clamp(vSu, 0.0, 1.0);
  vec2 off = (vSu - e) * vec2(uAspect.x, 1.0);
  float dist = length(off);                       // screen heights from the picture's edge
  float lodBase = log2(float(textureSize(uTex, 0).y));
  // TV-cabinet plastic: dark warm grey with a fine grain
  vec3 plastic = vec3(0.060, 0.058, 0.056) * (0.92 + 0.16 * grain(vSu * vec2(900.0 * uAspect.x, 900.0)));
  if (uKind == 0) {
    float w = clamp(dist / max(uAspect.y, 1e-4), 0.0, 1.0);   // 0 at the screen, 1 at the cabinet front
    vec2 r = clamp(e - (vSu - e) * 1.25, 0.0, 1.0);          // mirrored across the edge
    // a hazy reflection, as on matte plastic: already soft at the screen, very soft further out
    vec3 refl = soft(r, lodBase - 6.0 + w * 1.5, 0.010 + 0.035 * w);
    vec3 lit  = pic(e, lodBase - 3.0);                       // diffuse light from the nearby picture
    float k = 0.42 * pow(1.0 - w, 0.7) * uLight;
    float ao = mix(0.55, 1.0, smoothstep(0.0, 0.25, w));     // darker in the crease next to the tube
    vec3 c = (plastic + refl * k + lit * 0.10 * uLight * (1.0 - w)) * ao;
    c *= uShade;
    // rounded lip where the recess meets the front: a soft highlight
    c += vec3(0.030) * smoothstep(0.80, 1.0, w) * (1.0 - smoothstep(0.97, 1.0, w));
    fragColor = vec4(c, 1.0);
  } else {
    vec3 lit = pic(e, lodBase - 2.0);
    float k = exp(-dist * 7.0) * 0.16 * uLight;
    float edge = 1.0 - 0.35 * smoothstep(0.75, 1.0, dist / max(uAspect.y * 1.8, 1e-4));   // falls off towards the outer edge
    fragColor = vec4((plastic + lit * k) * edge, 1.0);
  }
}
)";

const char* kLineVS = R"(#version 330 core
uniform mat4 uMvp;
uniform vec3 uA, uB;
void main() { gl_Position = uMvp * vec4(gl_VertexID == 0 ? uA : uB, 1.0); }
)";
const char* kLineFS = R"(#version 330 core
uniform vec4 uColor;
out vec4 fragColor;
void main() { fragColor = uColor; }
)";

// Hit "static" outside the game's screen. The game draws its static (noise over the picture)
// on the 2D layer, so it covers only the 4:3 screen. A 1x1 pass measures how much of the layer
// is noise right now; the fill pass then draws matching noise around the screen.
const char* kFullVS = R"(#version 330 core
void main() {
  vec2 p = vec2((gl_VertexID & 1) != 0 ? 1.0 : -1.0, (gl_VertexID & 2) != 0 ? 1.0 : -1.0);
  gl_Position = vec4(p, 0.0, 1.0);
})";

const char* kStaticDetectFS = R"(#version 330 core
// The 2D layer's pixels are either solid or see-through, so the game's static is a fine scatter
// of solid grey pixels. Measure how often neighbouring pixels flip between solid and clear
// (high for static, low for text and the HUD) and the share of solid pixels (its density),
// below the score/timer rows.
uniform sampler2D uTex;
out vec4 fragColor;
void main() {
  ivec2 size = textureSize(uTex, 0);
  float flips = 0.0, solid = 0.0, total = 0.0;
  for (int y = 52; y < size.y - 2; y += 6)
    for (int x = 3; x < size.x - 2; x += 6) {
      float a  = step(0.5, texelFetch(uTex, ivec2(x, y), 0).a);
      float ax = step(0.5, texelFetch(uTex, ivec2(x + 1, y), 0).a);
      float ay = step(0.5, texelFetch(uTex, ivec2(x, y + 1), 0).a);
      total += 1.0;
      solid += a;
      flips += abs(a - ax) + abs(a - ay);
    }
  float busy = flips / max(2.0 * total, 1.0);
  fragColor = vec4(smoothstep(0.12, 0.28, busy), solid / max(total, 1.0), 0.0, 1.0);
})";

// Noise on the plane of the game's screen, outside it, with the game's pixel size.
const char* kStaticVS = R"(#version 330 core
uniform mat4 uMvp;
uniform vec4 uRect;   // area to fill (left, right, top, bottom) on the screen plane
out vec2 vPos;
void main() {
  int id = gl_VertexID;
  bool left = (id == 0 || id == 2);
  bool bottom = (id < 2);
  vPos = vec2(left ? uRect.x : uRect.y, bottom ? uRect.w : uRect.z);
  gl_Position = uMvp * vec4(vPos, -1.0, 1.0);
})";

const char* kStaticFS = R"(#version 330 core
uniform vec4 uGame;          // the game's screen (left, right, top, bottom)
uniform sampler2D uAmount;   // 1x1: r = how much static is on, g = its opacity
uniform float uSeed;
in vec2 vPos;
out vec4 fragColor;
float hash(vec3 p) {
  p = fract(p * vec3(0.1031, 0.1030, 0.0973));
  p += dot(p, p.yxz + 33.33);
  return fract((p.x + p.y) * p.z);
}
void main() {
  if (vPos.x > uGame.x && vPos.x < uGame.y && vPos.y < uGame.z && vPos.y > uGame.w) discard;
  vec4 amount = texture(uAmount, vec2(0.5));   // r: static on (0..1), g: density of solid pixels
  if (amount.r <= 0.01) discard;
  vec2 texel = floor(vec2((vPos.x - uGame.x) / (uGame.y - uGame.x) * 496.0,
                          (uGame.z - vPos.y) / (uGame.z - uGame.w) * 384.0));
  // like the game: a scatter of solid grey pixels at the measured density, the rest clear
  if (hash(vec3(texel, uSeed)) > amount.g * amount.r) discard;
  float v = hash(vec3(texel.yx + 17.0, uSeed + 31.0));
  fragColor = vec4(vec3(v), 1.0);
})";

// An even, see-through tint over the whole 2D layer (e.g. a fade or the CONTINUE screen's white
// veil): measured along the layer's left and right edges, below the score rows. Output: the
// tint, premultiplied, or transparent when the edges aren't one even, partly see-through colour.
const char* kTintDetectFS = R"(#version 330 core
uniform sampler2D uTex;
out vec4 fragColor;
vec4 edgeTexel(ivec2 size, int i, int n) {
  int rows = (size.y - 30 - 48) / 4;
  int side = i / rows, r = i - side * rows;
  int x = (side == 0) ? 2 + (r % 4) * 10 : size.x - 3 - (r % 4) * 10;
  vec4 c = texelFetch(uTex, ivec2(x, 48 + r * 4), 0);
  return vec4(c.rgb * c.a, c.a);
}
void main() {
  ivec2 size = textureSize(uTex, 0);
  int n = 2 * ((size.y - 30 - 48) / 4);
  vec4 sum = vec4(0.0);
  for (int i = 0; i < n; i++) sum += edgeTexel(size, i, n);
  vec4 mean = sum / float(n);
  float same = 0.0;
  for (int i = 0; i < n; i++) {
    vec4 d = abs(edgeTexel(size, i, n) - mean);
    same += step(max(max(d.r, d.g), max(d.b, d.a)), 0.06);
  }
  fragColor = (mean.a > 0.03 && same > 0.92 * float(n)) ? mean : vec4(0.0);
})";

const char* kTintFS = R"(#version 330 core
uniform vec4 uGame;          // the game's screen (left, right, top, bottom)
uniform sampler2D uTint;     // 1x1, premultiplied
in vec2 vPos;
out vec4 fragColor;
void main() {
  if (vPos.x > uGame.x && vPos.x < uGame.y && vPos.y < uGame.z && vPos.y > uGame.w) discard;
  vec4 t = texture(uTint, vec2(0.5));
  if (t.a <= 0.0) discard;
  fragColor = t;
})";

// Gun model held in the hand: lit by the room (soft top light) and by the cinema screen in
// front, whose average colour (smallest mip of the cinema picture) tints the light.
const char* kGunVS = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(location = 2) in vec3 aCol;
layout(location = 3) in vec2 aUv;
layout(location = 4) in vec3 aFaceNrm;
uniform mat4 uMvp;
uniform mat4 uModel;
out vec3 vNrm;
out vec3 vCol;
out vec3 vWorld;
out vec2 vUv;
out vec3 vFaceNrm;
void main()
{
  vFaceNrm = mat3(uModel) * aFaceNrm;
  vec4 w = uModel * vec4(aPos, 1.0);
  vWorld = w.xyz;
  vNrm = mat3(uModel) * aNrm;
  vCol = aCol;
  vUv = aUv;
  gl_Position = uMvp * vec4(aPos, 1.0);
}
)";

const char* kGunFS = R"(#version 330 core
// Moulded plastic: soft diffuse, a white (uncoloured) glossy highlight and a Fresnel sheen
// that reflects the room - the bright cinema screen in front, a dim ceiling, a dark floor.
in vec3 vNrm;
in vec3 vCol;
in vec3 vWorld;
in vec2 vUv;
in vec3 vFaceNrm;
uniform vec3 uEye;
uniform float uCullByNormal;   // 1: the game's guns - draw only the side whose face normal faces the eye
uniform sampler2D uGunTex;
uniform vec2 uTextured;  // x = 1: textured part (game's gun from the ROM); y = 1: alpha-tested
uniform sampler2D uScreen;
uniform float uUseScreen;
uniform vec2 uSpec;      // specular strength (Ks), shininess (Ns)
uniform vec4 uTint;      // a = 1: the whole model in this colour (rgb) instead of its own
out vec4 fragColor;
vec3 room(vec3 d, vec3 sc, float rough)
{
  // d: direction; the screen sits ahead (-Z), slightly above eye level. A rougher surface
  // sees it as a wider, dimmer blur (matte plastic) instead of a sharp mirror image.
  float up = d.y;
  vec3 c = mix(vec3(0.015), vec3(0.05), smoothstep(-0.3, 0.6, up));
  float scr = smoothstep(mix(0.80, 0.15, rough), mix(0.97, 0.85, rough),
                         dot(d, normalize(vec3(0.0, 0.08, -1.0))));
  return c + scr * sc * mix(1.4, 0.30, rough);
}
void main()
{
  vec3 n = normalize(vNrm);
  vec3 v = normalize(uEye - vWorld);
  if (uCullByNormal > 0.5) {
    if (dot(vFaceNrm, uEye - vWorld) < 0.0)
      discard;            // the back side (the game culls by the face normal too)
    if (dot(n, v) < 0.0)
      n = -n;             // smooth normals bent away at a silhouette
  } else if (!gl_FrontFacing) {
    n = -n;
  }
  vec3 sc = vec3(0.75, 0.78, 0.85);
  if (uUseScreen > 0.5)
    sc = clamp(textureLod(uScreen, vec2(0.5), 16.0).rgb * 1.6 + 0.12, 0.0, 1.0);
  vec3 ls = normalize(vec3(0.0, 0.15, -1.0));   // towards the screen
  vec3 lt = normalize(vec3(0.3, 1.0, 0.25));    // room light from above
  float wt = max(dot(n, lt) * 0.6 + 0.4, 0.0);  // wrapped: plastic scatters a little
  float ws = max(dot(n, ls) * 0.7 + 0.3, 0.0);
  vec3 col = uTint.a > 0.5 ? uTint.rgb : vCol;
  if (uTextured.x > 0.5) {
    vec4 t = texture(uGunTex, vUv);
    if (uTextured.y > 0.5 && t.a < 0.5)
      discard;
    col *= t.rgb;
  }
  vec3 diffuse = col * (vec3(0.18) + 0.55 * wt * wt * vec3(1.0) + 0.70 * ws * ws * sc);
  if (uCullByNormal > 0.5) {
    // The game's guns: dark gun-metal colours (face colour x texture), lit in the game by a
    // strong scene light. Here a soft key light from just above the eye plus a fill, so every
    // face you look at reads, instead of the room light leaving the sides black.
    vec3 key = normalize(v + vec3(0.0, 0.6, 0.0));
    float k = max(dot(n, key), 0.0);
    diffuse = col * (0.55 + 1.05 * k) * mix(vec3(1.0), sc, 0.25);
  }
  float ns = max(uSpec.y, 1.0);
  float norm = (ns + 8.0) / 25.0;               // energy-normalised Blinn
  float s1 = pow(max(dot(n, normalize(ls + v)), 0.0), ns) * norm;
  float s2 = pow(max(dot(n, normalize(lt + v)), 0.0), ns) * norm;
  float rough = clamp(1.0 - log2(ns) / 9.0, 0.0, 1.0);   // Ns 10 -> 0.63, Ns 90 -> 0.28
  // rough surfaces lose most of the grazing-angle mirror effect
  float fres = 0.04 + 0.96 * pow(1.0 - max(dot(n, v), 0.0), 5.0) * mix(1.0, 0.2, rough);
  vec3 spec = uSpec.x * (0.30 * s1 * mix(vec3(1.0), sc, 0.35) + 0.22 * s2);
  vec3 refl = room(reflect(-v, n), sc, rough) * fres * (0.4 + uSpec.x);
  fragColor = vec4(diffuse + spec + refl, 1.0);
}
)";

bool DesktopXR::CreateShaders()
{
  m_quadProgram = LinkProgram(kQuadVS, kQuadFS);
  m_dotProgram = LinkProgram(kDotVS, kDotFS);
  if (!m_quadProgram || !m_dotProgram)
    return Fail("VR shaders failed to build");
  m_uMvp  = glGetUniformLocation(m_quadProgram, "uMvp");
  m_uTex  = glGetUniformLocation(m_quadProgram, "uTex");
  m_uRect = glGetUniformLocation(m_quadProgram, "uRect");
  m_uMode = glGetUniformLocation(m_quadProgram, "uMode");
  m_uKeep = glGetUniformLocation(m_quadProgram, "uKeep");
  m_uFilter = glGetUniformLocation(m_quadProgram, "uFilter");
  m_uSubUv = glGetUniformLocation(m_quadProgram, "uSubUv");
  m_uGlow = glGetUniformLocation(m_quadProgram, "uGlow");
  m_uUvXform = glGetUniformLocation(m_quadProgram, "uUvXform");
  glUseProgram(m_quadProgram);
  glUniform4f(m_uUvXform, 1.f, 1.f, 0.f, 0.f);
  glUseProgram(0);
  m_uDotCenter = glGetUniformLocation(m_dotProgram, "uCenter");
  m_uDotRadius = glGetUniformLocation(m_dotProgram, "uRadius");
  m_uDotColor  = glGetUniformLocation(m_dotProgram, "uColor");
  m_uDotAspect = glGetUniformLocation(m_dotProgram, "uAspect");
  glGenVertexArrays(1, &m_vao);

  m_vignetteProgram = LinkProgram(kFullVS, kVignetteFS);
  if (m_vignetteProgram) {
    m_uVigTan    = glGetUniformLocation(m_vignetteProgram, "uTan");
    m_uVigSize   = glGetUniformLocation(m_vignetteProgram, "uSize");
    m_uVigRadius = glGetUniformLocation(m_vignetteProgram, "uRadius");
    m_uVigSoft   = glGetUniformLocation(m_vignetteProgram, "uSoft");
  }
  m_gunProgram = LinkProgram(kGunVS, kGunFS);
  if (m_gunProgram) {
    m_uGunMvp       = glGetUniformLocation(m_gunProgram, "uMvp");
    m_uGunModel     = glGetUniformLocation(m_gunProgram, "uModel");
    m_uGunEye       = glGetUniformLocation(m_gunProgram, "uEye");
    m_uGunScreen    = glGetUniformLocation(m_gunProgram, "uScreen");
    m_uGunUseScreen = glGetUniformLocation(m_gunProgram, "uUseScreen");
    m_uGunSpec      = glGetUniformLocation(m_gunProgram, "uSpec");
    m_uGunTint      = glGetUniformLocation(m_gunProgram, "uTint");
    m_uGunTex       = glGetUniformLocation(m_gunProgram, "uGunTex");
    m_uGunTextured  = glGetUniformLocation(m_gunProgram, "uTextured");
    m_uGunCull      = glGetUniformLocation(m_gunProgram, "uCullByNormal");
  }
  m_bezelProgram = LinkProgram(kBezelVS, kBezelFS);
  if (m_bezelProgram) {
    m_uBzMvp    = glGetUniformLocation(m_bezelProgram, "uMvp");
    m_uBzPos    = glGetUniformLocation(m_bezelProgram, "uPos");
    m_uBzSu     = glGetUniformLocation(m_bezelProgram, "uSu");
    m_uBzTex    = glGetUniformLocation(m_bezelProgram, "uTex");
    m_uBzKind   = glGetUniformLocation(m_bezelProgram, "uKind");
    m_uBzAspect = glGetUniformLocation(m_bezelProgram, "uAspect");
    m_uBzLight  = glGetUniformLocation(m_bezelProgram, "uLight");
    m_uBzShade  = glGetUniformLocation(m_bezelProgram, "uShade");
  }
  m_lineProgram = LinkProgram(kLineVS, kLineFS);
  if (!m_lineProgram)
    return Fail("VR shaders failed to build");
  m_uLineMvp   = glGetUniformLocation(m_lineProgram, "uMvp");
  m_uLineA     = glGetUniformLocation(m_lineProgram, "uA");
  m_uLineB     = glGetUniformLocation(m_lineProgram, "uB");
  m_uLineColor = glGetUniformLocation(m_lineProgram, "uColor");

  m_staticDetectProgram = LinkProgram(kFullVS, kStaticDetectFS);
  m_staticProgram = LinkProgram(kStaticVS, kStaticFS);
  if (m_staticDetectProgram && m_staticProgram) {
    m_uStaticMvp    = glGetUniformLocation(m_staticProgram, "uMvp");
    m_uStaticRect   = glGetUniformLocation(m_staticProgram, "uRect");
    m_uStaticGame   = glGetUniformLocation(m_staticProgram, "uGame");
    m_uStaticAmount = glGetUniformLocation(m_staticProgram, "uAmount");
    m_uStaticSeed   = glGetUniformLocation(m_staticProgram, "uSeed");
    glGenTextures(1, &m_staticTex);
    glBindTexture(GL_TEXTURE_2D, m_staticTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &m_staticFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_staticFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_staticTex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  else
    m_config.staticFill = false;   // optional effect: VR keeps working without it
  m_tintDetectProgram = LinkProgram(kFullVS, kTintDetectFS);
  m_tintProgram = LinkProgram(kStaticVS, kTintFS);
  if (m_tintDetectProgram && m_tintProgram) {
    m_uTintMvp  = glGetUniformLocation(m_tintProgram, "uMvp");
    m_uTintRect = glGetUniformLocation(m_tintProgram, "uRect");
    m_uTintGame = glGetUniformLocation(m_tintProgram, "uGame");
    m_uTintTex  = glGetUniformLocation(m_tintProgram, "uTint");
    glGenTextures(1, &m_tintTex);
    glBindTexture(GL_TEXTURE_2D, m_tintTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &m_tintFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_tintFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_tintTex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  return true;
}

void DesktopXR::Shutdown()
{
  m_sessionRunning = false;
  StopWaitThread();
  if (New3D::GetVRFrameTarget() == this)
    New3D::SetVRFrameTarget(nullptr);
  if (m_lineProgram) glDeleteProgram(m_lineProgram);
  if (m_bezelProgram) glDeleteProgram(m_bezelProgram);
  for (auto& p : m_gunParts)
    if (p.tex) glDeleteTextures(1, &p.tex);
  m_gunParts.clear();
  if (m_gunProgram) glDeleteProgram(m_gunProgram);
  for (int i = 0; i < 3; i++) {
    if (m_gunVbo[i]) glDeleteBuffers(1, &m_gunVbo[i]);
    if (m_gunVao[i]) glDeleteVertexArrays(1, &m_gunVao[i]);
    m_gunVbo[i] = m_gunVao[i] = 0;
    m_gunCount[i] = 0;
  }
  if (m_gunDepth) glDeleteRenderbuffers(1, &m_gunDepth);
  m_gunDepth = 0;
  m_gunProgram = 0;
  m_lineProgram = 0;
  if (m_vignetteProgram) glDeleteProgram(m_vignetteProgram);
  m_vignetteProgram = 0;
  for (int v = 0; v < 2; v++) {
    if (m_cinemaFbo[v]) glDeleteFramebuffers(1, &m_cinemaFbo[v]);
    if (m_cinemaTex[v]) glDeleteTextures(1, &m_cinemaTex[v]);
    m_cinemaFbo[v] = m_cinemaTex[v] = 0;
  }
  if (m_staticFbo) glDeleteFramebuffers(1, &m_staticFbo);
  if (m_staticTex) glDeleteTextures(1, &m_staticTex);
  if (m_staticProgram) glDeleteProgram(m_staticProgram);
  if (m_staticDetectProgram) glDeleteProgram(m_staticDetectProgram);
  m_staticFbo = m_staticTex = m_staticProgram = m_staticDetectProgram = 0;
  if (m_tintFbo) glDeleteFramebuffers(1, &m_tintFbo);
  if (m_tintTex) glDeleteTextures(1, &m_tintTex);
  if (m_tintProgram) glDeleteProgram(m_tintProgram);
  if (m_tintDetectProgram) glDeleteProgram(m_tintDetectProgram);
  m_tintFbo = m_tintTex = m_tintProgram = m_tintDetectProgram = 0;
  if (m_vao) glDeleteVertexArrays(1, &m_vao);
  if (m_quadProgram) glDeleteProgram(m_quadProgram);
  if (m_dotProgram) glDeleteProgram(m_dotProgram);
  m_vao = m_quadProgram = m_dotProgram = 0;
  for (auto& e : m_eyes)
    DestroySwapchain(e);
  DestroySwapchain(m_hud);
  if (xrDestroySpace) {
    for (auto& s : m_aimSpaces)
      if (s != XR_NULL_HANDLE) { xrDestroySpace(s); s = XR_NULL_HANDLE; }
    for (XrSpace* s : {&m_viewSpace, &m_localSpace, &m_stageSpace})
      if (*s != XR_NULL_HANDLE) { xrDestroySpace(*s); *s = XR_NULL_HANDLE; }
  }
  if (m_actionSet != XR_NULL_HANDLE && xrDestroyActionSet) { xrDestroyActionSet(m_actionSet); m_actionSet = XR_NULL_HANDLE; }
  if (m_session != XR_NULL_HANDLE && xrDestroySession) { xrDestroySession(m_session); m_session = XR_NULL_HANDLE; }
  if (m_instance != XR_NULL_HANDLE && xrDestroyInstance) { xrDestroyInstance(m_instance); m_instance = XR_NULL_HANDLE; }
  m_sessionRunning = false;
#ifdef _WIN32
  if (m_loaderLib) FreeLibrary((HMODULE)m_loaderLib);
#else
  if (m_loaderLib) dlclose(m_loaderLib);
#endif
  m_loaderLib = nullptr;
}

// ---------------------------------------------------------------------------------------------
// Session / frame loop
// ---------------------------------------------------------------------------------------------

void DesktopXR::HandleSessionState(XrSessionState state)
{
  switch (state) {
    case XR_SESSION_STATE_READY: {
      XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
      bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
      if (Check(xrBeginSession(m_session, &bi), "xrBeginSession")) {
        m_sessionRunning = true;
        m_recenterPending = true;
        InfoLog("VR: session running");
        if (m_config.threadedWait)
          StartWaitThread();
        if (pfnRequestDisplayRefreshRateFB && m_config.refreshHz > 0.f) {
          // The highest refresh the headset offers, up to VRRefreshHz. (Asking for a rate the
          // runtime doesn't list is refused - e.g. Virtual Desktop only offers the rate chosen
          // in its own settings.)
          std::vector<float> rates;
          uint32_t n = 0;
          if (pfnEnumerateDisplayRefreshRatesFB && XR_SUCCEEDED(pfnEnumerateDisplayRefreshRatesFB(m_session, 0, &n, nullptr)) && n) {
            rates.resize(n);
            if (XR_FAILED(pfnEnumerateDisplayRefreshRatesFB(m_session, n, &n, rates.data())))
              rates.clear();
            rates.resize(n);
          }
          float current = 0.f;
          if (pfnGetDisplayRefreshRateFB)
            pfnGetDisplayRefreshRateFB(m_session, &current);
          std::string list;
          float best = 0.f;
          for (float hz : rates) {
            char b[16];
            snprintf(b, sizeof(b), "%s%.0f", list.empty() ? "" : " ", hz);
            list += b;
            if (hz <= m_config.refreshHz + 0.5f && hz > best)
              best = hz;
          }
          if (rates.empty())
            best = m_config.refreshHz;   // no list: ask anyway
          if (best > current + 0.5f) {
            XrResult r = pfnRequestDisplayRefreshRateFB(m_session, best);
            InfoLog("VR: headset refresh rates offered: %s (now %.0f Hz) -> asked for %.0f Hz -> %d",
                    list.empty() ? "?" : list.c_str(), current, best, (int)r);
          } else {
            InfoLog("VR: headset refresh rates offered: %s - staying at %.0f Hz (the highest available)",
                    list.empty() ? "?" : list.c_str(), current);
          }
        }
      }
      break;
    }
    case XR_SESSION_STATE_FOCUSED:
      if (!m_focused)
        InfoLog("VR: headset in use (focused)");
      m_focused = true;
      break;
    case XR_SESSION_STATE_VISIBLE:
    case XR_SESSION_STATE_SYNCHRONIZED:
    case XR_SESSION_STATE_IDLE:
      m_focused = false;
      break;
    case XR_SESSION_STATE_STOPPING:
      m_focused = false;
      m_sessionRunning = false;
      StopWaitThread();
      xrEndSession(m_session);
      break;
    case XR_SESSION_STATE_EXITING:
    case XR_SESSION_STATE_LOSS_PENDING:
      m_focused = false;
      m_sessionRunning = false;
      m_exitRequested = true;
      break;
    default:
      break;
  }
}

bool DesktopXR::PollEvents()
{
  XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(m_instance, &ev) == XR_SUCCESS) {
    switch (ev.type) {
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
        HandleSessionState(reinterpret_cast<XrEventDataSessionStateChanged*>(&ev)->state);
        break;
      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
        m_exitRequested = true;
        break;
      case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
        m_recenterPending = true;   // the user recentred from the system menu
        break;
      default:
        break;
    }
    ev = {XR_TYPE_EVENT_DATA_BUFFER};
  }
  return !m_exitRequested;
}

void DesktopXR::SetMenu(GLuint tex, bool open, float aspect)
{
  m_menuTex = tex;
  kMenuWidth = 1.30f;
  kMenuHeight = kMenuWidth * std::clamp(aspect, 0.3f, 1.5f);
  if (open && !m_menuOpen) {
    // place the panel ~1.4 m in front of the head, level, facing the player
    const XrVector3f f = Rotate(m_headPose.orientation, {0.f, 0.f, -1.f});
    m_menuYaw = std::atan2(-f.x, -f.z);
    const XrVector3f hp = m_headPose.position;
    m_menuCenter = {hp.x - std::sin(m_menuYaw) * kMenuDistance, hp.y - 0.10f, hp.z - std::cos(m_menuYaw) * kMenuDistance};
  }
  m_menuOpen = open;
}

void DesktopXR::Haptic(int hand, float amplitude, float seconds)
{
  if (!m_sessionRunning || m_haptic == XR_NULL_HANDLE || hand < 0 || hand > 1 || !xrApplyHapticFeedback)
    return;
  XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
  info.action = m_haptic;
  info.subactionPath = m_hands[hand];
  XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
  vib.amplitude = amplitude;
  vib.duration = (XrDuration)(seconds * 1e9);
  vib.frequency = XR_FREQUENCY_UNSPECIFIED;
  xrApplyHapticFeedback(m_session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vib));
}

void DesktopXR::DrawMenuInEye(const New3D::VREye& eye)
{
  float proj[16];
  EyeProjection(eye, proj);
  const float s = m_config.worldUnitsPerMeter;
  const float cs = std::cos(m_menuYaw), sn = std::sin(m_menuYaw);
  // quad at z = -1 in panel space -> the panel centre, turned to face the player
  const float model[16] = { cs * s, 0, -sn * s, 0,   0, s, 0, 0,   sn * s, 0, cs * s, 0,
                            (m_menuCenter.x + sn) * s, m_menuCenter.y * s, (m_menuCenter.z + cs) * s, 1 };
  float vm[16], mvp[16];
  New3D::VRMul4(eye.view, model, vm);
  New3D::VRMul4(proj, vm, mvp);

  glBindFramebuffer(GL_FRAMEBUFFER, eye.fbo);
  glViewport(0, 0, eye.width, eye.height);
  glDisable(GL_SCISSOR_TEST);
  // Same space as the guns in your hands: test against their depth (drawn just before), so a
  // gun nearer than the panel stays in front of it and one behind it is covered.
  const bool gunDepth = m_gunDepthValid && m_gunDepth && m_gunDepthW == eye.width && m_gunDepthH == eye.height;
  if (gunDepth) {
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_gunDepth);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_FALSE);
  } else {
    glDisable(GL_DEPTH_TEST);
  }
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // the menu image is premultiplied, see-through around the artwork
  glBindVertexArray(m_vao);
  glUseProgram(m_quadProgram);
  glUniformMatrix4fv(m_uMvp, 1, GL_FALSE, mvp);
  glUniform4f(m_uRect, -0.5f * kMenuWidth, 0.5f * kMenuWidth, 0.5f * kMenuHeight, -0.5f * kMenuHeight);
  glUniform1i(m_uMode, 4);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(m_uTex, 0);
  glBindTexture(GL_TEXTURE_2D, m_menuTex);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // pointer rays from both controllers (to the panel, or 2 m)
  float pv[16], pmvp[16];
  const float id[16] = { s, 0, 0, 0,  0, s, 0, 0,  0, 0, s, 0,  0, 0, 0, 1 };
  New3D::VRMul4(eye.view, id, pv);
  New3D::VRMul4(proj, pv, pmvp);
  glUseProgram(m_lineProgram);
  glUniformMatrix4fv(m_uLineMvp, 1, GL_FALSE, pmvp);
  for (int h = 0; h < 2; h++) {
    if (!m_input.rayValid[h])
      continue;
    const XrVector3f o = m_input.rayOrigin[h], d = m_input.rayDir[h];
    const float len = m_input.menuGrab[h] ? m_menuGrabDist : (m_input.menuHit[h] ? m_input.menuDist[h] : 2.0f);
    glUniform3f(m_uLineA, o.x, o.y, o.z);
    glUniform3f(m_uLineB, o.x + d.x * len, o.y + d.y * len, o.z + d.z * len);
    if (m_input.menuGrab[h])     glUniform4f(m_uLineColor, 0.35f, 0.75f, 1.0f, 1.0f);   // carrying the menu
    else if (m_input.menuHit[h]) glUniform4f(m_uLineColor, 1.0f, 0.6f, 0.2f, 1.0f);
    else                    glUniform4f(m_uLineColor, 0.6f, 0.6f, 0.6f, 1.0f);
    glDrawArrays(GL_LINES, 0, 2);
  }
  glBindVertexArray(0);
  glUseProgram(0);
  if (gunDepth) {
    glDepthMask(GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void DesktopXR::EyeProjection(const New3D::VREye& eye, float proj[16]) const
{
  const float d = m_config.hudDistance;
  const float n = 0.05f, f = std::max(1000.f, 4.f * d * m_config.worldUnitsPerMeter);
  const float l = n * eye.tanLeft, r = n * eye.tanRight, t = n * eye.tanUp, b = n * eye.tanDown;
  for (int i = 0; i < 16; i++) proj[i] = 0.f;
  proj[0]  = 2 * n / (r - l);
  proj[5]  = 2 * n / (t - b);
  proj[8]  = (r + l) / (r - l);
  proj[9]  = (t + b) / (t - b);
  proj[10] = -(f + n) / (f - n);
  proj[11] = -1;
  proj[14] = -2 * f * n / (f - n);
}

void DesktopXR::Recenter()
{
  m_recenterPending = true;
}

void DesktopXR::RecreateLocalSpace(const XrPosef& origin)
{
  XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  rs.poseInReferenceSpace = origin;
  XrSpace space = XR_NULL_HANDLE;
  if (XR_SUCCEEDED(xrCreateReferenceSpace(m_session, &rs, &space))) {
    if (m_localSpace != XR_NULL_HANDLE)
      xrDestroySpace(m_localSpace);
    m_localSpace = space;
  }
}

void DesktopXR::StartWaitThread()
{
  if (m_waitThread.joinable())
    return;
  m_waitQuit = false;
  m_waitRequested = false;
  m_waitDone = false;
  m_waitThread = std::thread([this] {
    std::unique_lock<std::mutex> lock(m_waitMutex);
    for (;;) {
      m_waitCv.wait(lock, [&] { return m_waitQuit || (m_waitRequested && !m_waitDone); });
      if (m_waitQuit)
        return;
      lock.unlock();
      XrFrameState st{XR_TYPE_FRAME_STATE};
      XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
      XrResult r = m_sessionRunning ? xrWaitFrame(m_session, &wi, &st) : XR_ERROR_SESSION_NOT_RUNNING;
      lock.lock();
      m_waitState = st;
      m_waitResult = r;
      m_waitDone = true;
      m_waitCv.notify_all();
    }
  });
}

void DesktopXR::StopWaitThread()
{
  if (!m_waitThread.joinable())
    return;
  {
    std::lock_guard<std::mutex> lock(m_waitMutex);
    m_waitQuit = true;
  }
  m_waitCv.notify_all();
  m_waitThread.join();
  m_waitQuit = false;
  m_waitRequested = false;
  m_waitDone = false;
}

void DesktopXR::RequestWait()
{
  if (!m_waitThread.joinable() || !m_sessionRunning)
    return;
  std::lock_guard<std::mutex> lock(m_waitMutex);
  if (!m_waitRequested) {
    m_waitRequested = true;
    m_waitDone = false;
    m_waitCv.notify_all();
  }
}

bool DesktopXR::WaitForFrame(int maxMicroseconds)
{
  if (!m_waitThread.joinable() || !m_sessionRunning)
    return true;
  RequestWait();
  std::unique_lock<std::mutex> lock(m_waitMutex);
  m_waitCv.wait_for(lock, std::chrono::microseconds(maxMicroseconds), [&] { return m_waitDone || m_waitQuit || m_extEvent; });
  m_extEvent = false;
  return m_waitDone || m_waitQuit;
}

void DesktopXR::NotifyEvent()
{
  {
    std::lock_guard<std::mutex> lock(m_waitMutex);
    m_extEvent = true;
  }
  m_waitCv.notify_all();
}

bool DesktopXR::BeginFrame()
{
  m_frameBegun = false;
  m_viewsValid = false;
  m_eyeDrawn[0] = m_eyeDrawn[1] = false;
  m_hudDrawn = false;
  if (!m_sessionRunning)
    return false;

  m_frameState = {XR_TYPE_FRAME_STATE};
  if (m_waitThread.joinable()) {
    // xrWaitFrame runs on the waiter thread (OpenXR allows that), so the main thread can keep
    // feeding the emulator while the headset paces; collect its result here.
    RequestWait();
    std::unique_lock<std::mutex> lock(m_waitMutex);
    m_waitCv.wait(lock, [&] { return m_waitDone || m_waitQuit; });
    if (m_waitQuit)
      return false;
    m_waitRequested = false;
    m_waitDone = false;
    if (!Check(m_waitResult, "xrWaitFrame"))
      return false;
    m_frameState = m_waitState;
  } else {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    if (!Check(xrWaitFrame(m_session, &wi, &m_frameState), "xrWaitFrame"))
      return false;
  }
  XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
  if (XR_FAILED(xrBeginFrame(m_session, &bi)))
    return false;
  m_frameBegun = true;
  if (m_frameState.predictedDisplayPeriod > 0)
    m_displayPeriod = (double)m_frameState.predictedDisplayPeriod * 1e-9;
  if (m_lastDisplayTime != 0 && m_frameState.predictedDisplayTime > m_lastDisplayTime)
    m_frameDelta = std::min((double)(m_frameState.predictedDisplayTime - m_lastDisplayTime) * 1e-9, 0.1);
  else
    m_frameDelta = m_displayPeriod;
  m_lastDisplayTime = m_frameState.predictedDisplayTime;

  if (m_recenterPending) {
    // New origin: the head's position, rotated only around the vertical axis (keep the
    // floor level), so the game screen is straight ahead.
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(xrLocateSpace(m_viewSpace, m_stageSpace, m_frameState.predictedDisplayTime, &head)) &&
        (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
      const XrVector3f f = Rotate(head.pose.orientation, {0.f, 0.f, -1.f});
      const float yaw = std::atan2(-f.x, -f.z);
      XrPosef origin;
      origin.orientation = {0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
      origin.position = (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ? head.pose.position : XrVector3f{0, 0, 0};
      RecreateLocalSpace(origin);
      m_recenterPending = false;
      InfoLog("VR: recentred");
    }
  }

  SyncInput();
  if (!m_frameState.shouldRender)
    return false;

  XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
  li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  li.displayTime = m_frameState.predictedDisplayTime;
  li.space = m_localSpace;
  XrViewState vs{XR_TYPE_VIEW_STATE};
  uint32_t count = 0;
  m_views[0] = {XR_TYPE_VIEW};
  m_views[1] = {XR_TYPE_VIEW};
  if (XR_FAILED(xrLocateViews(m_session, &li, &vs, 2, &count, m_views)) || count != 2)
    return false;
  m_viewsValid = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;

  XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
  if (XR_SUCCEEDED(xrLocateSpace(m_viewSpace, m_localSpace, m_frameState.predictedDisplayTime, &head)) &&
      (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
    m_headPose = head.pose;

  if (m_viewsValid) {
    const XrVector3f a = m_views[0].pose.position, b = m_views[1].pose.position;
    const float d = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
    if (d > 0.04f && d < 0.09f)
      m_ipd = d;
  }
  UpdateComfort();
  return m_viewsValid;
}

void DesktopXR::UpdateComfort()
{
  // Vignette strength follows how fast the game's camera turns (degrees per second; the game
  // runs at 60 frames per second). Closes quickly, opens again slowly.
  const float speed = m_cameraTurn * 60.f;
  const float target = (m_config.vignette > 0 && m_inPlay && !m_config.cinema)
                       ? std::clamp((speed - 25.f) / (110.f - 25.f), 0.f, 1.f) : 0.f;
  const float dt = (float)std::clamp(m_frameDelta, 0.001, 0.1);
  const float rate = target > m_vignette ? 8.f : 2.5f;   // per second
  m_vignette += (target - m_vignette) * std::min(1.f, rate * dt);
}

void DesktopXR::DrawVignette(const New3D::VREye& eye)
{
  if (!m_vignetteProgram || m_vignette < 0.01f)
    return;
  static const float kMinRadius[4] = { 90.f, 42.f, 32.f, 24.f };   // degrees, fully closed
  const float closed = kMinRadius[std::clamp(m_config.vignette, 0, 3)];
  const float radius = 75.f + (closed - 75.f) * m_vignette;
  glBindFramebuffer(GL_FRAMEBUFFER, eye.fbo);
  glViewport(0, 0, eye.width, eye.height);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBindVertexArray(m_vao);
  glUseProgram(m_vignetteProgram);
  glUniform4f(m_uVigTan, eye.tanLeft, eye.tanRight, eye.tanUp, eye.tanDown);
  glUniform2f(m_uVigSize, (float)eye.width, (float)eye.height);
  glUniform1f(m_uVigRadius, radius * 0.0174533f);
  glUniform1f(m_uVigSoft, 14.f * 0.0174533f);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
  glUseProgram(0);
  glDisable(GL_BLEND);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ---- Cinema mode: the game drawn flat into a texture, shown on a big screen in a dark room

int DesktopXR::FlatViewCount() const
{
  return (m_config.cinema && m_config.cinemaDepth > 0) ? 2 : 1;
}

bool DesktopXR::BeginFlat(int view, New3D::VREye& t)
{
  if (!m_config.cinema || view < 0 || view > 1)
    return false;
  GLuint& m_cinemaTex = this->m_cinemaTex[view];
  GLuint& m_cinemaFbo = this->m_cinemaFbo[view];
  if (!m_cinemaFbo) {
    glGenTextures(1, &m_cinemaTex);
    glBindTexture(GL_TEXTURE_2D, m_cinemaTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kCinemaW, kCinemaH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenerateMipmap(GL_TEXTURE_2D);   // small levels feed the screen glow
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &m_cinemaFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_cinemaFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_cinemaTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      return false;
    }
  }
  // bottom 2D layer (backgrounds) first; the 3D scene is drawn over it by the renderer
  glBindFramebuffer(GL_FRAMEBUFFER, m_cinemaFbo);
  glViewport(0, 0, kCinemaW, kCinemaH);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  glClearColor(0.f, 0.f, 0.f, 0.f);
  const float id[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
  glBindVertexArray(m_vao);
  glUseProgram(m_quadProgram);
  glUniformMatrix4fv(m_uMvp, 1, GL_FALSE, id);
  glUniform4f(m_uRect, -1.f, 1.f, 1.f, -1.f);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(m_uTex, 0);
  if (m_backdropTex) {
    glUniform1i(m_uMode, 0);
    glBindTexture(GL_TEXTURE_2D, m_backdropTex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindVertexArray(0);
  glUseProgram(0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  t.fbo = m_cinemaFbo;
  t.width = kCinemaW;
  t.height = kCinemaH;
  if (FlatViewCount() == 2) {
    // 3D cinema: two views of the game's camera, half an eye distance to each side, with
    // off-axis frustums meeting at the convergence depth. The disparity of far scenery matches
    // the real distance to things behind the screen (Full), or half of it (Shallow).
    const float depth = m_config.cinemaDepth == 1 ? 0.5f : 1.0f;
    const float e = 0.5f * m_ipd * m_config.worldUnitsPerMeter;          // half eye distance, world units
    float sL, sR, sT, sB;
    CinemaRect(sL, sR, sT, sB);
    const float perGame = CinemaScale() * (sT - sB) / std::max(m_gameT - m_gameB, 1e-4f);   // screen tangent per game tangent
    const float farDisparity = depth * m_ipd / (kCinemaDistance * perGame);  // game tangent units
    const float conv = 2.f * e / std::max(farDisparity, 1e-4f);          // depth on the screen surface
    const float shift = (view == 0 ? 1.f : -1.f) * e / conv;             // left eye looks right of centre
    t.stereo = true;
    t.convergence = conv;
    t.tanLeft = m_gameL + shift;
    t.tanRight = m_gameR + shift;
    t.tanUp = m_gameT;
    t.tanDown = m_gameB;
    for (int i = 0; i < 16; i++) t.view[i] = (i % 5 == 0) ? 1.f : 0.f;
    t.view[12] = (view == 0 ? 1.f : -1.f) * e;                           // left eye sits at -e
  }
  return true;
}

void DesktopXR::EndFlat(int view)
{
  if (view < 0 || view > 1 || !m_cinemaFbo[view])
    return;
  // top 2D layer (score, text, crosshair) over the 3D scene
  glBindFramebuffer(GL_FRAMEBUFFER, m_cinemaFbo[view]);
  glViewport(0, 0, kCinemaW, kCinemaH);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  if (m_hudTopTex) {
    const float id[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(m_vao);
    glUseProgram(m_quadProgram);
    glUniformMatrix4fv(m_uMvp, 1, GL_FALSE, id);
    glUniform4f(m_uRect, -1.f, 1.f, 1.f, -1.f);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(m_uTex, 0);
    glUniform1i(m_uMode, 1);
    glUniform1i(m_uFilter, m_config.hudFilter);
    glUniform2f(m_uKeep, 0.f, 1.f);
    glBindTexture(GL_TEXTURE_2D, m_hudTopTex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, m_cinemaTex[view]);
  glGenerateMipmap(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, 0);
  m_cinemaValid[view] = true;
  if (FlatViewCount() == 1)
    m_cinemaValid[1] = false;
}

bool DesktopXR::PointerHand(int hand) const
{
  // Only the gun hand gets a pointer (both when no gun hand is set). Shown wherever the game
  // can be aimed: the 4:3 screen and, with the wide calibration, a screen-width past each edge.
  if (!m_input.aimValid[hand] || (m_pointerHand >= 0 && hand != m_pointerHand))
    return false;
  return m_input.aimX[hand] >= -1.f && m_input.aimX[hand] <= 2.f &&
         m_input.aimY[hand] >= -1.f && m_input.aimY[hand] <= 2.f;
}

void DesktopXR::DrawPointerRings(const float mvp[16], float L, float R, float T, float B)
{
  // Pointer rings at the aim points on a screen whose picture spans L..R, B..T at z = -1 in
  // the model space of mvp (the HUD panel, or the cinema screen).
  auto project = [&](float tx, float ty, float out[2]) {
    const float p[4] = { tx, ty, -1.f, 1.f };
    float c[4];
    for (int i = 0; i < 4; i++)
      c[i] = mvp[i] * p[0] + mvp[4 + i] * p[1] + mvp[8 + i] * p[2] + mvp[12 + i] * p[3];
    out[0] = c[0] / c[3];
    out[1] = c[1] / c[3];
    return c[3] > 0.f;
  };
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // premultiplied
  glBindVertexArray(m_vao);
  glUseProgram(m_dotProgram);
  const float rt = 0.02f * (T - B);
  for (int hand = 0; hand < 2; hand++) {
    if (!PointerHand(hand))
      continue;
    const float tx = L + m_input.aimX[hand] * (R - L);
    const float ty = T - m_input.aimY[hand] * (T - B);
    float c[2], cx[2], cy[2];
    if (!project(tx, ty, c) || !project(tx + rt, ty, cx) || !project(tx, ty + rt, cy))
      continue;
    const float ry = std::fabs(cy[1] - c[1]), rx = std::fabs(cx[0] - c[0]);
    if (ry <= 0.f || rx <= 0.f)
      continue;
    glUniform2f(m_uDotCenter, c[0], c[1]);
    glUniform1f(m_uDotRadius, ry);
    glUniform1f(m_uDotAspect, ry / rx);
    if (hand == 0) glUniform3f(m_uDotColor, 1.f, 0.25f, 0.2f);
    else           glUniform3f(m_uDotColor, 0.2f, 0.8f, 1.f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
}

void DesktopXR::CinemaRect(float& L, float& R, float& T, float& B) const
{
  // The cinema screen keeps one size. The game's own field of view changes (camera zooms,
  // cutscenes); sizing the screen by it made the screen grow and shrink - it seemed to move
  // back and forth. The picture is the game's whole 4:3 frame whatever its field of view.
  const float h = kCinemaTanHalfHeight;
  T = h;
  B = -h;
  R = h * (496.f / 384.f);
  L = -R;
}

float DesktopXR::CinemaScale() const
{
  static const float kScale[3] = { 0.55f, 0.75f, 1.0f };   // of the game's own field of view
  return kScale[std::clamp(m_config.cinemaSize, 0, 2)];
}

void DesktopXR::DrawCinemaInEye(const New3D::VREye& eye)
{
  // 3D cinema: each eye its own view; otherwise both eyes the same picture
  const int v = (FlatViewCount() == 2 && m_cinemaValid[1] && &eye == &m_curEye[1]) ? 1 : 0;
  if (!m_cinemaValid[v])
    return;
  float proj[16];
  EyeProjection(eye, proj);
  const float d = kCinemaDistance * m_config.worldUnitsPerMeter;
  const float k = CinemaScale();
  const float model[16] = { d * k, 0, 0, 0,  0, d * k, 0, 0,  0, 0, d, 0,  0, 0, 0, 1 };
  float vm[16], mvp[16];
  New3D::VRMul4(eye.view, model, vm);
  New3D::VRMul4(proj, vm, mvp);
  glBindFramebuffer(GL_FRAMEBUFFER, eye.fbo);
  glViewport(0, 0, eye.width, eye.height);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glBindVertexArray(m_vao);
  glUseProgram(m_quadProgram);
  glUniformMatrix4fv(m_uMvp, 1, GL_FALSE, mvp);
  float sL, sR, sT, sB;
  CinemaRect(sL, sR, sT, sB);
  if (m_config.cinemaGlow > 0) {
    // the glow: a bigger quad around the screen, added onto the dark room
    const float w = sR - sL, hgt = sT - sB, pad = 0.42f * hgt;
    glUniform4f(m_uRect, sL - pad, sR + pad, sT + pad, sB - pad);
    const float W = w + 2.f * pad, H = hgt + 2.f * pad;
    glUniform4f(m_uSubUv, pad / W, pad / H, (pad + w) / W, (pad + hgt) / H);
    glUniform2f(m_uGlow, m_config.cinemaGlow == 1 ? 0.5f : 1.0f, w / hgt);
    glUniform1i(m_uMode, 5);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(m_uTex, 0);
    glBindTexture(GL_TEXTURE_2D, m_cinemaTex[v]);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }
  if (m_config.cinemaBezel > 0 && m_bezelProgram) {
    DrawCinemaBezel(mvp, m_cinemaTex[v]);
    glBindVertexArray(m_vao);
    glUseProgram(m_quadProgram);
    glUniformMatrix4fv(m_uMvp, 1, GL_FALSE, mvp);
  }
  glUniform4f(m_uRect, sL, sR, sT, sB);
  glUniform1i(m_uMode, 3);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(m_uTex, 0);
  glBindTexture(GL_TEXTURE_2D, m_cinemaTex[v]);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindTexture(GL_TEXTURE_2D, 0);
  if (m_hudPointers)
    DrawPointerRings(mvp, sL, sR, sT, sB);
  glDisable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBindVertexArray(0);
  glUseProgram(0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

int DesktopXR::EffectiveGunMode() const
{
  if (m_gunMode == 2 && !CustomGunLoaded())
    return 1;   // no custom model: fall back to the game's gun
  if (m_gunMode == 3 && !m_gameGunFileLoaded)
    return 1;   // no arcade gun file: the game's gun
  if (m_gunMode == 1 && New3D::g_vrHandModel.address == 0 && m_gameGunFileLoaded)
    return 3;   // no gun in the game's ROM: the arcade gun file stands in
  return m_gunMode;
}

bool DesktopXR::HandMatrix(int h, float m[16]) const
{
  // Controller aim pose in the same space and units as the eye views (see ComputeEye):
  // rotation as is, position in world units.
  if (h < 0 || h > 1 || !m_handPoseValid[h])
    return false;
  const float s = m_config.worldUnitsPerMeter;
  XrVector3f pos = m_handPose[h].position;
  if (!m_config.positionalTracking)
    pos = {pos.x - m_headPose.position.x, pos.y - m_headPose.position.y, pos.z - m_headPose.position.z};
  float r[9];
  QuatToMat3(m_handPose[h].orientation, r);
  for (int col = 0; col < 3; col++)
    for (int row = 0; row < 3; row++)
      m[col * 4 + row] = r[col * 3 + row];   // QuatToMat3 is column-major
  m[3] = m[7] = m[11] = 0.f;
  m[12] = pos.x * s; m[13] = pos.y * s; m[14] = pos.z * s; m[15] = 1.f;
  return true;
}

int DesktopXR::HandPoses(New3D::VRHandPose* out, int max) const
{
  // The game's gun inside the 3D scene: only during play, never in cinema mode (the VR layer
  // draws the guns itself there) and not when the custom model is chosen.
  // Not any more: the VR layer always draws the guns itself, after the 2D layers (score, text),
  // so the gun in the hand is in front of them, and with the same look in play, menus and cinema.
  (void)out; (void)max;
  return 0;
}

static void UploadGun(GLuint& vao, GLuint& vbo, GLsizei& count, const std::vector<float>& v, int stride = 9)
{
  if (!vao) glGenVertexArrays(1, &vao);
  if (!vbo) glGenBuffers(1, &vbo);
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(float)), v.empty() ? nullptr : v.data(), GL_STATIC_DRAW);
  for (int a = 0; a < 3; a++) {
    glEnableVertexAttribArray(a);
    glVertexAttribPointer(a, 3, GL_FLOAT, GL_FALSE, stride * sizeof(float), (const void*)(a * 3 * sizeof(float)));
  }
  if (stride >= 11) {
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride * sizeof(float), (const void*)(9 * sizeof(float)));
  } else {
    glDisableVertexAttribArray(3);
    glVertexAttrib2f(3, 0.f, 0.f);
  }
  if (stride >= 14) {
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, stride * sizeof(float), (const void*)(11 * sizeof(float)));
  } else {
    glDisableVertexAttribArray(4);
    glVertexAttrib3f(4, 0.f, 0.f, 0.f);
  }
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  count = (GLsizei)(v.size() / stride);
}

void DesktopXR::DrawHandGuns(const New3D::VREye& eye)
{
  const int mode = EffectiveGunMode();
  // Shown from the moment the game starts (title, menus, cutscenes, play), always drawn here,
  // after the 2D layers.
  if (!m_gunProgram || mode == 0 || !m_gunHands)
    return;
  const bool gameFile = mode == 3;
  const int slot = mode == 2 ? 1 : (gameFile ? 2 : 0);
  if (slot == 2 && !m_gameGunFile.empty()) {
    UploadGun(m_gunVao[2], m_gunVbo[2], m_gunCount[2], m_gameGunFile);
    std::vector<float>().swap(m_gameGunFile);
  }
  if (slot == 1 && !m_customGun.empty()) {
    UploadGun(m_gunVao[1], m_gunVbo[1], m_gunCount[1], m_customGun);
    std::vector<float>().swap(m_customGun);
  }
  if (slot == 0 && m_gunVersion != New3D::g_vrHandMesh.version) {
    UploadGun(m_gunVao[0], m_gunVbo[0], m_gunCount[0], New3D::g_vrHandMesh.verts, 14);
    // one texture per textured part, from the game's texture RAM (see New3D::VRAddHandModels)
    for (auto& p : m_gunParts)
      if (p.tex) glDeleteTextures(1, &p.tex);
    m_gunParts.clear();
    for (const auto& src : New3D::g_vrHandMesh.parts) {
      GunPart p;
      p.first = src.first;
      p.count = src.count;
      p.alphaTest = src.alphaTest;
      if (!src.rgba.empty()) {
        glGenTextures(1, &p.tex);
        glBindTexture(GL_TEXTURE_2D, p.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, src.texWidth, src.texHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, src.rgba.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, src.mirrorU ? GL_MIRRORED_REPEAT : GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, src.mirrorV ? GL_MIRRORED_REPEAT : GL_REPEAT);
        glBindTexture(GL_TEXTURE_2D, 0);
      }
      m_gunParts.push_back(p);
    }
    m_gunVersion = New3D::g_vrHandMesh.version;
  }
  if (m_gunCount[slot] <= 0)
    return;

  if (!m_gunDepth || m_gunDepthW != eye.width || m_gunDepthH != eye.height) {
    if (!m_gunDepth) glGenRenderbuffers(1, &m_gunDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, m_gunDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, eye.width, eye.height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    m_gunDepthW = eye.width;
    m_gunDepthH = eye.height;
  }
  float proj[16], pv[16];
  EyeProjection(eye, proj);
  New3D::VRMul4(proj, eye.view, pv);
  // eye position in world space: -R^T t of the view matrix
  const float* V = eye.view;
  const float ex = -(V[0] * V[12] + V[1] * V[13] + V[2] * V[14]);
  const float ey = -(V[4] * V[12] + V[5] * V[13] + V[6] * V[14]);
  const float ez = -(V[8] * V[12] + V[9] * V[13] + V[10] * V[14]);
  const int v = (FlatViewCount() == 2 && m_cinemaValid[1] && &eye == &m_curEye[1]) ? 1 : 0;
  const bool useScreen = m_config.cinema && m_cinemaValid[v];

  glBindFramebuffer(GL_FRAMEBUFFER, eye.fbo);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_gunDepth);
  glViewport(0, 0, eye.width, eye.height);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDepthMask(GL_TRUE);
  glClearDepth(1.0);
  glClear(GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glUseProgram(m_gunProgram);
  glUniform3f(m_uGunEye, ex, ey, ez);
  glUniform1f(m_uGunUseScreen, useScreen ? 1.f : 0.f);
  if (slot == 1)
    glUniform2f(m_uGunSpec, m_customSpec, m_customShine);
  else if (slot == 2)
    glUniform2f(m_uGunSpec, m_gameFileSpec, m_gameFileShine);
  else
    glUniform2f(m_uGunSpec, 0.45f, 32.f);   // the game's guns (from the ROM): glossy, with soft highlights
  // The game gun from a file comes in the arcade's two gun colours (menu: Gun colour).
  if (slot == 2)
    glUniform4f(m_uGunTint, m_gunColour == 1 ? 0.88f : 0.07f, m_gunColour == 1 ? 0.07f : 0.26f,
                m_gunColour == 1 ? 0.05f : 0.92f, 1.f);
  else
    glUniform4f(m_uGunTint, 0.f, 0.f, 0.f, 0.f);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, useScreen ? m_cinemaTex[v] : 0);
  glUniform1i(m_uGunScreen, 0);
  glUniform1i(m_uGunTex, 1);
  glUniform2f(m_uGunTextured, 0.f, 0.f);
  glUniform1f(m_uGunCull, slot == 0 ? 1.f : 0.f);
  glBindVertexArray(m_gunVao[slot]);
  const float s = m_config.worldUnitsPerMeter;
  for (int h = 0; h < 2; h++) {
    float pose[16];
    if (!(m_gunHands & (1 << h)) || !HandMatrix(h, pose))
      continue;
    for (int i = 0; i < 12; i++)
      if (i % 4 != 3) pose[i] *= s;   // the meshes are in metres
    float mvp[16];
    New3D::VRMul4(pv, pose, mvp);
    glUniformMatrix4fv(m_uGunMvp, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(m_uGunModel, 1, GL_FALSE, pose);
    if (slot == 0 && !m_gunParts.empty()) {
      glActiveTexture(GL_TEXTURE1);
      for (const auto& p : m_gunParts) {
        glBindTexture(GL_TEXTURE_2D, p.tex);
        glUniform2f(m_uGunTextured, p.tex ? 1.f : 0.f, p.alphaTest ? 1.f : 0.f);
        glDrawArrays(GL_TRIANGLES, p.first, p.count);
      }
      glBindTexture(GL_TEXTURE_2D, 0);
      glActiveTexture(GL_TEXTURE0);
      glUniform2f(m_uGunTextured, 0.f, 0.f);
    } else {
      glDrawArrays(GL_TRIANGLES, 0, m_gunCount[slot]);
    }
  }
  glBindVertexArray(0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
  glDisable(GL_DEPTH_TEST);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  m_gunDepthValid = true;
}

bool DesktopXR::LoadCustomGun(const std::string& objPath, int slot)
{
  // Minimal OBJ: v, vn, f (v, v/vt, v//vn, v/vt/vn; polygons as fans), colours from the
  // Kd of each usemtl in the mtllib next to it. Units: metres, grip at the origin, barrel -Z, +Y up.
  FILE* f = fopen(objPath.c_str(), "rb");
  if (!f)
    return false;
  std::string dir = objPath;
  const size_t slash = dir.find_last_of("/\\");
  dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
  std::vector<float> pos, nrm, out;
  std::vector<std::pair<std::string, std::array<float, 3>>> mats;
  float ks = -1.f, nsh = -1.f;
  std::array<float, 3> kd = {0.5f, 0.5f, 0.5f};
  auto loadMtl = [&](const std::string& name) {
    FILE* m = fopen((dir + name).c_str(), "rb");
    if (!m) return;
    char line[512], cur[256] = "";
    while (fgets(line, sizeof(line), m)) {
      float r, g, b;
      if (sscanf(line, " newmtl %255s", cur) == 1)
        mats.push_back({cur, {0.5f, 0.5f, 0.5f}});
      else if (sscanf(line, " Kd %f %f %f", &r, &g, &b) == 3 && !mats.empty())
        mats.back().second = {r, g, b};
      else if (sscanf(line, " Ks %f %f %f", &r, &g, &b) == 3 && ks < 0.f)
        ks = (r + g + b) / 3.f;   // one value for the whole model (the first material's)
      else if (sscanf(line, " Ns %f", &r) == 1 && nsh < 0.f)
        nsh = r;
    }
    fclose(m);
  };
  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    float x, y, z;
    char name[256];
    if (p[0] == 'v' && p[1] == ' ' && sscanf(p + 2, "%f %f %f", &x, &y, &z) == 3) {
      pos.insert(pos.end(), {x, y, z});
    } else if (p[0] == 'v' && p[1] == 'n' && sscanf(p + 3, "%f %f %f", &x, &y, &z) == 3) {
      nrm.insert(nrm.end(), {x, y, z});
    } else if (sscanf(p, "mtllib %255s", name) == 1) {
      loadMtl(name);
    } else if (sscanf(p, "usemtl %255s", name) == 1) {
      for (auto& m : mats)
        if (m.first == name) kd = m.second;
    } else if (p[0] == 'f' && p[1] == ' ') {
      int vi[64], ni[64], n = 0;
      char* q = p + 2;
      while (*q && n < 64) {
        while (*q == ' ' || *q == '\t') q++;
        if (!*q || *q == '\r' || *q == '\n') break;
        int a = (int)strtol(q, &q, 10), c = 0;
        if (*q == '/') {
          q++;
          if (*q != '/') strtol(q, &q, 10);
          if (*q == '/') { q++; c = (int)strtol(q, &q, 10); }
        }
        while (*q && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n') q++;
        const int nv = (int)pos.size() / 3, nn = (int)nrm.size() / 3;
        vi[n] = a < 0 ? nv + a : a - 1;
        ni[n] = c < 0 ? nn + c : c - 1;
        if (vi[n] < 0 || vi[n] >= nv) { n = 0; break; }
        if (ni[n] >= nn) ni[n] = -1;
        n++;
      }
      for (int k = 1; k + 1 < n; k++) {
        const int t[3] = {0, k, k + 1};
        const float* A = &pos[vi[0] * 3]; const float* B = &pos[vi[k] * 3]; const float* C = &pos[vi[k + 1] * 3];
        float e1[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, e2[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
        float fn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float l = std::sqrt(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
        if (l > 0.f) { fn[0] /= l; fn[1] /= l; fn[2] /= l; }
        for (int c = 0; c < 3; c++) {
          const float* P = &pos[vi[t[c]] * 3];
          const float* N = ni[t[c]] >= 0 ? &nrm[ni[t[c]] * 3] : fn;
          out.insert(out.end(), {P[0], P[1], P[2], N[0], N[1], N[2], kd[0], kd[1], kd[2]});
        }
      }
    }
  }
  fclose(f);
  if (out.empty())
    return false;
  if (slot == 2) {
    m_gameGunFile.swap(out);
    m_gameFileSpec = ks >= 0.f ? std::min(ks, 1.f) : 0.35f;
    m_gameFileShine = nsh > 0.f ? std::min(nsh, 500.f) : 40.f;
    m_gameGunFileLoaded = true;
    m_gunCount[2] = 0;
    return true;
  }
  m_customGun.swap(out);
  m_customSpec = ks >= 0.f ? std::min(ks, 1.f) : 0.2f;
  m_customShine = nsh > 0.f ? std::min(nsh, 500.f) : 14.f;
  m_gunCount[1] = 0;
  return true;
}

void DesktopXR::DrawCinemaBezel(const float* mvp, GLuint tex)
{
  // Model space of the cinema screen: the picture is at z = -1 within CinemaRect()
  // (x, y are scaled by distance x size, z by distance). The bevel walls run from the picture's
  // edge to the front of the frame, which sits a little nearer to the viewer.
  float L, R, T, B;
  CinemaRect(L, R, T, B);
  const float w = R - L, h = T - B;
  const float bevel = 0.10f * h;         // width of the sloping inner walls (the recess)
  const float front = 0.08f * h;         // width of the flat front of the cabinet
  const float k = CinemaScale();
  const float zf = -1.0f + bevel * k;    // walls at about 45 degrees (x, y carry the extra size factor k)
  const float bs = bevel / h, fs = (bevel + front) / h;          // in screen heights
  const float bx = bs / (w / h), fx = fs / (w / h);              // ... and in screen widths
  glUseProgram(m_bezelProgram);
  glBindVertexArray(m_vao);
  glUniformMatrix4fv(m_uBzMvp, 1, GL_FALSE, mvp);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);
  glUniform1i(m_uBzTex, 0);
  glUniform2f(m_uBzAspect, w / h, bs);
  glUniform1f(m_uBzLight, 1.0f);
  auto quad = [&](const float (&p)[4][3], const float (&s)[4][2], int kind, float shade) {
    glUniform3fv(m_uBzPos, 4, &p[0][0]);
    glUniform2fv(m_uBzSu, 4, &s[0][0]);
    glUniform1i(m_uBzKind, kind);
    glUniform1f(m_uBzShade, shade);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  };
  // corners: inner (picture edge, z = -1), outer (front edge of the walls, z = zf), frame (z = zf)
  const float iL = L, iR = R, iT = T, iB = B;
  const float oL = L - bevel, oR = R + bevel, oT = T + bevel, oB = B - bevel;
  const float fL = oL - front, fR = oR + front, fT = oT + front, fB = oB - front;
  // strip order: 0 = a-inner, 1 = b-inner, 2 = a-outer, 3 = b-outer
  { const float p[4][3] = {{iL,iB,-1},{iL,iT,-1},{oL,oB,zf},{oL,oT,zf}}; const float s[4][2] = {{0,1},{0,0},{-bx,1+bs},{-bx,-bs}}; quad(p, s, 0, 0.95f); }   // left
  { const float p[4][3] = {{iR,iT,-1},{iR,iB,-1},{oR,oT,zf},{oR,oB,zf}}; const float s[4][2] = {{1,0},{1,1},{1+bx,-bs},{1+bx,1+bs}}; quad(p, s, 0, 0.95f); } // right
  { const float p[4][3] = {{iL,iT,-1},{iR,iT,-1},{oL,oT,zf},{oR,oT,zf}}; const float s[4][2] = {{0,0},{1,0},{-bx,-bs},{1+bx,-bs}}; quad(p, s, 0, 0.75f); }  // top
  { const float p[4][3] = {{iR,iB,-1},{iL,iB,-1},{oR,oB,zf},{oL,oB,zf}}; const float s[4][2] = {{1,1},{0,1},{1+bx,1+bs},{-bx,1+bs}}; quad(p, s, 0, 1.15f); } // bottom
  // front of the frame: four flat bands
  { const float p[4][3] = {{oL,oB,zf},{oL,oT,zf},{fL,fB,zf},{fL,fT,zf}}; const float s[4][2] = {{-bx,1+bs},{-bx,-bs},{-fx,1+fs},{-fx,-fs}}; quad(p, s, 1, 1.f); }
  { const float p[4][3] = {{oR,oT,zf},{oR,oB,zf},{fR,fT,zf},{fR,fB,zf}}; const float s[4][2] = {{1+bx,-bs},{1+bx,1+bs},{1+fx,-fs},{1+fx,1+fs}}; quad(p, s, 1, 1.f); }
  { const float p[4][3] = {{oL,oT,zf},{oR,oT,zf},{fL,fT,zf},{fR,fT,zf}}; const float s[4][2] = {{-bx,-bs},{1+bx,-bs},{-fx,-fs},{1+fx,-fs}}; quad(p, s, 1, 1.f); }
  { const float p[4][3] = {{oR,oB,zf},{oL,oB,zf},{fR,fB,zf},{fL,fB,zf}}; const float s[4][2] = {{1+bx,1+bs},{-bx,1+bs},{1+fx,1+fs},{-fx,1+fs}}; quad(p, s, 1, 1.f); }
  glBindTexture(GL_TEXTURE_2D, 0);
}

void DesktopXR::EndFrame()
{
  if (!m_frameBegun)
    return;

  for (auto& e : m_eyes)
    ReleaseImage(e);
  ReleaseImage(m_hud);

  XrCompositionLayerProjectionView projViews[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                 {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  const XrCompositionLayerBaseHeader* layers[2];
  uint32_t layerCount = 0;

  if (m_frameState.shouldRender && m_viewsValid && m_eyeDrawn[0] && m_eyeDrawn[1]) {
    for (int eye = 0; eye < 2; eye++) {
      projViews[eye].pose = m_views[eye].pose;
      projViews[eye].fov = m_views[eye].fov;
      projViews[eye].subImage.swapchain = m_eyes[eye].handle;
      projViews[eye].subImage.imageRect = {{0, 0}, {m_eyes[eye].width, m_eyes[eye].height}};
      projViews[eye].subImage.imageArrayIndex = 0;
    }
    proj.space = m_localSpace;
    proj.viewCount = 2;
    proj.views = projViews;
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
  }

  if (m_frameState.shouldRender && m_hud.releasedOnce && !m_config.hudInEye) {
    // Exactly over the game's original screen frustum, so 2D overlays line up with the 3D world.
    const float d = m_config.hudDistance;
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = m_localSpace;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = m_hud.handle;
    quad.subImage.imageRect = {{0, 0}, {m_hud.width, m_hud.height}};
    quad.pose.orientation = {0, 0, 0, 1};
    quad.pose.position = {d * 0.5f * (m_gameL + m_gameR), d * 0.5f * (m_gameT + m_gameB), -d};
    quad.size = {d * (m_gameR - m_gameL), d * (m_gameT - m_gameB)};
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
  }

  XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
  ei.displayTime = m_frameState.predictedDisplayTime;
  ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  ei.layerCount = layerCount;
  ei.layers = layers;
  Check(xrEndFrame(m_session, &ei), "xrEndFrame");
  m_frameBegun = false;
}

void DesktopXR::SetScreen(const New3D::VRScreenInfo& info)
{
  if (info.cameraTurn >= 0.f)
    m_cameraTurn = info.cameraTurn;   // < 0: a redraw of the same game frame
  m_screen = info;
  if (info.hasFrustum)
    SetGameFrustum(info.left, info.right, info.top, info.bottom);

  // Gameplay (full-height 3D view with a real scene): entered after ~1/3 s, left only after
  // ~1/2 s of something else, so short effects (the hit "static") don't count as leaving it.
  const bool play = info.hasFrustum && !info.subTexture && info.mainModels >= 48 && info.viewEndRow - info.viewFirstRow > 370;
  // A 3D scene this very frame (no delay): the backdrop is continued past the screen only then.
  const bool scene3D = info.hasFrustum && !info.subTexture && info.mainModels >= 48;
  if (scene3D != m_scene3D) {
    m_scene3D = scene3D;
    InfoLog("VR: 3D scene %s (%d models)", scene3D ? "on" : "off", info.mainModels);
  }
  if (play == m_inPlay)
    m_playSwitchFrames = 0;
  else if (++m_playSwitchFrames >= (play ? 20 : 30)) {
    m_inPlay = play;
    m_playSwitchFrames = 0;
    InfoLog("VR: gameplay %s", play ? "on screen" : "off screen");
  }

  // VRHudLock 2: the 2D layer follows the head only while a real 3D scene is on screen (play,
  // cutscenes: hundreds of models). Logos, the title and menus are 2D (at most a handful of
  // models, or small preview viewports) and stay fixed in the room. Switch only after the new
  // state has held for ~1/3 s, so short in-between screens don't flip it back and forth.
  const bool want = info.hasFrustum && !info.subTexture && info.mainModels >= 48;
  if (want == m_hudOnHead)
    m_hudSwitchFrames = 0;
  else if (++m_hudSwitchFrames >= 20) {
    m_hudOnHead = want;
    m_hudSwitchFrames = 0;
    InfoLog("VR: HUD %s (%d models in the 3D view)", want ? "follows the head" : "fixed in the room", info.mainModels);
  }
}

void DesktopXR::DrawHudInEye(const New3D::VREye& eye)
{
  // The game's screen as a world-locked panel at hudDistance, drawn with this eye's own view
  // and projection - the same math as the 3D world, so crosshair and shots line up everywhere.
  const float d = m_config.hudDistance;
  const float n = 0.05f, f = std::max(1000.f, 4.f * d * m_config.worldUnitsPerMeter);
  const float l = n * eye.tanLeft, r = n * eye.tanRight, t = n * eye.tanUp, b = n * eye.tanDown;
  float proj[16] = {};
  proj[0]  = 2 * n / (r - l);
  proj[5]  = 2 * n / (t - b);
  proj[8]  = (r + l) / (r - l);
  proj[9]  = (t + b) / (t - b);
  proj[10] = -(f + n) / (f - n);
  proj[11] = -1;
  proj[14] = -2 * f * n / (f - n);
  // The panel lives in the local (room) space; the eye view converts metres with
  // worldUnitsPerMeter, so scale the panel the same way.
  const float s = d * m_config.worldUnitsPerMeter;
  const float model[16] = { s,0,0,0, 0,s,0,0, 0,0,s,0, 0,0,0,1 };
  float vm[16], mvp[16];
  New3D::VRMul4(eye.view, model, vm);
  New3D::VRMul4(proj, vm, mvp);
  float panelMvp[16];   // where the 2D layers go: the room-fixed screen, or the same screen carried by the head
  std::copy(mvp, mvp + 16, panelMvp);
  const bool headLock = m_config.hudLock == 1 || (m_config.hudLock == 2 && m_hudOnHead);
  if (headLock) {
    float r[9];
    QuatToMat3(m_headPose.orientation, r);
    const float w = m_config.worldUnitsPerMeter;
    const XrVector3f hp = m_config.positionalTracking ? XrVector3f{m_headPose.position.x * w, m_headPose.position.y * w, m_headPose.position.z * w}
                                                      : XrVector3f{0.f, 0.f, 0.f};
    const float head[16] = { r[0], r[1], r[2], 0,  r[3], r[4], r[5], 0,  r[6], r[7], r[8], 0,  hp.x, hp.y, hp.z, 1 };
    float hm[16], hvm[16];
    New3D::VRMul4(head, model, hm);
    New3D::VRMul4(eye.view, hm, hvm);
    New3D::VRMul4(proj, hvm, panelMvp);
  }

  int keepFirst = 0, keepEnd = 384;
  if (m_hideLetterbox && m_screen.viewEndRow - m_screen.viewFirstRow <= 370) {
    keepFirst = m_screen.viewFirstRow + 6;   // the bars reach a few rows into the 3D view
    keepEnd = m_screen.viewEndRow - 6;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, eye.fbo);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glViewport(0, 0, eye.width, eye.height);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // premultiplied
  glBindVertexArray(m_vao);
  glUseProgram(m_quadProgram);
  glUniformMatrix4fv(m_uMvp, 1, GL_FALSE, panelMvp);
  glUniform4f(m_uRect, m_gameL, m_gameR, m_gameT, m_gameB);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(m_uTex, 0);

  if (m_screen.subTexture) {
    glUniform1i(m_uMode, 2);
    glUniform4f(m_uSubUv, m_screen.subUv[0], m_screen.subUv[1], m_screen.subUv[2], m_screen.subUv[3]);
    glBindTexture(GL_TEXTURE_2D, (GLuint)m_screen.subTexture);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  if (m_hudTopTex && m_config.staticFill && m_inPlay) {
    DrawTintFill(eye, panelMvp);
    DrawStaticFill(eye, panelMvp);
  }

  if (m_hudTopTex) {
    glUseProgram(m_quadProgram);
    glBindVertexArray(m_vao);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(m_uFilter, m_config.hudFilter);
    glUniform1i(m_uMode, 1);
    glUniform2f(m_uKeep, (float)keepFirst / 384.f, (float)keepEnd / 384.f);
    glBindTexture(GL_TEXTURE_2D, m_hudTopTex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  glBindTexture(GL_TEXTURE_2D, 0);

  if (m_hudPointers)
    DrawPointerRings(mvp, m_gameL, m_gameR, m_gameT, m_gameB);

  glBindVertexArray(0);
  glUseProgram(0);
  glDisable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void DesktopXR::DrawStaticFill(const New3D::VREye& eye, const float panelMvp[16])
{
  // Measure the game's static once per headset frame (same 2D layer for both eyes).
  const XrTime frameId = m_frameState.predictedDisplayTime;
  if (frameId != m_staticMeasuredFrame) {
    m_staticMeasuredFrame = frameId;
    glBindFramebuffer(GL_FRAMEBUFFER, m_staticFbo);
    glViewport(0, 0, 1, 1);
    glDisable(GL_BLEND);
    glUseProgram(m_staticDetectProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_hudTopTex);
    glUniform1i(glGetUniformLocation(m_staticDetectProgram, "uTex"), 0);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, eye.fbo);
    glViewport(0, 0, eye.width, eye.height);
    glEnable(GL_BLEND);
  }

  // Noise around the screen, on the same plane and with the same pixel size; a new pattern
  // every game frame (60 Hz), like the game's own.
  const float w = m_gameR - m_gameL, h = m_gameT - m_gameB;
  const float cx = 0.5f * (m_gameL + m_gameR), cy = 0.5f * (m_gameT + m_gameB);
  glUseProgram(m_staticProgram);
  glUniformMatrix4fv(m_uStaticMvp, 1, GL_FALSE, panelMvp);
  glUniform4f(m_uStaticRect, cx - 2.5f * w, cx + 2.5f * w, cy + 2.5f * h, cy - 2.5f * h);
  glUniform4f(m_uStaticGame, m_gameL, m_gameR, m_gameT, m_gameB);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_staticTex);
  glUniform1i(m_uStaticAmount, 0);
  glUniform1f(m_uStaticSeed, (float)((frameId / 16666667) % 997));
  glBindVertexArray(m_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void DesktopXR::DrawTintFill(const New3D::VREye& eye, const float panelMvp[16])
{
  if (!m_tintProgram)
    return;
  const XrTime frameId = m_frameState.predictedDisplayTime;
  if (frameId != m_tintMeasuredFrame) {
    m_tintMeasuredFrame = frameId;
    glBindFramebuffer(GL_FRAMEBUFFER, m_tintFbo);
    glViewport(0, 0, 1, 1);
    glDisable(GL_BLEND);
    glUseProgram(m_tintDetectProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_hudTopTex);
    glUniform1i(glGetUniformLocation(m_tintDetectProgram, "uTex"), 0);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, eye.fbo);
    glViewport(0, 0, eye.width, eye.height);
    glEnable(GL_BLEND);
  }
  const float w = m_gameR - m_gameL, h = m_gameT - m_gameB;
  const float cx = 0.5f * (m_gameL + m_gameR), cy = 0.5f * (m_gameT + m_gameB);
  glUseProgram(m_tintProgram);
  glUniformMatrix4fv(m_uTintMvp, 1, GL_FALSE, panelMvp);
  glUniform4f(m_uTintRect, cx - 2.5f * w, cx + 2.5f * w, cy + 2.5f * h, cy - 2.5f * h);
  glUniform4f(m_uTintGame, m_gameL, m_gameR, m_gameT, m_gameB);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_tintTex);
  glUniform1i(m_uTintTex, 0);
  glBindVertexArray(m_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void DesktopXR::SetGameFrustum(float left, float right, float top, float bottom)
{
  if (right - left > 1e-3f && top - bottom > 1e-3f) {
    m_gameL = left;
    m_gameR = right;
    m_gameT = top;
    m_gameB = bottom;
  }
}

// ---------------------------------------------------------------------------------------------
// Rendering targets
// ---------------------------------------------------------------------------------------------

bool DesktopXR::AcquireImage(Swapchain& sc)
{
  if (sc.acquired != UINT32_MAX)
    return true;
  XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  uint32_t index = 0;
  if (XR_FAILED(xrAcquireSwapchainImage(sc.handle, &ai, &index)))
    return false;
  XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wi.timeout = XR_INFINITE_DURATION;
  if (XR_FAILED(xrWaitSwapchainImage(sc.handle, &wi))) {
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(sc.handle, &ri);
    return false;
  }
  sc.acquired = index;
  return true;
}

void DesktopXR::ReleaseImage(Swapchain& sc)
{
  if (sc.acquired == UINT32_MAX)
    return;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xrReleaseSwapchainImage(sc.handle, &ri);
  sc.acquired = UINT32_MAX;
  sc.releasedOnce = true;
}

void DesktopXR::ComputeEye(int eye, New3D::VREye& out) const
{
  const XrView& v = m_views[eye];
  out.tanLeft  = std::tan(v.fov.angleLeft);
  out.tanRight = std::tan(v.fov.angleRight);
  out.tanUp    = std::tan(v.fov.angleUp);
  out.tanDown  = std::tan(v.fov.angleDown);

  XrVector3f pos = v.pose.position;
  if (!m_config.positionalTracking)
    pos = {pos.x - m_headPose.position.x, pos.y - m_headPose.position.y, pos.z - m_headPose.position.z};
  const float s = m_config.worldUnitsPerMeter;
  pos = {pos.x * s, pos.y * s, pos.z * s};

  // view = inverse(eye pose) = [R^T | -R^T * p]
  float r[9];
  QuatToMat3(v.pose.orientation, r);
  float* m = out.view;
  m[0] = r[0]; m[4] = r[1]; m[8]  = r[2];
  m[1] = r[3]; m[5] = r[4]; m[9]  = r[5];
  m[2] = r[6]; m[6] = r[7]; m[10] = r[8];
  m[3] = 0;    m[7] = 0;    m[11] = 0;
  m[12] = -(m[0] * pos.x + m[4] * pos.y + m[8]  * pos.z);
  m[13] = -(m[1] * pos.x + m[5] * pos.y + m[9]  * pos.z);
  m[14] = -(m[2] * pos.x + m[6] * pos.y + m[10] * pos.z);
  m[15] = 1;
}

bool DesktopXR::BeginEye(int eye, New3D::VREye& outEye)
{
  if (eye < 0 || eye > 1 || m_suppressEyes || !m_frameBegun || !m_frameState.shouldRender || !m_viewsValid || m_eyeDrawn[eye])
    return false;
  Swapchain& sc = m_eyes[eye];
  if (!AcquireImage(sc))
    return false;

  ComputeEye(eye, outEye);
  outEye.fbo = sc.fbos[sc.acquired];
  outEye.width = sc.width;
  outEye.height = sc.height;
  m_curEye[eye] = outEye;

  glBindFramebuffer(GL_FRAMEBUFFER, outEye.fbo);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glViewport(0, 0, sc.width, sc.height);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  if (m_config.cinema)
    glClearColor(0.02f, 0.02f, 0.025f, 1.f);   // dark room
  else
    glClearColor(0.f, 0.f, 0.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  // Supermodel relies on a transparent clear colour: New3D clears its layer buffers with it,
  // and an opaque one would paint over the 2D background (menus, title screens).
  glClearColor(0.f, 0.f, 0.f, 0.f);

  if (m_backdropTex && !m_config.cinema)
    DrawBackdrop(outEye);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return true;
}

void DesktopXR::EndEye(int eye)
{
  if (eye < 0 || eye > 1)
    return;
  if (m_eyes[eye].acquired != UINT32_MAX) {
    m_gunDepthValid = false;
    if (m_config.cinema) {
      DrawCinemaInEye(m_curEye[eye]);
      DrawHandGuns(m_curEye[eye]);
    } else {
      if (m_config.hudInEye)
        DrawHudInEye(m_curEye[eye]);
      DrawHandGuns(m_curEye[eye]);
      DrawVignette(m_curEye[eye]);
    }
    if (m_menuOpen && m_menuTex)
      DrawMenuInEye(m_curEye[eye]);
  }
  ReleaseImage(m_eyes[eye]);
  m_eyeDrawn[eye] = true;
}

void DesktopXR::DrawEyesIfMissing()
{
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  for (int eye = 0; eye < 2; eye++) {
    if (m_eyeDrawn[eye])
      continue;
    New3D::VREye e;
    if (BeginEye(eye, e))
      EndEye(eye);
  }
  if (scissor)
    glEnable(GL_SCISSOR_TEST);
}

void DesktopXR::DrawBackdrop(const New3D::VREye& eye)
{
  // Rotation only (no translation): the backdrop sits at infinity behind the 3D world.
  float rot[16];
  std::memcpy(rot, eye.view, sizeof(rot));
  rot[12] = rot[13] = rot[14] = 0.f;

  const float n = 0.1f, f = 10.f;
  const float l = n * eye.tanLeft, r = n * eye.tanRight, t = n * eye.tanUp, b = n * eye.tanDown;
  float proj[16] = {};
  proj[0]  = 2 * n / (r - l);
  proj[5]  = 2 * n / (t - b);
  proj[8]  = (r + l) / (r - l);
  proj[9]  = (t + b) / (t - b);
  proj[10] = -(f + n) / (f - n);
  proj[11] = -1;
  proj[14] = -2 * f * n / (f - n);
  float mvp[16];
  New3D::VRMul4(proj, rot, mvp);

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glUseProgram(m_quadProgram);
  glUniformMatrix4fv(m_uMvp, 1, GL_FALSE, mvp);
  glUniform1i(m_uMode, 0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_backdropTex);
  glUniform1i(m_uTex, 0);
  GLint wrapS = GL_CLAMP_TO_EDGE, wrapT = GL_CLAMP_TO_EDGE;
  // only while a 3D scene is on screen: 2D-only screens (menus, scores) stay a framed picture
  const bool fill = m_config.backdropFill && m_scene3D;
  if (fill && m_wideTex) {
    // The game's background (sky, distant scenery) only covers its 4:3 screen. Around it: the
    // same background layers drawn wider by the tile generator (the real continuation of the
    // sky left and right, faded above and below), then the screen's own sharp picture on top.
    const float w = m_gameR - m_gameL, h = m_gameT - m_gameB;
    glBindTexture(GL_TEXTURE_2D, m_wideTex);
    glUniform4f(m_uRect, m_gameL + m_wideRect[0] / 496.f * w, m_gameL + m_wideRect[2] / 496.f * w,
                m_gameT - m_wideRect[1] / 384.f * h, m_gameT - m_wideRect[3] / 384.f * h);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, m_backdropTex);
    glUniform4f(m_uRect, m_gameL, m_gameR, m_gameT, m_gameB);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    return;
  }
  if (fill) {
    // No wide background (yet): continue the screen's picture past the edges, mirrored, so the
    // sky doesn't end in black around the screen.
    const float k = 2.5f, w = m_gameR - m_gameL, h = m_gameT - m_gameB;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrapS);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrapT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
    glUniform4f(m_uRect, m_gameL - k * w, m_gameR + k * w, m_gameT + k * h, m_gameB - k * h);
    glUniform4f(m_uUvXform, 1.f + 2.f * k, 1.f + 2.f * k, -k, -k);
  }
  else
    glUniform4f(m_uRect, m_gameL, m_gameR, m_gameT, m_gameB);
  glBindVertexArray(m_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
  if (fill) {
    glUniform4f(m_uUvXform, 1.f, 1.f, 0.f, 0.f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
}

void DesktopXR::DrawHud(GLuint topLayerTex, bool pointers, int keepFirstRow, int keepEndRow,
                        GLuint subTex, const float* subUv)
{
  if (m_config.hudInEye || !m_frameBegun || !m_frameState.shouldRender || m_hudDrawn)
    return;
  if (!AcquireImage(m_hud))
    return;

  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_FRAMEBUFFER, m_hud.fbos[m_hud.acquired]);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glViewport(0, 0, m_hud.width, m_hud.height);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(0.f, 0.f, 0.f, 0.f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBindVertexArray(m_vao);

  static const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // everything here is premultiplied

  if (subTex && subUv) {
    // small 3D viewports (menus), flat on the screen panel, under the 2D top layer
    glUseProgram(m_quadProgram);
    glUniformMatrix4fv(m_uMvp, 1, GL_FALSE, identity);
    glUniform4f(m_uRect, -1.f, 1.f, 1.f, -1.f);
    glUniform1i(m_uMode, 2);
    glUniform4f(m_uSubUv, subUv[0], subUv[1], subUv[2], subUv[3]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, subTex);
    glUniform1i(m_uTex, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  if (topLayerTex) {
    glUseProgram(m_quadProgram);
    glUniformMatrix4fv(m_uMvp, 1, GL_FALSE, identity);
    glUniform4f(m_uRect, -1.f, 1.f, 1.f, -1.f);
    glUniform1i(m_uMode, 1);
    glUniform2f(m_uKeep, (float)keepFirstRow / 384.f, (float)keepEndRow / 384.f);
    glUniform1i(m_uFilter, m_config.hudFilter);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, topLayerTex);
    glUniform1i(m_uTex, 0);
    // The game's (z = -1) quad through an identity transform lands on the near plane.
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  if (pointers) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // premultiplied
    glUseProgram(m_dotProgram);
    glUniform1f(m_uDotAspect, (float)m_hud.width / (float)m_hud.height);
    glUniform1f(m_uDotRadius, 0.03f);
    for (int hand = 0; hand < 2; hand++) {
      if (!PointerHand(hand) || !m_input.onScreen[hand])
        continue;
      glUniform2f(m_uDotCenter, m_input.aimX[hand] * 2.f - 1.f, 1.f - m_input.aimY[hand] * 2.f);
      if (hand == 0) glUniform3f(m_uDotColor, 1.f, 0.25f, 0.2f);
      else           glUniform3f(m_uDotColor, 0.2f, 0.8f, 1.f);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }

  glBindVertexArray(0);
  glUseProgram(0);
  ReleaseImage(m_hud);
  m_hudDrawn = true;
  if (scissor)
    glEnable(GL_SCISSOR_TEST);
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

void DesktopXR::SyncInput()
{
  XrActiveActionSet active{m_actionSet, XR_NULL_PATH};
  XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
  si.countActiveActionSets = 1;
  si.activeActionSets = &active;
  if (xrSyncActions(m_session, &si) != XR_SUCCESS) {
    m_input = DesktopXRInput{};   // not focused (system menu open, etc.)
    return;
  }

  auto getBool = [&](XrAction a, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = sub;
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &gi, &st)) && st.isActive && st.currentState;
  };
  auto getFloat = [&](XrAction a, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = sub;
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
    return (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &gi, &st)) && st.isActive) ? st.currentState : 0.f;
  };

  DesktopXRInput in;
  m_handPoseValid[0] = m_handPoseValid[1] = false;
  in.buttonA = getBool(m_a, XR_NULL_PATH);
  in.buttonB = getBool(m_b, XR_NULL_PATH);
  in.buttonX = getBool(m_x, XR_NULL_PATH);
  in.buttonY = getBool(m_y, XR_NULL_PATH);
  in.menu    = getBool(m_menu, XR_NULL_PATH);

  const float d = m_config.hudDistance;
  for (int h = 0; h < 2; h++) {
    in.trigger[h]    = getFloat(m_trigger, m_hands[h]) > 0.5f;
    in.grip[h]       = getFloat(m_grip, m_hands[h]) > 0.5f;
    in.thumbClick[h] = getBool(m_thumbClick, m_hands[h]);

    // Aim ray -> game screen: intersect with the HUD plane (z = -d) and normalise against
    // the game's frustum at that distance. Matches where the HUD is drawn.
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(m_aimSpaces[h], m_localSpace, m_frameState.predictedDisplayTime, &loc)))
      continue;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & need) != need)
      continue;
    const XrVector3f o = loc.pose.position;
    XrVector3f dir = Rotate(loc.pose.orientation, {0.f, 0.f, -1.f});
    // aim fine-tune: turn the ray a little to the side / up (degrees)
    if (m_config.aimNudgeX != 0.f || m_config.aimNudgeY != 0.f) {
      XrVector3f right = {-dir.z, 0.f, dir.x};   // dir x up, level
      const float rl = std::sqrt(right.x * right.x + right.z * right.z);
      if (rl > 1e-4f) {
        right = {right.x / rl, 0.f, right.z / rl};
        const XrVector3f up = {right.y * dir.z - right.z * dir.y, right.z * dir.x - right.x * dir.z, right.x * dir.y - right.y * dir.x};
        const float tx = std::tan(m_config.aimNudgeX * 0.0174533f), ty = std::tan(m_config.aimNudgeY * 0.0174533f);
        dir = {dir.x + right.x * tx + up.x * ty, dir.y + right.y * tx + up.y * ty, dir.z + right.z * tx + up.z * ty};
        const float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        dir = {dir.x / dl, dir.y / dl, dir.z / dl};
      }
    }
    in.rayValid[h] = true;
    in.rayOrigin[h] = o;
    in.rayDir[h] = dir;
    m_handPose[h] = loc.pose;
    m_handPoseValid[h] = true;
    if (m_menuOpen) {
      // pointer on the menu panel
      const float cs = std::cos(m_menuYaw), sn = std::sin(m_menuYaw);
      const XrVector3f n = {sn, 0.f, cs};                  // panel normal, towards the player
      const float denom = dir.x * n.x + dir.y * n.y + dir.z * n.z;
      if (denom < -1e-4f) {
        const XrVector3f c = m_menuCenter;
        const float t = ((c.x - o.x) * n.x + (c.y - o.y) * n.y + (c.z - o.z) * n.z) / denom;
        if (t > 0.f) {
          const XrVector3f p = {o.x + t * dir.x, o.y + t * dir.y, o.z + t * dir.z};
          const float u = ((p.x - c.x) * cs + (p.z - c.z) * -sn) / kMenuWidth + 0.5f;
          const float v = 0.5f - (p.y - c.y) / kMenuHeight;
          in.menuDist[h] = t;
          in.menuU[h] = u;
          in.menuV[h] = v;
          in.menuHit[h] = u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f;
        }
      }
      // Grip (side button) on the panel: pick the menu up. It follows the ray at the same
      // distance, keeps the grabbed point under the pointer and turns to face the player.
      if (in.grip[h] && !m_prevGrip[h] && m_menuGrabHand < 0 && in.menuHit[h]) {
        m_menuGrabHand = h;
        m_menuGrabDist = in.menuDist[h];
        m_menuGrabX = (in.menuU[h] - 0.5f) * kMenuWidth;
        m_menuGrabY = (0.5f - in.menuV[h]) * kMenuHeight;
        Haptic(h, 0.25f, 0.03f);
      }
      if (m_menuGrabHand == h) {
        if (!in.grip[h]) {
          m_menuGrabHand = -1;
          Haptic(h, 0.15f, 0.02f);
        } else {
          const XrVector3f p = {o.x + m_menuGrabDist * dir.x, o.y + m_menuGrabDist * dir.y, o.z + m_menuGrabDist * dir.z};
          const XrVector3f hp = m_headPose.position;
          const float dx = hp.x - p.x, dz = hp.z - p.z;
          if (dx * dx + dz * dz > 0.04f)
            m_menuYaw = std::atan2(dx, dz);
          const float cs2 = std::cos(m_menuYaw), sn2 = std::sin(m_menuYaw);
          m_menuCenter = {p.x - m_menuGrabX * cs2, p.y - m_menuGrabY, p.z + m_menuGrabX * sn2};
          in.menuHit[h] = false;   // no clicking while carrying the menu
          in.menuGrab[h] = true;
        }
      }
    } else
      m_menuGrabHand = -1;
    m_prevGrip[h] = in.grip[h];
    if (dir.z > -1e-4f)
      continue;
    // In cinema mode the screen is smaller and nearer: aim at it instead.
    const float pd = m_config.cinema ? kCinemaDistance : d;
    const float ps = m_config.cinema ? CinemaScale() : 1.f;
    const float t = (-pd - o.z) / dir.z;
    if (t <= 0.f)
      continue;
    const float px = (o.x + t * dir.x) / (pd * ps);
    const float py = (o.y + t * dir.y) / (pd * ps);
    float L = m_gameL, R = m_gameR, T = m_gameT, B = m_gameB;
    if (m_config.cinema)
      CinemaRect(L, R, T, B);
    const float nx = (px - L) / (R - L);
    const float ny = (T - py) / (T - B);
    in.aimValid[h] = true;
    in.onScreen[h] = nx >= 0.f && nx <= 1.f && ny >= 0.f && ny <= 1.f;
    // Not clamped: with the wide gun calibration the game aims (and draws its crosshair)
    // beyond the original screen, so the whole view in the headset can be shot at.
    in.aimX[h] = nx;
    in.aimY[h] = ny;
  }
  m_input = in;
}
