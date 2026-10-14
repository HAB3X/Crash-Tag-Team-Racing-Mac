// The window and the picture's way to it. The GS draws on the GPU (the hardware layer's OpenGL renderer, hw/hw/hwgs.h) at a
// multiple of the PS2's resolution, on the render thread with an OpenGL context of its own; the window has another, sharing
// its textures. At each vblank the render thread copies the shown picture into one of three textures and fences it, and the
// main thread draws the newest one over the window when the fence is passed (the Crash Twinsanity native port's hand-over,
// graphics/display.cpp there). Snapshots read the picture back. The software GS (the PS2's own rules at its resolution) is
// still there: --gs software or $PS2_GS=software, and whenever there's no OpenGL 3.3 or no render thread.
//
// The PC menus' display settings are kept here and in graphics.txt: the window's size or fullscreen, the GPU's internal
// resolution, the textures' filtering and upscale, anti-aliasing (FXAA on the picture shown, or supersampling: the internal
// resolution 2 or 4 times higher, up to 8 times the PS2's), the CRT filter and v-sync (the presenter's, from the Twinsanity
// port's graphics/display.cpp and ui/crtfilter.cpp). The menus' overlay is drawn over the picture (ui/menus.h).
#include "runtime.h"

#include "ui/menus.h"

#include "../../hw/gs/gs.h"
#include "../../hw/hw/gl.h"
#include "../../hw/hw/hwgs.h"
#include "../../hw/native.h"
#include "../../hw/png.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <map>
#include <mutex>

namespace NativeGraphics
{
Hardware& GetHardware() { return Ps2::Hw(); }
bool PresenterContext(SDL_Window** window, SDL_GLContext* context);
bool RenderContext(SDL_Window** window, SDL_GLContext* context);
}

namespace Native
{
std::string SettingsFolder() { return Ps2::g_options.settingsDir; }
}

