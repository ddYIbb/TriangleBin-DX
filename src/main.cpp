#ifdef WIN32
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#include <SDL_syswm.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"

#include "logger.h"
#include "renderer.h"

#include <cstring>
#include <cstdio>
#include <ctime>

// ---------------------------------------------------------------------------
// Diagnostic error report.
//
// On any fatal startup error or crash we write a single "trianglebin-..."
// report next to the executable (the working directory), so that a user can
// simply send back that file for diagnosis: it contains OS version, CPU
// architecture, GPU adapter, driver version, highest DirectX feature level and
// the exact error / exception.
// ---------------------------------------------------------------------------
typedef LONG (WINAPI *pfnRtlGetVersion)(OSVERSIONINFOW *);

static std::string getOSVersion() {
    OSVERSIONINFOW vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    pfnRtlGetVersion fn = (pfnRtlGetVersion)GetProcAddress(ntdll, "RtlGetVersion");
    if (fn && fn(&vi) >= 0 && vi.dwMajorVersion != 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Windows %u.%u (build %u)",
                 (unsigned)vi.dwMajorVersion, (unsigned)vi.dwMinorVersion,
                 (unsigned)vi.dwBuildNumber);
        return buf;
    }
    return "unknown";
}

static std::string getArchName() {
    SYSTEM_INFO si = {};
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return "x64 (AMD64)";
        case PROCESSOR_ARCHITECTURE_INTEL: return "x86 (32-bit)";
        case PROCESSOR_ARCHITECTURE_ARM64: return "ARM64";
        case PROCESSOR_ARCHITECTURE_ARM:   return "ARM32";
        default: return "unknown";
    }
}

