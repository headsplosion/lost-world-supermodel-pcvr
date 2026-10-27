#include "VRMenu.h"

#include "Pkgs/imgui/imgui.h"
#include "Pkgs/imgui/imgui_impl_opengl3.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace
{
  // All capitals, like the lettering in the title artwork.
  std::string Caps(const char *text)
  {
    std::string t(text);
    for (auto &c : t)
      if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    return t;
  }

  // A row of buttons, the current one highlighted - easier to hit with a laser than a combo box.
  bool Choice(const char *label, int *value, const char *const *names, int count)
  {
    bool changed = false;
    ImGui::TextUnformatted(Caps(label).c_str());
    ImGui::PushID(label);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float w = (avail - ImGui::GetStyle().ItemSpacing.x * (count - 1)) / (float)count;
    for (int i = 0; i < count; i++)
    {
      if (i)
        ImGui::SameLine();
      const bool on = (*value == i);
      if (on)
      {
        // the selected choice: a lighter InGen panel with a lime outline
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.40f, 0.19f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.06f, 0.99f, 0.06f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
      }
      if (ImGui::Button(Caps(names[i]).c_str(), ImVec2(w, 0)) && !on)
      {
        *value = i;
        changed = true;
      }
      if (on)
      {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
      }
    }
    ImGui::PopID();
    return changed;
  }

  bool Toggle(const char *label, bool *value)
  {
    static const char *const kOnOff[] = { "Off", "On" };
    int v = *value ? 1 : 0;
    if (!Choice(label, &v, kOnOff, 2))
      return false;
    *value = (v != 0);
    return true;
  }

  // - value + buttons, so it can be set precisely with a pointer
  bool Stepper(const char *label, float *value, float step, float lo, float hi, const char *fmt)
  {
    bool changed = false;
    ImGui::TextUnformatted(Caps(label).c_str());
    ImGui::PushID(label);
    const float bw = 110.0f;
    if (ImGui::Button("-", ImVec2(bw, 0)))
    {
      *value = std::max(lo, *value - step);
      changed = true;
    }
    ImGui::SameLine();
    char text[32];
    std::snprintf(text, sizeof(text), fmt, *value);
    for (char *c = text; *c; c++)
      if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
    const float tw = ImGui::GetContentRegionAvail().x - bw - ImGui::GetStyle().ItemSpacing.x;
    ImGui::Button(text, ImVec2(tw, 0));
    ImGui::SameLine();
    if (ImGui::Button("+", ImVec2(bw, 0)))
    {
      *value = std::min(hi, *value + step);
      changed = true;
    }
    ImGui::PopID();
    if (changed)   // keep it on the step grid (no 0.30000001)
      *value = (float)((int)(*value / step + (*value >= 0 ? 0.5f : -0.5f))) * step;
    return changed;
  }
}

VRMenu::~VRMenu()
{
  Shutdown();
}

namespace
{
  // Uncompressed 32-bit TGA (what the menu artwork is saved as) -> RGBA, rows top-first.
  bool LoadTGA(const std::string &path, std::vector<unsigned char> &rgba, int &w, int &h)
  {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
      return false;
    unsigned char hdr[18];
    bool ok = std::fread(hdr, 1, 18, f) == 18 && hdr[2] == 2 && hdr[16] == 32;
    if (ok)
    {
      w = hdr[12] | (hdr[13] << 8);
      h = hdr[14] | (hdr[15] << 8);
      ok = w > 0 && h > 0 && w <= 4096 && h <= 4096;
    }
    if (ok)
    {
      std::fseek(f, 18 + hdr[0], SEEK_SET);
      std::vector<unsigned char> bgra((size_t)w * h * 4);
      ok = std::fread(bgra.data(), 1, bgra.size(), f) == bgra.size();
      if (ok)
      {
        const bool topFirst = (hdr[17] & 0x20) != 0;
        rgba.resize(bgra.size());
        for (int y = 0; y < h; y++)
        {
          const unsigned char *src = &bgra[(size_t)(topFirst ? y : h - 1 - y) * w * 4];
          unsigned char *dst = &rgba[(size_t)y * w * 4];
          for (int x = 0; x < w; x++)
          {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
          }
        }
      }
    }
    std::fclose(f);
    return ok;
  }

  bool FileExists(const std::string &path)
  {
    if (FILE *f = std::fopen(path.c_str(), "rb"))
    {
      std::fclose(f);
      return true;
    }
    return false;
  }
}