namespace Ps2
{
namespace
{
using namespace Hw;

SDL_Window* g_window = nullptr;
SDL_GLContext g_windowContext = nullptr;
// The render thread's context, with a hidden window of its own to be current with
SDL_Window* g_renderWindow = nullptr;
SDL_GLContext g_renderContext = nullptr;
thread_local bool t_onRenderThread = false;

// The window's drawing: a quad over the viewport from the vertex number alone, rows top first; FXAA's and the CRT filter's
// programs in its place when they're on
GLuint g_program = 0;
GLint g_uvRect = -1;
GLuint g_fxaaProgram = 0;
GLuint g_crtProgram = 0;
GLuint g_vertexArray = 0;
// The software GS's pictures (and the hardware renderer's when the shown buffer isn't one of its targets) go up into this
GLuint g_cpuTexture = 0;
u32 g_cpuWidth = 0, g_cpuHeight = 0;
bool g_vsyncSet = false;

// The display settings (graphics.txt): the window's size in points, fullscreen, the textures' filtering (0 the game's, 1
// bilinear, 2 trilinear), anti-aliasing (0 off, 1 FXAA, 2 and 3 supersampling 2x and 4x), the CRT filter, v-sync
int g_windowWidth = 1280, g_windowHeight = 960;
bool g_fullscreen = false;
int g_filtering = 0;
int g_antiAliasing = 0;
bool g_crt = false;
bool g_vsync = true;
// The window's sizes the menus offer
constexpr int WindowSizes[][2] = {{960, 720}, {1280, 960}, {1600, 1200}, {1920, 1440}, {1280, 720}, {1920, 1080}};

constexpr const char* VertexSource = R"(#version 330 core
out vec2 place;
uniform vec4 uvRect;
void main()
{
    vec2 corner = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    place = mix(uvRect.xy, uvRect.zw, vec2(corner.x, 1.0 - corner.y));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* FragmentSource = R"(#version 330 core
in vec2 place;
out vec4 colour;
uniform sampler2D picture;
void main()
{
    colour = vec4(texture(picture, place).rgb, 1.0);
}
)";

// FXAA (a short version of Timothy Lottes' FXAA 3 console algorithm): edges found by luma contrast with their neighbours and
// blended along them, on the shown picture (the Twinsanity port's)
constexpr const char* FxaaSource = R"(#version 330 core
in vec2 place;
out vec4 colour;
uniform sampler2D picture;
uniform vec2 texel;
float Luma(vec3 c)
{
    return dot(c, vec3(0.299, 0.587, 0.114));
}
void main()
{
    vec3 m = texture(picture, place).rgb;
    vec3 nw = texture(picture, place + vec2(-1.0, -1.0) * texel).rgb;
    vec3 ne = texture(picture, place + vec2(1.0, -1.0) * texel).rgb;
    vec3 sw = texture(picture, place + vec2(-1.0, 1.0) * texel).rgb;
    vec3 se = texture(picture, place + vec2(1.0, 1.0) * texel).rgb;
    float lm = Luma(m);
    float lnw = Luma(nw);
    float lne = Luma(ne);
    float lsw = Luma(sw);
    float lse = Luma(se);
    float lowest = min(lm, min(min(lnw, lne), min(lsw, lse)));
    float highest = max(lm, max(max(lnw, lne), max(lsw, lse)));
    vec2 direction = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
    float reduce = max((lnw + lne + lsw + lse) * 0.25 * (1.0 / 8.0), 1.0 / 128.0);
    float smallest = 1.0 / (min(abs(direction.x), abs(direction.y)) + reduce);
    direction = clamp(direction * smallest, vec2(-8.0), vec2(8.0)) * texel;
    vec3 a = 0.5 * (texture(picture, place + direction * (1.0 / 3.0 - 0.5)).rgb + texture(picture, place + direction * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (texture(picture, place - direction * 0.5).rgb + texture(picture, place + direction * 0.5).rgb);
    float lb = Luma(b);
    colour = vec4((lb < lowest || lb > highest) ? a : b, 1.0);
}
)";

// The CRT filter: the picture as a period television showed it, kept subtle (the Twinsanity port's ui/crtfilter.cpp): the
// beam's horizontal softness, a dark gap between the picture's lines (less where the line is bright, as a beam blooms), an
// aperture grille's faint stripes at the screen's own pixels, the brightness the gaps take given back, the corners a touch
// darker. pictureSize is the PS2's lines (the texture's at the internal resolution divided down)
constexpr const char* CrtSource = R"(#version 330 core
in vec2 place;
out vec4 colour;
uniform sampler2D picture;
uniform vec2 pictureSize;
uniform vec2 viewSize;
vec3 Sample(vec2 at)
{
    return texture(picture, at).rgb;
}
void main()
{
    vec2 texel = 1.0 / pictureSize;
    vec3 beam = Sample(place) * 0.5 + Sample(place - vec2(texel.x, 0.0)) * 0.25 + Sample(place + vec2(texel.x, 0.0)) * 0.25;
    float row = fract(place.y * pictureSize.y);
    float brightness = max(max(beam.r, beam.g), beam.b);
    float depth = mix(0.35, 0.12, brightness);
    float line = 1.0 - depth * pow(abs(row - 0.5) * 2.0, 2.0);
    float column = mod(floor(place.x * viewSize.x), 3.0);
    vec3 grille = viewSize.x >= pictureSize.x * 2.0 ? (column == 0.0 ? vec3(1.0, 0.94, 0.94)
                                                     : column == 1.0 ? vec3(0.94, 1.0, 0.94) : vec3(0.94, 0.94, 1.0))
                                                   : vec3(1.0);
    vec2 centred = place * 2.0 - 1.0;
    float vignette = 1.0 - 0.08 * dot(centred * centred, vec2(0.5, 0.5));
    colour = vec4(clamp(beam * line * grille * 1.12 * vignette, 0.0, 1.0), 1.0);
}
)";

// The three copies of the shown picture the render thread hands over: the one the window draws (busy), the newest one made
// (ready) and one to make the next into. A copy's fences: the render thread's after its copy (the window waits for it), the
// window's after drawing it (the render thread waits for it before it copies into it again)
struct Slot
{
    NativeGraphics::ShownTexture shown = {};
    void* drawn = nullptr;
};
std::mutex g_slotLock;
Slot g_slots[3];
int g_ready = -1;
// The copy the window drew last (kept for drawing again under the PC menus while the game is paused: the render thread
// copies into the third)
int g_shown = -1;

GLuint Compile(Gl& gl, GLenum kind, const char* source)
{
    GLuint shader = gl.CreateShader(kind);
    gl.ShaderSource(shader, 1, &source, nullptr);
    gl.CompileShader(shader);
    GLint ok = 0;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024];
        gl.GetShaderInfoLog(shader, sizeof(log), nullptr, log);
        Log("graphics: the window's shader: %s", log);
    }
    return shader;
}