static void writeDiagnosticReport(const char* severity, const char* detail) {
    std::string osVer = getOSVersion();
    std::string arch = getArchName();
    std::string gpu, dx, driver;
    if (!probeHardwareGpu(gpu, dx, driver)) {
        gpu = "unknown"; dx = "unknown"; driver = "unknown";
    }

    char ts[64];
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_s(&tmv, &now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    FILE* f = nullptr;
    fopen_s(&f, "trianglebin-error-report.txt", "w");
    if (f) {
        fprintf(f, "TriangleBin (DirectX) diagnostic report\n");
        fprintf(f, "=======================================\n");
        fprintf(f, "Timestamp : %s\n", ts);
        fprintf(f, "OS        : %s\n", osVer.c_str());
        fprintf(f, "Arch      : %s\n", arch.c_str());
        fprintf(f, "GPU       : \"%s\"\n", gpu.c_str());
        fprintf(f, "Driver    : \"%s\"\n", driver.c_str());
        fprintf(f, "DX support: %s\n", dx.c_str());
        fprintf(f, "Severity  : %s\n", severity);
        fprintf(f, "Message   : %s\n", detail ? detail : "");
        fclose(f);
    }
}

static LONG WINAPI crashHandler(EXCEPTION_POINTERS* eps) {
    char buf[160];
    if (eps && eps->ExceptionRecord) {
        snprintf(buf, sizeof(buf), "Unhandled exception 0x%08X at address 0x%p",
                 (unsigned)eps->ExceptionRecord->ExceptionCode,
                 eps->ExceptionRecord->ExceptionAddress);
    } else {
        snprintf(buf, sizeof(buf), "Unhandled exception (unknown)");
    }
    Log(LOG_FATAL) << buf;
    writeDiagnosticReport("CRASH", buf);
    MessageBoxA(nullptr, buf, "TriangleBin (DirectX) 崩溃报告", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Software vertical-sync pacing.
//
// The DXGI flip-model swapchain pauses the thread inside Present(1,0), but on
// this NVIDIA driver tearing that swapchain down while a vsync present is in
// flight triggers a GPU TDR (release timeout, ~3s freeze). Instead we present
// without blocking (Present(0,0)) and pace the render loop to the display
// refresh rate on the CPU. This keeps the frame rate in sync with the display
// (vsync-like, no tearing because flip model is composited at vsync) while
// avoiding the driver bug on shutdown.
static LARGE_INTEGER g_freq = {};
static LARGE_INTEGER g_last = {};
static double g_interval = 1.0 / 60.0;
static HANDLE g_paceTimer = nullptr;   // high-resolution waitable timer
typedef HRESULT (WINAPI *pfnDwmFlush)();
static pfnDwmFlush g_dwmFlush = nullptr;

static void initPacing(int refreshHz) {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_last);
    const double hz = (refreshHz > 0) ? (double)refreshHz : 60.0;
    g_interval = 1.0 / hz;
    // A high-resolution waitable timer lets us sleep to the target instead of
    // busy-spinning, which used to burn a large fraction of a CPU core.
    g_paceTimer = CreateWaitableTimerExW(nullptr, nullptr,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
    if (!g_paceTimer) {
        g_paceTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    // DwmFlush() blocks until the next DWM composition, which is inherently
    // synced to the display refresh and uses no CPU. Loaded dynamically so no
    // extra import library is required.
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        g_dwmFlush = (pfnDwmFlush)GetProcAddress(dwm, "DwmFlush");
    }
    Log(LOG_INFO) << "Software v-sync target refresh: " << refreshHz << " Hz";
}

// Sleep-based pacing (no busy spin). Only used as a fallback when the renderer
// has no waitable swap chain.
static void paceToRefresh() {
    if (!g_freq.QuadPart) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = (double)(now.QuadPart - g_last.QuadPart) / (double)g_freq.QuadPart;

    // If we fell far behind (e.g. right after a GPU switch), resync instead of
    // trying to catch up.
    if (elapsed < 0.0 || elapsed > g_interval * 4.0) {
        g_last = now;
        elapsed = 0.0;
    }

    if (elapsed < g_interval) {
        double need = g_interval - elapsed; // seconds to wait
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(need * 10000000.0);   // relative, 100ns units
        if (!(g_paceTimer && SetWaitableTimer(g_paceTimer, &due, 0, nullptr, nullptr, FALSE))) {
            SDL_Delay((Uint32)(need * 1000.0));
        } else {
            WaitForSingleObject(g_paceTimer, (DWORD)(need * 1000.0) + 50);
        }
    }
    // Fixed-step advance: keeps the average frame interval exactly equal to the
    // display refresh period, without cumulative drift over time.
    g_last.QuadPart += (LONGLONG)(g_interval * (double)g_freq.QuadPart);
}

static void showFatalError(const char* message) {
    Log(LOG_FATAL) << message;
    writeDiagnosticReport("FATAL", message);
    std::string text = std::string(message) +
        "\n\n详细诊断已写入 trianglebin-error-report.txt。\n请把该文件发给开发者以便定位问题。";
    MessageBoxA(nullptr, text.c_str(), "TriangleBin (DirectX) 错误", MB_OK | MB_ICONERROR);
}

// The original Windows release binary was built with an old imgui release
// (pre-1.53) whose default "classic" theme is the gray look: black
// semi-transparent window body, purple-blue title bar, light gray widget
// frames and dark red-brown buttons. imgui 1.91.6's StyleColorsClassic()
// was modernized and no longer matches it, so we reproduce the old values
// manually (colors and layout metrics).
static void SetupClassicStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(8, 8);
    style.WindowMinSize = ImVec2(32, 32);
    style.WindowRounding = 9.0f;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.ChildRounding = 0.0f;
    style.PopupRounding = 9.0f;
    style.FramePadding = ImVec2(4, 3);
    style.FrameRounding = 0.0f;
    style.ItemSpacing = ImVec2(8, 4);
    style.ItemInnerSpacing = ImVec2(4, 4);
    style.IndentSpacing = 21.0f;
    style.ScrollbarSize = 16.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize = 10.0f;
    style.GrabRounding = 0.0f;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.DisplayWindowPadding = ImVec2(22, 22);
    style.DisplaySafeAreaPadding = ImVec2(4, 4);
    style.AntiAliasedLines = true;
    style.AntiAliasedFill = true;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.70f);
    c[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.05f, 0.05f, 0.10f, 0.90f);
    c[ImGuiCol_Border] = ImVec4(0.70f, 0.70f, 0.70f, 0.40f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.30f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.90f, 0.80f, 0.80f, 0.40f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.90f, 0.65f, 0.65f, 0.45f);
    c[ImGuiCol_TitleBg] = ImVec4(0.27f, 0.27f, 0.54f, 0.83f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.32f, 0.32f, 0.63f, 0.87f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.40f, 0.40f, 0.80f, 0.20f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.40f, 0.40f, 0.55f, 0.80f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.20f, 0.25f, 0.30f, 0.60f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.40f, 0.40f, 0.80f, 0.30f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.80f, 0.40f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.80f, 0.50f, 0.50f, 0.40f);
    c[ImGuiCol_CheckMark] = ImVec4(0.90f, 0.90f, 0.90f, 0.50f);
    c[ImGuiCol_SliderGrab] = ImVec4(1.00f, 1.00f, 1.00f, 0.30f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.80f, 0.50f, 0.50f, 1.00f);
    c[ImGuiCol_Button] = ImVec4(0.67f, 0.40f, 0.40f, 0.60f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.67f, 0.40f, 0.40f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.80f, 0.50f, 0.50f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(0.40f, 0.40f, 0.90f, 0.45f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.45f, 0.45f, 0.90f, 0.80f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.53f, 0.53f, 0.87f, 0.80f);
    c[ImGuiCol_Separator] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    c[ImGuiCol_SeparatorHovered] = ImVec4(0.60f, 0.60f, 0.70f, 1.00f);
    c[ImGuiCol_SeparatorActive] = ImVec4(0.70f, 0.70f, 0.90f, 1.00f);
    c[ImGuiCol_ResizeGrip] = ImVec4(1.00f, 1.00f, 1.00f, 0.30f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(1.00f, 1.00f, 1.00f, 0.60f);
    c[ImGuiCol_ResizeGripActive] = ImVec4(1.00f, 1.00f, 1.00f, 0.90f);
    c[ImGuiCol_Tab] = ImVec4(0.34f, 0.34f, 0.64f, 0.80f);
    c[ImGuiCol_TabHovered] = ImVec4(0.45f, 0.45f, 0.90f, 0.80f);
    c[ImGuiCol_TabSelected] = ImVec4(0.40f, 0.40f, 0.90f, 0.45f);
    c[ImGuiCol_TabSelectedOverline] = ImVec4(0.53f, 0.53f, 0.87f, 1.00f);
    c[ImGuiCol_TabDimmed] = ImVec4(0.24f, 0.24f, 0.50f, 0.60f);
    c[ImGuiCol_TabDimmedSelected] = ImVec4(0.30f, 0.30f, 0.60f, 0.60f);
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.53f, 0.53f, 0.87f, 0.00f);
    c[ImGuiCol_PlotLines] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_PlotLinesHovered] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.27f, 0.27f, 0.38f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.31f, 0.31f, 0.45f, 1.00f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.26f, 0.26f, 0.28f, 1.00f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.07f);
    c[ImGuiCol_TextLink] = c[ImGuiCol_HeaderActive];
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.00f, 0.00f, 1.00f, 0.35f);
    c[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    c[ImGuiCol_NavCursor] = c[ImGuiCol_HeaderHovered];
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.35f);
}