bool VRMenu::Init(const std::string &assetDir)
{
  if (m_ready)
    return true;

  // Artwork: the frame picture. Its inner rectangle (where the options go) is found from the
  // companion VRMenuFrame.txt ("x0 y0 x1 y1" in picture pixels) or defaults to the middle.
  std::vector<unsigned char> frame;
  int fw = 0, fh = 0;
  const bool haveFrame = LoadTGA(assetDir + "VRMenuFrame.tga", frame, fw, fh);
  if (haveFrame)
  {
    const float scale = 1600.0f / (float)fw;   // render the menu sharper than the picture
    m_w = 1600;
    m_h = (int)(fh * scale + 0.5f);
    float r[4] = { fw * 0.06f, fh * 0.27f, fw * 0.94f, fh * 0.93f };
    if (FILE *f = std::fopen((assetDir + "VRMenuFrame.txt").c_str(), "r"))
    {
      float v[4];
      if (std::fscanf(f, "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]) == 4)
        for (int i = 0; i < 4; i++) r[i] = v[i];
      std::fclose(f);
    }
    for (int i = 0; i < 4; i++)
      m_inner[i] = r[i] * scale;
  }
  else
  {
    m_w = 960;
    m_h = 720;
    m_inner[0] = 0; m_inner[1] = 0; m_inner[2] = (float)m_w; m_inner[3] = (float)m_h;
  }

  ImGuiContext *previous = ImGui::GetCurrentContext();
  ImGuiContext *ctx = ImGui::CreateContext();
  m_ctx = ctx;
  ImGui::SetCurrentContext(ctx);
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;   // no imgui.ini next to the game
  io.LogFilename = nullptr;
  io.DisplaySize = ImVec2((float)m_w, (float)m_h);
  io.DisplayFramebufferScale = ImVec2((float)kSupersample, (float)kSupersample);   // fonts are rasterised at this density too
  const std::string fontPath = assetDir + "VRMenuFont.ttf";
  ImFont *font = nullptr;
  if (FileExists(fontPath))
  {
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    font = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), haveFrame ? 28.0f : 22.0f, &cfg);
  }
  if (!font)
  {
    ImFontConfig cfg;
    cfg.SizePixels = 30.0f;
    io.Fonts->AddFontDefault(&cfg);
  }

  // The Lost World's InGen STAGE SELECT screen: lime letters (RGB 14, 253, 14) on deep green
  // panels with teal edges; what is selected is a lighter panel outlined in lime.
  const ImVec4 text(0.06f, 0.99f, 0.06f, 1.0f), sel(0.0f, 0.40f, 0.19f, 1.0f), selHi(0.06f, 0.99f, 0.06f, 1.0f);
  const ImVec4 panel(0.0f, 0.13f, 0.06f, 0.94f), panelHover(0.0f, 0.25f, 0.12f, 0.96f), teal(0.0f, 0.76f, 0.41f, 0.75f);
  ImGuiStyle &style = ImGui::GetStyle();
  ImGui::StyleColorsDark(&style);
  style.WindowPadding = ImVec2(30, 22);
  style.FramePadding = ImVec2(12, 11);
  style.ItemSpacing = ImVec2(12, 9);
  style.FrameRounding = 2.0f;
  style.TabRounding = 2.0f;
  style.WindowRounding = 0.0f;
  style.WindowBorderSize = 0.0f;
  style.FrameBorderSize = 1.0f;
  style.Colors[ImGuiCol_Text] = text;
  style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.10f, 0.42f, 0.16f, 1.0f);
  style.Colors[ImGuiCol_WindowBg] = haveFrame ? ImVec4(0, 0, 0, 0) : ImVec4(0.0f, 0.05f, 0.02f, 0.96f);
  style.Colors[ImGuiCol_Border] = teal;
  style.Colors[ImGuiCol_Button] = panel;
  style.Colors[ImGuiCol_ButtonHovered] = panelHover;
  style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.0f, 0.55f, 0.26f, 1.0f);   // pressed: lime text stays readable
  style.Colors[ImGuiCol_Tab] = panel;
  style.Colors[ImGuiCol_TabHovered] = panelHover;
  style.Colors[ImGuiCol_TabSelected] = sel;
  style.Colors[ImGuiCol_TabSelectedOverline] = selHi;
  style.Colors[ImGuiCol_Separator] = teal;

  const bool ok = ImGui_ImplOpenGL3_Init("#version 330");

  glGenTextures(1, &m_tex);
  glBindTexture(GL_TEXTURE_2D, m_tex);
  // The menu is drawn at kSupersample x its layout size and mipmapped, so it stays sharp up close
  // and doesn't shimmer when it covers fewer headset pixels than it has.
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_w * kSupersample, m_h * kSupersample, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glGenerateMipmap(GL_TEXTURE_2D);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  if (GLEW_EXT_texture_filter_anisotropic)
  {
    GLfloat maxAniso = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, std::min(8.0f, maxAniso));
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (haveFrame)
  {
    glGenTextures(1, &m_frameTex);
    glBindTexture(GL_TEXTURE_2D, m_frameTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, frame.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &m_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_tex, 0);
  const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  ImGui::SetCurrentContext(previous);
  m_ready = ok && complete;
  if (!m_ready)
    Shutdown();
  return m_ready;
}