void SetGlAttributes()
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
}

GLuint Program(Gl& gl, const char* fragmentSource)
{
    GLuint vertex = Compile(gl, GL_VERTEX_SHADER, VertexSource);
    GLuint fragment = Compile(gl, GL_FRAGMENT_SHADER, fragmentSource);
    GLuint program = gl.CreateProgram();
    gl.AttachShader(program, vertex);
    gl.AttachShader(program, fragment);
    gl.LinkProgram(program);
    gl.DeleteShader(vertex);
    gl.DeleteShader(fragment);
    GLint linked = 0;
    gl.GetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        Log("graphics: a window's program doesn't link");
        return 0;
    }
    gl.UseProgram(program);
    gl.Uniform1i(gl.GetUniformLocation(program, "picture"), 0);
    return program;
}

// The window's context and what it draws with (false: no OpenGL 3.3)
bool MakeWindowContext()
{
    g_windowContext = SDL_GL_CreateContext(g_window);
    if (g_windowContext == nullptr)
    {
        Log("graphics: no OpenGL 3.3 context for the window: %s", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(g_window, g_windowContext);
    if (!LoadGl())
    {
        return false;
    }
    SDL_GL_SetSwapInterval(g_vsync ? 1 : 0);
    g_vsyncSet = true;
    Gl& gl = GetGl();
    g_program = Program(gl, FragmentSource);
    if (g_program == 0)
    {
        return false;
    }
    g_fxaaProgram = Program(gl, FxaaSource);
    g_crtProgram = Program(gl, CrtSource);
    g_uvRect = gl.GetUniformLocation(g_program, "uvRect");
    gl.GenVertexArrays(1, &g_vertexArray);
    gl.GenTextures(1, &g_cpuTexture);
    gl.BindTexture(GL_TEXTURE_2D, g_cpuTexture);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_CLAMP_TO_EDGE));
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_CLAMP_TO_EDGE));
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    return true;
}

