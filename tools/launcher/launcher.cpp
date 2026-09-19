#include <windows.h>
#include <string>
#include <cstdio>

// Resource IDs of the embedded per-architecture payloads (see launcher.rc).
#define IDR_X64_EXE   101
#define IDR_X64_SDL2  102
#define IDR_X86_EXE   103
#define IDR_X86_SDL2  104
#define IDR_ARM64_EXE 105

static bool extractResource(int id, const std::wstring& outPath) {
    HRSRC hRes = FindResourceW(NULL, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (!hRes) return false;
    HGLOBAL hLoad = LoadResource(NULL, hRes);
    if (!hLoad) return false;
    DWORD size = SizeofResource(NULL, hRes);
    const void* data = LockResource(hLoad);
    if (!data || size == 0) return false;
    HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(hFile, data, size, &written, NULL) && (written == size);
    CloseHandle(hFile);
    return ok;
}

static std::wstring getLauncherDir() {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring p(path);
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p = p.substr(0, pos);
    return p;
}

static void writeLauncherError(const std::wstring& dir, const char* msg) {
    std::wstring path = dir + L"\\trianglebin-launcher-error.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"w");
    if (f) {
        fprintf(f, "TriangleBin launcher error\n");
        fprintf(f, "Message: %s\n", msg ? msg : "");
        fclose(f);
    }
    MessageBoxA(NULL, msg, "TriangleBin (DirectX) 启动器", MB_OK | MB_ICONERROR);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    std::wstring launcherDir = getLauncherDir();

    // Determine the real (native) machine architecture.
    USHORT procMachine = 0, nativeMachine = 0;
    bool got = false;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    typedef BOOL (WINAPI *ppfnIsWow64Process2)(HANDLE, USHORT*, USHORT*);
    ppfnIsWow64Process2 pfn = (ppfnIsWow64Process2)GetProcAddress(k32, "IsWow64Process2");
    if (pfn && pfn(GetCurrentProcess(), &procMachine, &nativeMachine)) got = true;
    if (!got) {
        SYSTEM_INFO si = {};
        GetNativeSystemInfo(&si);
        switch (si.wProcessorArchitecture) {
            case PROCESSOR_ARCHITECTURE_AMD64: nativeMachine = IMAGE_FILE_MACHINE_AMD64; break;
            case PROCESSOR_ARCHITECTURE_ARM64: nativeMachine = IMAGE_FILE_MACHINE_ARM64; break;
            case PROCESSOR_ARCHITECTURE_ARM:   nativeMachine = IMAGE_FILE_MACHINE_ARM;   break;
            default: nativeMachine = IMAGE_FILE_MACHINE_I386; break;
        }
    }

    int exeId = 0, sdlId = 0;
    const char* archName = "?";
    if (nativeMachine == IMAGE_FILE_MACHINE_ARM64) { exeId = IDR_ARM64_EXE; sdlId = 0;    archName = "arm64"; }
    else if (nativeMachine == IMAGE_FILE_MACHINE_AMD64) { exeId = IDR_X64_EXE;  sdlId = IDR_X64_SDL2; archName = "x64"; }
    else if (nativeMachine == IMAGE_FILE_MACHINE_I386)  { exeId = IDR_X86_EXE;  sdlId = IDR_X86_SDL2; archName = "x86"; }
    else {
        writeLauncherError(launcherDir, "Unsupported CPU architecture.");
        return 1;
    }

    wchar_t tmpPath[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmpPath);
    wchar_t dir[MAX_PATH] = {0};
    wsprintfW(dir, L"%sTriBinDX-%lu", tmpPath, (unsigned long)GetCurrentProcessId());
    CreateDirectoryW(dir, NULL);

    std::wstring exePath = std::wstring(dir) + L"\\demo-dx.exe";
    if (!extractResource(exeId, exePath)) {
        writeLauncherError(launcherDir, "Failed to extract the demo-dx.exe payload.");
        return 1;
    }
    if (sdlId) {
        std::wstring sdlPath = std::wstring(dir) + L"\\SDL2.dll";
        if (!extractResource(sdlId, sdlPath)) {
            writeLauncherError(launcherDir, "Failed to extract the SDL2.dll payload.");
            return 1;
        }
    }

    // Run from the launcher directory so imgui.ini / diagnostic reports land
    // next to the launcher (portable behaviour).
    SetCurrentDirectoryW(launcherDir.c_str());

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exePath.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        char buf[192];
        snprintf(buf, sizeof(buf), "Failed to launch demo-dx (arch %s). Error %lu.",
                 archName, (unsigned)GetLastError());
        writeLauncherError(launcherDir, buf);
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (sdlId) DeleteFileW((std::wstring(dir) + L"\\SDL2.dll").c_str());
    DeleteFileW(exePath.c_str());
    RemoveDirectoryW(dir);
    return 0;
}