void VRMenu::Shutdown()
{
  if (m_ctx)
  {
    ImGuiContext *previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext((ImGuiContext *)m_ctx);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext((ImGuiContext *)m_ctx);
    ImGui::SetCurrentContext(previous == m_ctx ? nullptr : previous);
    m_ctx = nullptr;
  }
  if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
  if (m_tex) glDeleteTextures(1, &m_tex);
  if (m_frameTex) glDeleteTextures(1, &m_frameTex);
  m_fbo = m_tex = m_frameTex = 0;
  m_ready = false;
}

VRMenuResult VRMenu::Render(VRSettings &s, float pointerU, float pointerV, bool valid, bool press, double dt)
{
  VRMenuResult r;
  if (!m_ready)
    return r;

  ImGuiContext *previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext((ImGuiContext *)m_ctx);
  ImGuiIO &io = ImGui::GetIO();
  io.DeltaTime = (float)std::clamp(dt, 1.0 / 240.0, 0.25);
  io.DisplaySize = ImVec2((float)m_w, (float)m_h);
  io.DisplayFramebufferScale = ImVec2((float)kSupersample, (float)kSupersample);   // fonts are rasterised at this density too
  if (valid)
    io.AddMousePosEvent(pointerU * m_w, pointerV * m_h);
  else
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
  io.AddMouseButtonEvent(0, valid && press);

  ImGui_ImplOpenGL3_NewFrame();
  ImGui::NewFrame();
  if (m_frameTex)
  {
    // the artwork, and a dark glass over its inner rectangle so the options read clearly
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    bg->AddImage(ImTextureRef((ImTextureID)(intptr_t)m_frameTex), ImVec2(0, 0), ImVec2((float)m_w, (float)m_h));
    bg->AddRectFilled(ImVec2(m_inner[0], m_inner[1]), ImVec2(m_inner[2], m_inner[3]), IM_COL32(0, 12, 5, 160));
  }
  ImGui::SetNextWindowPos(ImVec2(m_inner[0], m_inner[1]));
  ImGui::SetNextWindowSize(ImVec2(m_inner[2] - m_inner[0], m_inner[3] - m_inner[1]));
  ImGui::Begin("The Lost World VR", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);

  if (!m_frameTex)
  {
    ImGui::TextUnformatted("THE LOST WORLD VR - OPTIONS");
    ImGui::Separator();
  }

  if (ImGui::BeginTabBar("tabs"))
  {
    if (ImGui::BeginTabItem("  COMFORT  "))
    {
      static const char *const kVignette[] = { "Off", "Light", "Medium", "Strong" };
      r.changed |= Choice("Vignette during fast camera turns", &s.vignette, kVignette, 4);
      ImGui::Spacing();
      r.changed |= Toggle("Cinema mode (game on a big screen, no motion)", &s.cinema);
      static const char *const kSize[] = { "Small", "Medium", "Large" };
      ImGui::BeginDisabled(!s.cinema);
      r.changed |= Choice("Cinema screen size", &s.cinemaSize, kSize, 3);
      static const char *const kDepth[] = { "Flat", "Shallow", "Full" };
      r.changed |= Choice("Cinema 3D depth", &s.cinemaDepth, kDepth, 3);
      static const char *const kBezel[] = { "Off", "On" };
      r.changed |= Choice("Screen bezel (a frame that reflects the picture)", &s.cinemaBezel, kBezel, 2);
      static const char *const kGlow[] = { "Off", "Subtle", "Strong" };
      r.changed |= Choice("Screen glow (the picture lights up its surroundings)", &s.cinemaGlow, kGlow, 3);
      ImGui::EndDisabled();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("  GUN  "))
    {
      static const char *const kHand[] = { "Right", "Left" };
      int handIdx = s.gunHand == 2 ? 1 : 0;
      if (Choice("Gun hand", &handIdx, kHand, 2))
      {
        s.gunHand = handIdx == 1 ? 2 : 1;
        r.changed = true;
      }
      {
        // Buttons in menu order: Off, Game (or Rifle + Pistol), Arcade (when its file is
        // there), Custom. Saved values: 0 off, 1 game / rifle, 4 pistol, 3 arcade, 2 custom.
        const char *names[5];
        int values[5];
        int n = 0;
        names[n] = "Off"; values[n++] = 0;
        if (s.gunPistol)
        {
          names[n] = "Rifle";  values[n++] = 1;
          names[n] = "Pistol"; values[n++] = 4;
        }
        else
        {
          names[n] = "Game"; values[n++] = 1;
        }
        if (s.gunColours)
        {
          names[n] = "Arcade"; values[n++] = 3;
        }
        names[n] = "Custom"; values[n++] = 2;
        int idx = -1;
        for (int i = 0; i < n; i++)
          if (values[i] == s.gunModel) idx = i;
        if (idx < 0)
          idx = 1;   // a choice this game doesn't have: shown as its own gun
        if (Choice("Gun model in your hand", &idx, names, n))
        {
          s.gunModel = values[idx];
          r.changed = true;
        }
      }
      if (s.gunColours)
      {
        static const char *const kColour[] = { "Blue", "Red" };
        ImGui::BeginDisabled(s.gunModel != 3);
        r.changed |= Choice("Gun colour", &s.gunColour, kColour, 2);
        ImGui::EndDisabled();
      }
      static const char *const kRecoil[] = { "Off", "Light", "Strong" };
      r.changed |= Choice("Recoil vibration", &s.recoil, kRecoil, 3);
      r.changed |= Stepper("Aim fine-tune: left / right", &s.aimNudgeX, 0.1f, -3.0f, 3.0f, "%+.1f deg");
      r.changed |= Stepper("Aim fine-tune: down / up", &s.aimNudgeY, 0.1f, -3.0f, 3.0f, "%+.1f deg");
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("  SCREEN  "))
    {
      static const char *const kLock[] = { "Fixed", "Follow head", "Auto" };
      int lock = s.hudLock == 0 ? 0 : (s.hudLock == 1 ? 1 : 2);
      if (Choice("Score / timer / text", &lock, kLock, 3))
      {
        s.hudLock = lock;
        r.changed = true;
      }
      static const char *const kFilter[] = { "Soft", "Sharp", "Smooth edges" };
      r.changed |= Choice("2D graphics scaling", &s.hudFilter, kFilter, 3);
      r.changed |= Toggle("Hide player 2 PRESS START during play", &s.hideP2);
      r.changed |= Toggle("Hit static fills the whole view", &s.staticFill);
      {
        // 1.0 = the headset's (or streamer's) recommended eye size; the pixels per eye are shown
        char fmt[64] = "%.1fx";
        if (s.eyeRecW > 0 && s.eyeRecH > 0)
        {
          float sc = s.renderScale;
          if (s.eyeCapW > 0 && s.eyeRecW * sc > (float)s.eyeCapW)
            sc = (float)s.eyeCapW / (float)s.eyeRecW;
          int w = (int)(s.eyeRecW * sc + 0.5f), h = (int)(s.eyeRecH * sc + 0.5f);
          if (s.eyeMaxW > 0) w = std::min(w, s.eyeMaxW);
          if (s.eyeMaxH > 0) h = std::min(h, s.eyeMaxH);
          std::snprintf(fmt, sizeof(fmt), "%%.1fx  -  %d x %d", w, h);
        }
        r.changed |= Stepper("Render resolution, pixels per eye (next start)", &s.renderScale, 0.1f, 0.5f, 2.0f, fmt);
      }
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  // Bottom row
  const float rowY = (m_inner[3] - m_inner[1]) - ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().WindowPadding.y;
  ImGui::SetCursorPosY(rowY);
  const float bw = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
  if (ImGui::Button("RESUME", ImVec2(bw, 0)))
    r.resume = true;
  ImGui::SameLine();
  if (ImGui::Button("RE-CENTRE", ImVec2(bw, 0)))
    r.recenter = true;
  ImGui::SameLine();
  if (ImGui::Button("QUIT GAME", ImVec2(bw, 0)))
    r.quit = true;

  ImGui::End();
  ImGui::Render();

  GLint prevFbo = 0, viewport[4];
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
  glGetIntegerv(GL_VIEWPORT, viewport);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
  glViewport(0, 0, m_w * kSupersample, m_h * kSupersample);
  glDisable(GL_SCISSOR_TEST);
  if (m_frameTex)
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);   // see-through around the artwork
  else
    glClearColor(0.06f, 0.06f, 0.08f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  glBindTexture(GL_TEXTURE_2D, m_tex);
  glGenerateMipmap(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
  glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
  if (scissor)
    glEnable(GL_SCISSOR_TEST);
  else
    glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

  ImGui::SetCurrentContext(previous);
  return r;
}