// The render thread's context, made here (SDL's windows are the main thread's) sharing the window's objects
bool MakeRenderContext()
{
    SetGlAttributes();
    if (g_windowContext != nullptr)
    {
        SDL_GL_MakeCurrent(g_window, g_windowContext);
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    }
    g_renderWindow = SDL_CreateWindow("graphics", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (g_renderWindow != nullptr)
    {
        g_renderContext = SDL_GL_CreateContext(g_renderWindow);
    }
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    if (g_renderContext == nullptr)
    {
        Log("graphics: no OpenGL 3.3 context for the render thread: %s", SDL_GetError());
        if (g_renderWindow != nullptr)
        {
            SDL_DestroyWindow(g_renderWindow);
            g_renderWindow = nullptr;
        }
        return false;
    }
    // Making it made it current here: the window's again (the render thread makes its own current there)
    if (g_windowContext != nullptr)
    {
        SDL_GL_MakeCurrent(g_window, g_windowContext);
    }
    else
    {
        SDL_GL_MakeCurrent(g_renderWindow, nullptr);
    }
    return true;
}

// graphics.txt's renderer and scale as read (the options may say otherwise for this run: those are written back)
std::string g_fileRenderer = "hardware";
int g_fileScale = 3;
int g_fileUpscale = 0;

void WriteGraphicsSettings()
{
    std::error_code ec;
    std::filesystem::create_directories(g_options.settingsDir, ec);
    std::ofstream out(g_options.settingsDir + "/graphics.txt");
    out << "# The picture (the PC menus' Display tab changes these). renderer: hardware (the GPU draws the PS2's graphics) or\n"
           "# software (the PS2's own rules, at its resolution; slower). --gs, $PS2_GS override it\n"
           "renderer = " << g_fileRenderer << "\n"
           "# The GPU's resolution, 1-8 times the PS2's (--scale, $PS2_GS_SCALE)\n"
           "scale = " << g_fileScale << "\n"
           "# The game's textures made smoother on the GPU: 0 (off), 2 or 4 times their size (--texture-upscale, "
           "$PS2_TEXTURE_UPSCALE)\n"
           "texture_upscale = " << g_fileUpscale << "\n"
           "# The textures' filtering: 0 the game's own, 1 bilinear, 2 trilinear (and anisotropic)\n"
           "filtering = " << g_filtering << "\n"
           "# Anti-aliasing: 0 off, 1 FXAA, 2 or 3 supersampling (the GPU's resolution 2 or 4 times higher, up to 8x)\n"
           "anti_aliasing = " << g_antiAliasing << "\n"
           "# The CRT filter (scanlines): 0 off, 1 on\n"
           "crt = " << (g_crt ? 1 : 0) << "\n"
           "# V-sync: 0 off, 1 on\n"
           "vsync = " << (g_vsync ? 1 : 0) << "\n"
           "# The window: its size in points, and fullscreen (0 or 1; Cmd+F or Alt+Enter switch)\n"
           "window = " << g_windowWidth << "x" << g_windowHeight << "\n"
           "fullscreen = " << (g_fullscreen ? 1 : 0) << "\n";
}

// The settings folder's graphics.txt (written with the defaults the first time): name = value lines
std::map<std::string, std::string> ReadGraphicsSettings()
{
    std::map<std::string, std::string> values;
    std::string path = g_options.settingsDir + "/graphics.txt";
    std::ifstream in(path);
    if (!in)
    {
        WriteGraphicsSettings();
        return values;
    }
    std::string line;
    while (std::getline(in, line))
    {
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos)
        {
            continue;
        }
        auto trim = [](std::string text) {
            text.erase(0, text.find_first_not_of(" \t\r"));
            text.erase(text.find_last_not_of(" \t\r") + 1);
            return text;
        };
        values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return values;
}

// The option's value, or $NAME's, or graphics.txt's, or the default
std::string Setting(const char* name, const std::map<std::string, std::string>& file, const char* key, const char* otherwise)
{
    const char* value = getenv(name);
    if (value != nullptr && *value != 0)
    {
        return value;
    }
    auto found = file.find(key);
    return found != file.end() ? found->second : otherwise;
}

// The GPU's resolution with supersampling's
int EffectiveScale() { return std::min(8, g_options.gsScale * (g_antiAliasing == 2 ? 2 : g_antiAliasing == 3 ? 4 : 1)); }

// The picture's place in a window of these pixels: the TV's 4:3, letterboxed
SDL_FRect PictureRect(int width, int height)
{
    float scale = std::min(width / 640.0f, height / 480.0f);
    return {(width - 640 * scale) / 2, (height - 480 * scale) / 2, 640 * scale, 480 * scale};
}

// The texture to draw: the newest copy (it becomes the shown one), else the shown one again when asked to redraw, else the
// pixels given; 0 when there's nothing new to draw. Its picture's size
GLuint TakePicture(const Gs::DisplayImage* image, bool redraw, u32* width, u32* height, int* slot)
{
    Gl& gl = GetGl();
    *slot = -1;
    {
        std::lock_guard<std::mutex> lock(g_slotLock);
        if (g_ready >= 0)
        {
            g_shown = g_ready;
            g_ready = -1;
            *slot = g_shown;
        }
        else if (redraw && g_shown >= 0 && (image == nullptr || image->width == 0))
        {
            *slot = g_shown;
        }
    }
    if (*slot >= 0)
    {
        Slot& s = g_slots[*slot];
        if (s.shown.fence != nullptr)
        {
            gl.WaitSync(static_cast<GLsync>(s.shown.fence), 0, GL_TIMEOUT_IGNORED);
            gl.DeleteSync(static_cast<GLsync>(s.shown.fence));
            s.shown.fence = nullptr;
        }
        *width = s.shown.width;
        *height = s.shown.height;
        return s.shown.texture;
    }
    if (image != nullptr && image->width != 0)
    {
        {
            // A picture of the GS's memory now: the copies are older
            std::lock_guard<std::mutex> lock(g_slotLock);
            g_shown = -1;
        }
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, g_cpuTexture);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl.TexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), static_cast<GLsizei>(image->width),
                      static_cast<GLsizei>(image->height), 0, GL_RGBA, GL_UNSIGNED_BYTE, image->pixels.data());
        g_cpuWidth = image->width;
        g_cpuHeight = image->height;
        *width = g_cpuWidth;
        *height = g_cpuHeight;
        return g_cpuTexture;
    }
    if (redraw && g_cpuWidth != 0)
    {
        *width = g_cpuWidth;
        *height = g_cpuHeight;
        return g_cpuTexture;
    }
    return 0;
}