static void SrvDescriptorAlloc(ImGui_ImplDX12_InitInfo* info,
                               D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu,
                               D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
    UINT inc = info->Device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    UINT* next = (UINT*)info->UserData;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu =
        info->SrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu =
        info->SrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)(*next) * inc;
    gpu.ptr += (UINT64)(*next) * inc;
    *next += 1;
    *out_cpu = cpu;
    *out_gpu = gpu;
}

static void SrvDescriptorFree(ImGui_ImplDX12_InitInfo* info,
                              D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                              D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
    (void)info; (void)cpu; (void)gpu;
}

// Initialise / shut down the ImGui Direct3D backend matching the given
// renderer. Extracted so the backend can be re-created when the user switches
// GPU in the "Select GPU" window.
static bool initImguiD3D(Renderer* r) {
    if (r->backend() == Backend::D3D12) {
        ImguiD3D12Handles handles;
        if (!r->getImguiD3D12Handles(handles)) return false;
        ImGui_ImplDX12_InitInfo initInfo;
        initInfo.Device = handles.device;
        initInfo.CommandQueue = handles.queue;
        initInfo.NumFramesInFlight = handles.numFrames;
        initInfo.RTVFormat = handles.rtvFormat;
        initInfo.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        initInfo.SrvDescriptorHeap = handles.srvHeap;
        initInfo.SrvDescriptorAllocFn = SrvDescriptorAlloc;
        initInfo.SrvDescriptorFreeFn = SrvDescriptorFree;
        UINT next = 0;
        initInfo.UserData = &next;
        return ImGui_ImplDX12_Init(&initInfo);
    }
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (!r->getImguiD3D11Handles(&dev, &ctx)) return false;
    return ImGui_ImplDX11_Init(dev, ctx);
}

