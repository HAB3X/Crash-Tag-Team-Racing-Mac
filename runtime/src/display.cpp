// The window and the picture's way to it. The GS draws on the GPU (the hardware layer's OpenGL renderer, hw/hw/hwgs.h) at a
// multiple of the PS2's resolution, on the render thread with an OpenGL context of its own; the window has another, sharing
// its textures. At each vblank the render thread copies the shown picture into one of three textures and fences it, and the
// main thread draws the newest one over the window when the fence is passed (the Crash Twinsanity native port's hand-over,
// graphics/display.cpp there). Snapshots read the picture back. The software GS (the PS2's own rules at its resolution) is
// still there: --gs software or $PS2_GS=software, and whenever there's no OpenGL 3.3 or no render thread.
#include "runtime.h"

#include "../../hw/gs/gs.h"
#include "../../hw/hw/gl.h"
#include "../../hw/hw/hwgs.h"
#include "../../hw/native.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// The window's drawing: a quad over the viewport from the vertex number alone, rows top first
GLuint g_program = 0;
GLint g_uvRect = -1;
GLuint g_vertexArray = 0;
// The software GS's pictures (and the hardware renderer's when the shown buffer isn't one of its targets) go up into this
GLuint g_cpuTexture = 0;

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
int g_busy = -1;
int g_ready = -1;

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
    SDL_GL_SetSwapInterval(1);
    Gl& gl = GetGl();
    GLuint vertex = Compile(gl, GL_VERTEX_SHADER, VertexSource);
    GLuint fragment = Compile(gl, GL_FRAGMENT_SHADER, FragmentSource);
    g_program = gl.CreateProgram();
    gl.AttachShader(g_program, vertex);
    gl.AttachShader(g_program, fragment);
    gl.LinkProgram(g_program);
    gl.DeleteShader(vertex);
    gl.DeleteShader(fragment);
    GLint linked = 0;
    gl.GetProgramiv(g_program, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        Log("graphics: the window's program doesn't link");
        return false;
    }
    gl.UseProgram(g_program);
    gl.Uniform1i(gl.GetUniformLocation(g_program, "picture"), 0);
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

// The settings folder's graphics.txt (written with the defaults the first time): name = value lines
std::map<std::string, std::string> ReadGraphicsSettings()
{
    std::map<std::string, std::string> values;
    std::string path = g_options.settingsDir + "/graphics.txt";
    std::ifstream in(path);
    if (!in)
    {
        std::error_code ec;
        std::filesystem::create_directories(g_options.settingsDir, ec);
        std::ofstream out(path);
        out << "# The picture. renderer: hardware (the GPU draws the PS2's graphics) or software (the PS2's own rules, at its\n"
               "# resolution; slower). --gs, $PS2_GS override it\n"
               "renderer = hardware\n"
               "# The GPU's resolution, 1-8 times the PS2's (--scale, $PS2_GS_SCALE)\n"
               "scale = 3\n"
               "# The game's textures made smoother on the GPU: 0 (off), 2 or 4 times their size (--texture-upscale, "
               "$PS2_TEXTURE_UPSCALE)\n"
               "texture_upscale = 0\n";
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

    if (g_options.headless && g_options.gs == 0)
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
    if (!g_options.headless)
    {
        SetGlAttributes();
        g_window = SDL_CreateWindow(WindowTitle().c_str(), 1280, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
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
    if (!NativeGraphics::StartHardwareRenderer(static_cast<u32>(g_options.gsScale)))
    {
        Log("graphics: no hardware renderer: the software GS draws");
        g_options.gs = 0;
        return;
    }
    NativeGraphics::HardwareSetUpscale(g_options.textureUpscale == 4 ? 2 : g_options.textureUpscale == 2 ? 1 : 0);
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
    // A slot neither drawn by the window nor the newest one
    int slot = 0;
    void* drawn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_slotLock);
        while (slot == g_busy || slot == g_ready)
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

void DisplayPresent(const Gs::DisplayImage* image, SDL_FRect* pictureRect)
{
    if (g_window == nullptr)
    {
        return;
    }
    Gl& gl = GetGl();
    int slot = -1;
    {
        std::lock_guard<std::mutex> lock(g_slotLock);
        if (g_ready >= 0)
        {
            slot = g_ready;
            g_ready = -1;
            g_busy = slot;
        }
    }
    GLuint texture = 0;
    if (slot >= 0)
    {
        Slot& s = g_slots[slot];
        if (s.shown.fence != nullptr)
        {
            gl.WaitSync(static_cast<GLsync>(s.shown.fence), 0, GL_TIMEOUT_IGNORED);
            gl.DeleteSync(static_cast<GLsync>(s.shown.fence));
            s.shown.fence = nullptr;
        }
        texture = s.shown.texture;
    }
    else if (image != nullptr && image->width != 0)
    {
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, g_cpuTexture);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl.TexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), static_cast<GLsizei>(image->width),
                      static_cast<GLsizei>(image->height), 0, GL_RGBA, GL_UNSIGNED_BYTE, image->pixels.data());
        texture = g_cpuTexture;
    }
    else
    {
        return;
    }

    // The TV's 4:3 in the window, letterboxed; the picture scaled smoothly (the TV's picture has no pixels of its own)
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(g_window, &width, &height);
    float scale = std::min(width / 640.0f, height / 480.0f);
    SDL_FRect dst = {(width - 640 * scale) / 2, (height - 480 * scale) / 2, 640 * scale, 480 * scale};
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Disable(GL_BLEND);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_STENCIL_TEST);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.Viewport(0, 0, width, height);
    gl.ClearColor(0, 0, 0, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    gl.Viewport(static_cast<GLint>(dst.x), static_cast<GLint>(height - dst.y - dst.h), static_cast<GLsizei>(dst.w),
                static_cast<GLsizei>(dst.h));
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
    gl.UseProgram(g_program);
    gl.Uniform4f(g_uvRect, 0, 0, 1, 1);
    gl.BindVertexArray(g_vertexArray);
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
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
        g_busy = -1;
    }
    SDL_GL_SwapWindow(g_window);
    // The mouse's place over the picture goes by the window's points
    float density = SDL_GetWindowPixelDensity(g_window);
    density = density > 0 ? density : 1;
    *pictureRect = {dst.x / density, dst.y / density, dst.w / density, dst.h / density};
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