// The picture (and the menus' overlay) drawn into the framebuffer bound, of these pixels
void DrawPicture(GLuint texture, u32 pictureWidth, u32 pictureHeight, GLuint framebuffer, int width, int height,
                 float pixelScale)
{
    Gl& gl = GetGl();
    SDL_FRect dst = PictureRect(width, height);
    gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Disable(GL_BLEND);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_STENCIL_TEST);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.Viewport(0, 0, width, height);
    gl.ClearColor(0, 0, 0, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    if (texture != 0)
    {
        gl.Viewport(static_cast<GLint>(dst.x), static_cast<GLint>(height - dst.y - dst.h), static_cast<GLsizei>(dst.w),
                    static_cast<GLsizei>(dst.h));
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, texture);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
        // The CRT filter, else FXAA, else the plain picture
        GLuint program = g_crt && g_crtProgram != 0 ? g_crtProgram : g_antiAliasing == 1 && g_fxaaProgram != 0 ? g_fxaaProgram
                                                                                                                 : g_program;
        gl.UseProgram(program);
        gl.Uniform4f(gl.GetUniformLocation(program, "uvRect"), 0, 0, 1, 1);
        if (program == g_fxaaProgram)
        {
            gl.Uniform2f(gl.GetUniformLocation(program, "texel"), 1.0f / static_cast<float>(pictureWidth),
                         1.0f / static_cast<float>(pictureHeight));
        }
        else if (program == g_crtProgram)
        {
            // The PS2's lines: the picture's pixels less the GPU's scale (the software GS's are the PS2's own)
            float scale = texture == g_cpuTexture ? 1.0f : static_cast<float>(NativeGraphics::HardwareScale());
            gl.Uniform2f(gl.GetUniformLocation(program, "pictureSize"), static_cast<float>(pictureWidth) / scale,
                         static_cast<float>(pictureHeight) / scale);
            gl.Uniform2f(gl.GetUniformLocation(program, "viewSize"), dst.w, dst.h);
        }
        gl.BindVertexArray(g_vertexArray);
        gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    gl.Viewport(0, 0, width, height);
    Ui::MenusDraw(width, height, pixelScale, dst);
}

void ApplyWindow()
{
    if (g_window == nullptr || g_options.headless)
    {
        return;
    }
    if (!g_fullscreen)
    {
        SDL_SetWindowFullscreen(g_window, false);
        SDL_SetWindowSize(g_window, g_windowWidth, g_windowHeight);
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    else
    {
        SDL_SetWindowFullscreen(g_window, true);
    }
}

// The GS's settings changed on the GS's side (in their place among its work: from the game's side)
void ApplyRenderer()
{
    int scale = EffectiveScale();
    int filtering = g_filtering;
    int upscale = g_options.textureUpscale == 4 ? 2 : g_options.textureUpscale == 2 ? 1 : 0;
    KernelOnGameSide([=] {
        RenderCall([=] {
            NativeGraphics::HardwareSetScale(static_cast<u32>(scale));
            NativeGraphics::HardwareSetFiltering(filtering);
            NativeGraphics::HardwareSetUpscale(upscale);
        });
    });
}
}