static void shutdownImguiD3D(Renderer* r) {
    if (!r) return;
    if (r->backend() == Backend::D3D12) {
        ImGui_ImplDX12_Shutdown();
    } else {
        ImGui_ImplDX11_Shutdown();
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv; // Only automatic selection is offered (like the original).
    SetUnhandledExceptionFilter(crashHandler);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        showFatalError("Could not initialize SDL.");
        return 1;
    }

    Log(LOG_INFO) << "Creating SDL_Window";
    // Keep the exact original window (1280x800, OS-chosen position) whenever it
    // fits on the display's usable area. Only when it would overflow (e.g. an
    // 800x600 screen) shrink it to fit and center it there.
    int winW = 1280, winH = 800;
    int winX = SDL_WINDOWPOS_UNDEFINED, winY = SDL_WINDOWPOS_UNDEFINED;
    SDL_Rect usable = {};
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0 &&
        usable.w > 0 && usable.h > 0 &&
        (usable.w < 1280 || usable.h < 800)) {
        winW = (usable.w < 1280) ? usable.w : 1280;
        winH = (usable.h < 800) ? usable.h : 800;
        winX = usable.x + (usable.w - winW) / 2;
        winY = usable.y + (usable.h - winH) / 2;
        Log(LOG_INFO) << "Usable display " << usable.w << "x" << usable.h
                      << " is smaller than 1280x800; using window "
                      << winW << "x" << winH;
    }
    SDL_Window* window = SDL_CreateWindow(
        "Shade Order Tester", winX, winY, winW, winH, SDL_WINDOW_RESIZABLE);
    if (!window) {
        showFatalError("Could not create window.");
        SDL_Quit();
        return 1;
    }

    SDL_DisplayMode dm;
    int refreshHz = 60;
    int dip = SDL_GetWindowDisplayIndex(window);
    if (SDL_GetCurrentDisplayMode(dip, &dm) == 0 && dm.refresh_rate > 0) {
        refreshHz = dm.refresh_rate;
    }
    initPacing(refreshHz);

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(window, &wmInfo)) {
        showFatalError("Could not retrieve window handle.");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    HWND hwnd = wmInfo.info.win.window;

    int clientW = winW, clientH = winH;
    SDL_GetWindowSize(window, &clientW, &clientH);

    int requestedAdapter = -1;  // -1 = automatic (best GPU)
    std::unique_ptr<Renderer> renderer =
        createRenderer(Backend::Auto, hwnd, clientW, clientH, requestedAdapter);
    if (!renderer) {
        showFatalError("No supported DirectX device found.\n"
                       "The fragment counting feature requires DirectX 11 or newer "
                       "hardware (SM5.0). Your GPU appears too old to support it.");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // The original Windows release binary uses imgui's classic gray theme.
    SetupClassicStyle();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "imgui.ini";
    // The original binary renders the Controls window at a slightly larger
    // scale than imgui's default 13px font (its window is 802x291 and the
    // content fills it). Scale the UI to match the original's proportions.
    io.FontGlobalScale = 1.5f;

    if (!ImGui_ImplSDL2_InitForD3D(window)) {
        showFatalError("Could not initialize ImGui (SDL2).");
        renderer.reset();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (!initImguiD3D(renderer.get())) {
        showFatalError("Could not initialize ImGui (Direct3D).");
        renderer.reset();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    Log(LOG_INFO) << "ImGui initialized for Direct3D";

    bool show_test_window = true;
    bool show_another_window = false;
    ImVec4 clear_color = ImColor(0, 0, 0);

    Log(LOG_INFO) << "Entering main loop";
    {
        bool done = false;

        int deltaX = 0, deltaY = 0;
        int prevX, prevY;
        SDL_GetMouseState(&prevX, &prevY);
        float frag_percentage = 0;
        float frag_speed = 1;
        int tri_count = 200;
        bool auto_increment = false;

        // "Select GPU" window (independent from Controls). Auto-opened at
        // startup; toggled again with F2 so the Controls layout stays identical
        // to the original.
        std::vector<AdapterInfo> adapters = enumerateAdapters();
        bool show_gpu_window = true;
        // GPU selection is debounced: choosing an adapter only records the
        // desired target; the actual switch runs once the selection has been
        // stable for a short time. This stops rapid clicking from recreating
        // the device every frame (which froze the app and slowed exit).
        int gpu_target = requestedAdapter;
        ULONGLONG gpu_target_changed = GetTickCount64();
        bool gpu_target_set = false;

        while (!done) {
            SDL_Event e;

            deltaX = 0;
            deltaY = 0;

            float deltaZoom = 0.0f;

            while (SDL_PollEvent(&e)) {
                bool handledByImGui = ImGui_ImplSDL2_ProcessEvent(&e);
                (void)handledByImGui;
                {
                    switch (e.type) {
                        case SDL_QUIT:
                            done = true;
                            break;
                        case SDL_WINDOWEVENT:
                            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                                renderer->resize(e.window.data1, e.window.data2);
                            }
                            break;
                        case SDL_MOUSEBUTTONDOWN:
                            prevX = e.button.x;
                            prevY = e.button.y;
                            break;
                        case SDL_MOUSEMOTION:
                            if (e.motion.state & SDL_BUTTON_LMASK) {
                                deltaX += prevX - e.motion.x;
                                deltaY += prevY - e.motion.y;
                                prevX = e.motion.x;
                                prevY = e.motion.y;
                            }
                            break;
                        case SDL_MULTIGESTURE:
                            if (e.mgesture.numFingers > 1) {
                                deltaZoom += e.mgesture.dDist * 10.0f;
                            }
                            break;
                        case SDL_MOUSEWHEEL:
                            deltaZoom += e.wheel.y / 100.0f;
                            break;
                        default:
                            break;
                    }
                }
            }
            if (io.WantTextInput) {
                SDL_StartTextInput();
            } else {
                SDL_StopTextInput();
            }
            // Exit immediately on SDL_QUIT instead of rendering/presenting one
            // more frame (presenting on a closing window can block).
            if (done) break;

            // Frame pacing: block until the next DWM composition (zero CPU,
            // synced to the display's real refresh rate). Skip it while the
            // window is hidden/minimized, where DWM would not compose and the
            // call would return immediately (letting the loop run uncapped).
            bool winVisible = !(SDL_GetWindowFlags(window) &
                                (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN));
            if (g_dwmFlush && winVisible) {
                g_dwmFlush();
            } else {
                paceToRefresh();
            }

            // Start the frame: for D3D12 this opens the command list, so the
            // ImGui backends can upload the font texture into it.
            renderer->setClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
            renderer->beginFrame();

            ImGui_ImplSDL2_NewFrame();
            if (renderer->backend() == Backend::D3D12) {
                ImGui_ImplDX12_NewFrame();
            } else {
                ImGui_ImplDX11_NewFrame();
            }
            ImGui::NewFrame();

            ImGui::Begin("Controls");

            // Runtime-detected values, labelled with the DX read source (mirroring
            // the original's glGetString(...) titles).
            // Keep labels short enough to fit the 802px window (like the original's
            // glGetString(...) titles), while still naming the DX read source.
            ImGui::Text("DXGI GetDesc (Vendor/Renderer): \"%s\"", renderer->info().adapterName.c_str());
            ImGui::Text("DXGI CheckInterfaceSupport (Driver): \"%s\"", renderer->info().driverVersion.c_str());
            const char* apiSource =
                (renderer->backend() == Backend::D3D12) ? "D3D12CreateDevice" : "D3D11CreateDevice";
            ImGui::Text("%s (API): \"%s\"", apiSource, renderer->info().apiName.c_str());

            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate,
                        ImGui::GetIO().Framerate);

            ImGui::ColorEdit3("clear color", (float*)&clear_color);

            ImGui::SliderFloat("Rendered/Screen %", &frag_percentage, 0, 500);
            if (ImGui::Button("0%")) {
                frag_percentage = 0;
            }
            ImGui::SameLine();
            if (ImGui::Button("100%")) {
                frag_percentage = 100;
            }

            ImGui::Checkbox("Auto Increment", &auto_increment);
            if (auto_increment) {
                ImGui::SliderFloat("ppf", &frag_speed, 0, 10, "%.1f%");
                frag_percentage += frag_speed;
            }

            ImGui::SliderInt("Tris", &tri_count, 0, 200);

            ImGui::Text("Frag Count <= %d\n", renderer->fragCount());

            ImGui::End();

            // Independent "Select GPU" window. Toggled with F2 so the Controls
            // window keeps the exact original layout.
            if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
                show_gpu_window = !show_gpu_window;
            }
            if (show_gpu_window) {
                const RendererInfo& cur = renderer->info();
                ImGui::SetNextWindowSize(ImVec2(620, 240), ImGuiCond_FirstUseEver);
                ImGui::Begin("Select GPU", &show_gpu_window);
                ImGui::Text("Current: \"%s\"  (%s)",
                            cur.adapterName.c_str(), cur.apiName.c_str());
                ImGui::TextDisabled("F2 to reopen this window.");
                ImGui::Separator();
                if (ImGui::Selectable("Auto (best GPU for this app)", requestedAdapter < 0)) {
                    if (gpu_target != -1) {
                        gpu_target = -1;
                        gpu_target_changed = GetTickCount64();
                        gpu_target_set = true;
                    }
                }
                for (const auto& a : adapters) {
                    char label[384];
                    snprintf(label, sizeof(label), "%s   [driver %s]%s",
                             a.name.c_str(), a.driverVersion.c_str(),
                             a.supportsD3D12 ? "" : "   (D3D11 only)");
                    if (ImGui::Selectable(label, requestedAdapter == a.index)) {
                        if (gpu_target != a.index) {
                            gpu_target = a.index;
                            gpu_target_changed = GetTickCount64();
                            gpu_target_set = true;
                        }
                    }
                }
                ImGui::End();
            }

            float percentage = frag_percentage;
            renderer->setFragCount((uint32_t)
                    (renderer->width() * renderer->height() *
                     percentage / 100.0 * renderer->triangleCount() / 2));

            renderer->draw();
            renderer->drawRandomTris(tri_count);

            ImGui::Render();
            if (renderer->backend() == Backend::D3D12) {
                ImguiD3D12Handles handles;
                renderer->getImguiD3D12Handles(handles);
                renderer->bindImguiSrvHeap();
                ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), handles.commandList);
            } else {
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            }

            renderer->present();

            // Apply the debounced GPU switch once the selection has settled
            // (300 ms without a newer choice). Debouncing coalesces rapid
            // clicking into a single device re-creation.
            if (gpu_target_set && gpu_target != requestedAdapter &&
                GetTickCount64() - gpu_target_changed >= 300) {
                gpu_target_set = false;
                int target = gpu_target;
                int prev = requestedAdapter;
                // Use the CURRENT client size: the window may have been
                // resized/maximized since startup, and a swapchain built at a
                // stale size would be stretched (UI offset + wrong hit-testing).
                int curW = clientW, curH = clientH;
                SDL_GetWindowSize(window, &curW, &curH);
                if (curW <= 0 || curH <= 0) { curW = clientW; curH = clientH; }
                shutdownImguiD3D(renderer.get());
                renderer.reset();
                renderer = createRenderer(Backend::Auto, hwnd, curW, curH, target);
                if (!renderer) {
                    Log(LOG_WARN) << "Requested GPU unavailable, reverting to automatic";
                    renderer = createRenderer(Backend::Auto, hwnd, curW, curH, -1);
                    target = -1;
                }
                if (!renderer && prev != -1) {
                    renderer = createRenderer(Backend::Auto, hwnd, curW, curH, prev);
                    target = prev;
                }
                if (!renderer) {
                    showFatalError("Failed to switch GPU (no usable Direct3D device).");
                    done = true;
                    break;
                }
                if (!initImguiD3D(renderer.get())) {
                    showFatalError("Could not re-initialize ImGui after switching GPU.");
                    done = true;
                    break;
                }
                requestedAdapter = target;
                // Keep the desired target in sync with what actually got
                // selected so a failed switch is not retried every 300 ms.
                gpu_target = requestedAdapter;
                Log(LOG_INFO) << "Switched GPU; requested adapter index = "
                              << requestedAdapter;
            }
        }
    }

    shutdownImguiD3D(renderer.get());
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    renderer.reset();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