SDL_Window* DisplayOpen()
{
    // The renderer: the GPU unless asked for the software GS
    std::map<std::string, std::string> file = ReadGraphicsSettings();
    if (g_options.gs < 0)
    {
        g_options.gs = Setting("PS2_GS", file, "renderer", "hardware") == "software" ? 0 : 1;
    }
    if (g_options.gsScale < 0)
    {
        g_options.gsScale = atoi(Setting("PS2_GS_SCALE", file, "scale", "3").c_str());
    }
    g_options.gsScale = std::clamp(g_options.gsScale, 1, 8);
    if (g_options.textureUpscale < 0)
    {
        g_options.textureUpscale = atoi(Setting("PS2_TEXTURE_UPSCALE", file, "texture_upscale", "0").c_str());
    }
    g_options.textureUpscale = g_options.textureUpscale >= 4 ? 4 : g_options.textureUpscale >= 2 ? 2 : 0;
    g_fileRenderer = Setting("", file, "renderer", "hardware") == "software" ? "software" : "hardware";
    g_fileScale = std::clamp(atoi(Setting("", file, "scale", "3").c_str()), 1, 8);
    g_fileUpscale = atoi(Setting("", file, "texture_upscale", "0").c_str());
    g_filtering = std::clamp(atoi(Setting("", file, "filtering", "0").c_str()), 0, 2);
    g_antiAliasing = std::clamp(atoi(Setting("", file, "anti_aliasing", "0").c_str()), 0, 3);
    g_crt = atoi(Setting("", file, "crt", "0").c_str()) != 0;
    g_vsync = atoi(Setting("", file, "vsync", "1").c_str()) != 0;
    g_fullscreen = atoi(Setting("", file, "fullscreen", "0").c_str()) != 0;
    if (sscanf(Setting("", file, "window", "1280x960").c_str(), "%dx%d", &g_windowWidth, &g_windowHeight) != 2 ||
        g_windowWidth < 320 || g_windowHeight < 240)
    {
        g_windowWidth = 1280;
        g_windowHeight = 960;
    }
    // Settings added since the file was written are written in with their defaults
    if (!file.empty() && file.find("vsync") == file.end())
    {
        WriteGraphicsSettings();
    }

    // Headless, the menus' scripted pictures (--ui) are drawn in a hidden window's context
    bool hiddenWindow = g_options.headless && !g_options.ui.empty();
    if (g_options.headless && g_options.gs == 0 && !hiddenWindow)
    {
        return nullptr;
    }
    if (g_options.headless)
    {
        // Only a hidden window for the GPU's context: no icon in the Dock, no menu bar
        SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
    }
    if (!SDL_Init(SDL_INIT_VIDEO | (g_options.headless ? 0 : SDL_INIT_GAMEPAD)))
    {
        Log("SDL_Init: %s (running headless, the software GS drawing)", SDL_GetError());
        g_options.headless = true;
        g_options.gs = 0;
        return nullptr;
    }
    if (!g_options.headless || hiddenWindow)
    {
        SetGlAttributes();
        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (hiddenWindow)
        {
            flags |= SDL_WINDOW_HIDDEN;
        }
        else if (g_fullscreen)
        {
            flags |= SDL_WINDOW_FULLSCREEN;
        }
        g_window = SDL_CreateWindow(WindowTitle().c_str(), hiddenWindow ? 1280 : g_windowWidth, hiddenWindow ? 960 : g_windowHeight,
                                    flags);
        if (g_window == nullptr || !MakeWindowContext())
        {
            Log("graphics: no window: %s", SDL_GetError());
            Fatal("no OpenGL window");
        }
    }
    if (g_options.gs == 1 && !MakeRenderContext())
    {
        g_options.gs = 0;
    }
    return g_window;
}

void DisplayRenderThreadStart()
{
    t_onRenderThread = true;
    if (g_options.gs != 1 || g_renderContext == nullptr)
    {
        return;
    }
    SDL_GL_MakeCurrent(g_renderWindow, g_renderContext);
    if (!NativeGraphics::StartHardwareRenderer(static_cast<u32>(EffectiveScale())))
    {
        Log("graphics: no hardware renderer: the software GS draws");
        g_options.gs = 0;
        return;
    }
    NativeGraphics::HardwareSetUpscale(g_options.textureUpscale == 4 ? 2 : g_options.textureUpscale == 2 ? 1 : 0);
    NativeGraphics::HardwareSetFiltering(g_filtering);
    Log("graphics: the GS draws on the GPU at %dx the PS2's resolution%s", g_options.gsScale,
        g_options.textureUpscale != 0 ? (", textures upscaled " + std::to_string(g_options.textureUpscale) + "x").c_str() : "");
}

void DisplayNoRenderThread()
{
    if (g_options.gs == 1)
    {
        Log("graphics: the hardware renderer needs the render thread: the software GS draws");
        g_options.gs = 0;
    }
}

bool DisplayOnGpu() { return NativeGraphics::HardwareRendererActive(); }

int DisplayCopyShown()
{
    // A slot neither the window's shown one nor the newest one
    int slot = 0;
    void* drawn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_slotLock);
        while (slot == g_shown || slot == g_ready)
        {
            slot++;
        }
        drawn = g_slots[slot].drawn;
        g_slots[slot].drawn = nullptr;
    }
    NativeGraphics::ShownTexture shown = {};
    if (!NativeGraphics::HardwareCopyDisplay(static_cast<u32>(slot), drawn, &shown))
    {
        // (the copy's fence wasn't waited for: kept for the next copy into it)
        std::lock_guard<std::mutex> lock(g_slotLock);
        g_slots[slot].drawn = drawn;
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_slotLock);
    g_slots[slot].shown = shown;
    g_ready = slot;
    return slot;
}

void DisplayDropShown()
{
    std::lock_guard<std::mutex> lock(g_slotLock);
    g_ready = -1;
}

bool DisplayReadShown(Gs::DisplayImage& image)
{
    if (NativeGraphics::HardwareRendererActive() && NativeGraphics::HardwareReadDisplay(image))
    {
        return true;
    }
    Hw().gs->ReadDisplay(image);
    return false;
}

void DisplayPresent(const Gs::DisplayImage* image, bool redraw, SDL_FRect* pictureRect)
{
    if (g_window == nullptr || g_options.headless)
    {
        return;
    }
    Gl& gl = GetGl();
    if (!g_vsyncSet)
    {
        SDL_GL_SetSwapInterval(g_vsync ? 1 : 0);
        g_vsyncSet = true;
    }
    u32 pictureWidth = 0, pictureHeight = 0;
    int slot = -1;
    GLuint texture = TakePicture(image, redraw, &pictureWidth, &pictureHeight, &slot);
    if (texture == 0 && !redraw)
    {
        return;
    }
    // The TV's 4:3 in the window, letterboxed; the picture scaled smoothly (the TV's picture has no pixels of its own)
    int width = 0, height = 0, points = 0, pointsHigh = 0;
    SDL_GetWindowSizeInPixels(g_window, &width, &height);
    SDL_GetWindowSize(g_window, &points, &pointsHigh);
    float density = points > 0 ? static_cast<float>(width) / static_cast<float>(points) : 1.0f;
    DrawPicture(texture, pictureWidth, pictureHeight, 0, width, height, density);
    if (slot >= 0)
    {
        void* drawn = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        gl.Flush();
        std::lock_guard<std::mutex> lock(g_slotLock);
        if (g_slots[slot].drawn != nullptr)
        {
            gl.DeleteSync(static_cast<GLsync>(g_slots[slot].drawn));
        }
        g_slots[slot].drawn = drawn;
    }
    SDL_GL_SwapWindow(g_window);
    // The mouse's place over the picture goes by the window's points
    SDL_FRect dst = PictureRect(width, height);
    *pictureRect = {dst.x / density, dst.y / density, dst.w / density, dst.h / density};
}

bool DisplayShot(const std::string& path, const Gs::DisplayImage* image)
{
    if (g_window == nullptr)
    {
        return false;
    }
    Gl& gl = GetGl();
    SDL_GL_MakeCurrent(g_window, g_windowContext);
    // The window's own size (headless, its hidden one's points: a 1280 x 960 window as on a screen of one pixel a point)
    int width = 0, height = 0;
    if (g_options.headless)
    {
        SDL_GetWindowSize(g_window, &width, &height);
    }
    else
    {
        SDL_GetWindowSizeInPixels(g_window, &width, &height);
    }
    int points = 0, pointsHigh = 0;
    SDL_GetWindowSize(g_window, &points, &pointsHigh);
    float density = points > 0 && !g_options.headless ? static_cast<float>(width) / static_cast<float>(points) : 1.0f;
    u32 pictureWidth = 0, pictureHeight = 0;
    int slot = -1;
    GLuint texture = TakePicture(image, true, &pictureWidth, &pictureHeight, &slot);
    GLuint target = 0, framebuffer = 0;
    gl.GenTextures(1, &target);
    gl.BindTexture(GL_TEXTURE_2D, target);
    gl.TexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.GenFramebuffers(1, &framebuffer);
    gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    DrawPicture(texture, pictureWidth, pictureHeight, framebuffer, width, height, density);
    std::vector<u32> rows(static_cast<size_t>(width) * height), pixels(rows.size());
    gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.ReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            pixels[static_cast<size_t>(y) * width + x] = rows[static_cast<size_t>(height - 1 - y) * width + x] | 0xFF000000u;
        }
    }
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.DeleteFramebuffers(1, &framebuffer);
    gl.DeleteTextures(1, &target);
    if (!WritePng(path, pixels.data(), static_cast<u32>(width), static_cast<u32>(height)))
    {
        return false;
    }
    Log("saved %s (%dx%d, the window with the PC menus)", path.c_str(), width, height);
    return true;
}

int DisplayGet(DisplaySetting setting)
{
    switch (setting)
    {
    case DisplaySetting::Fullscreen: return g_fullscreen ? 1 : 0;
    case DisplaySetting::WindowSize:
    {
        if (g_window != nullptr && !g_options.headless && !g_fullscreen)
        {
            SDL_GetWindowSize(g_window, &g_windowWidth, &g_windowHeight);
        }
        for (int i = 0; i < static_cast<int>(std::size(WindowSizes)); i++)
        {
            if (WindowSizes[i][0] == g_windowWidth && WindowSizes[i][1] == g_windowHeight)
            {
                return i;
            }
        }
        return -1;
    }
    case DisplaySetting::Scale: return g_options.gsScale;
    case DisplaySetting::TextureUpscale: return g_options.textureUpscale == 4 ? 2 : g_options.textureUpscale == 2 ? 1 : 0;
    case DisplaySetting::Filtering: return g_filtering;
    case DisplaySetting::AntiAliasing: return g_antiAliasing;
    case DisplaySetting::Crt: return g_crt ? 1 : 0;
    case DisplaySetting::VSync: return g_vsync ? 1 : 0;
    case DisplaySetting::OnGpu: return g_options.gs == 1 ? 1 : 0;
    }
    return 0;
}

void DisplaySet(DisplaySetting setting, int value)
{
    switch (setting)
    {
    case DisplaySetting::Fullscreen:
        g_fullscreen = value != 0;
        ApplyWindow();
        break;
    case DisplaySetting::WindowSize:
        if (value >= 0 && value < static_cast<int>(std::size(WindowSizes)))
        {
            g_windowWidth = WindowSizes[value][0];
            g_windowHeight = WindowSizes[value][1];
            g_fullscreen = false;
            ApplyWindow();
        }
        break;
    case DisplaySetting::Scale:
        g_options.gsScale = std::clamp(value, 1, 8);
        g_fileScale = g_options.gsScale;
        ApplyRenderer();
        break;
    case DisplaySetting::TextureUpscale:
        g_options.textureUpscale = value == 2 ? 4 : value == 1 ? 2 : 0;
        g_fileUpscale = g_options.textureUpscale;
        ApplyRenderer();
        break;
    case DisplaySetting::Filtering:
        g_filtering = std::clamp(value, 0, 2);
        ApplyRenderer();
        break;
    case DisplaySetting::AntiAliasing:
        g_antiAliasing = std::clamp(value, 0, 3);
        ApplyRenderer();
        break;
    case DisplaySetting::Crt: g_crt = value != 0; break;
    case DisplaySetting::VSync:
        g_vsync = value != 0;
        g_vsyncSet = false;
        break;
    case DisplaySetting::OnGpu: return;
    }
    WriteGraphicsSettings();
}

std::string DisplayWindowSizeText(int choice)
{
    int w = g_windowWidth, h = g_windowHeight;
    if (choice >= 0 && choice < static_cast<int>(std::size(WindowSizes)))
    {
        w = WindowSizes[choice][0];
        h = WindowSizes[choice][1];
    }
    return std::to_string(w) + " \u00d7 " + std::to_string(h);
}

int DisplayWindowSizeCount() { return static_cast<int>(std::size(WindowSizes)); }

void DisplayWindowPoints(int* width, int* height)
{
    DisplayGet(DisplaySetting::WindowSize);
    *width = g_windowWidth;
    *height = g_windowHeight;
}

void DisplaySetWindowPoints(int width, int height)
{
    g_windowWidth = width;
    g_windowHeight = height;
    ApplyWindow();
    WriteGraphicsSettings();
}

void DisplayClose()
{
    if (g_window != nullptr)
    {
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
    }
    SDL_Quit();
}
}

namespace NativeGraphics
{
bool PresenterContext(SDL_Window** window, SDL_GLContext* context)
{
    if (Ps2::g_window == nullptr || Ps2::g_windowContext == nullptr)
    {
        return false;
    }
    *window = Ps2::g_window;
    *context = Ps2::g_windowContext;
    return true;
}

bool RenderContext(SDL_Window** window, SDL_GLContext* context)
{
    if (!Ps2::t_onRenderThread || Ps2::g_renderContext == nullptr)
    {
        return false;
    }
    *window = Ps2::g_renderWindow;
    *context = Ps2::g_renderContext;
    return true;
}
}
