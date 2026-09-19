# TriangleBin-DX

[English](README.md) | 中文

[TriangleBin](https://github.com/Swung0x48/TriangleBin) 的 DirectX 11 / 12 移植版，仅支持 Windows。

它会绘制一叠互相重叠的三角形，并统计 GPU 实际光栅化了多少个片段。这个数量可以判断你的
显卡是 **IMR**（立即模式渲染，每个三角形的每个覆盖像素都要着色）还是 **TBR**
（分块渲染，几何按 tile 分桶，每个像素基本只着色一次）。

## 与原项目的关系

这是一个移植版本，不是打补丁。核心思路、控制项和测量方式来自 Swung 0x48 的
[TriangleBin](https://github.com/Swung0x48/TriangleBin)（MIT），渲染器、构建系统和
平台支持则针对 DirectX 重写。

| | [TriangleBin](https://github.com/Swung0x48/TriangleBin) | TriangleBin-DX（本仓库） |
|---|---|---|
| 图形 API | OpenGL / OpenGL ES | Direct3D 12，失败时回退 Direct3D 11 |
| 平台 | Android（APK）+ Windows x64 | Windows：x86 / x64 / ARM64 |
| 构建 | Gradle + Android NDK | CMake + MSVC |
| 选择 GPU | 无 | 运行时可切换任意 DXGI 适配器（F2） |
| 启动失败诊断 | 无 | 自动生成诊断报告 |

## 本移植版新增的内容

**自动选择 API。** 优先创建 Direct3D 12 设备，失败则回退到 Direct3D 11 并在日志中说明。
与原版一样不提供手动指定。

**选择 GPU 窗口（F2）。** 列出所有硬件 DXGI 适配器及其驱动版本，不支持 D3D12 的会标注
`(D3D11 only)`。这在同时有核显和独显的笔记本上很有用——Windows 默认分配的那块不一定是你
想测的那块。`Auto (best GPU for this app)` 保持默认行为。切换适配器需要重建设备，因此界面
对连续点击做了 300 ms 防抖，合并成一次切换。

**诊断报告。** 启动遇到致命错误（例如找不到支持 DirectX 11 的设备）或发生未处理异常时，
程序会在可执行文件旁边生成 `trianglebin-error-report.txt`，内含系统版本与内部版本号、
CPU 架构、显卡名称、驱动版本、支持的最高 DirectX 特性级别，以及具体错误。排查启动闪退时
把这个文件发出来就够了。

**运行时数值标注。** 原版通过 `glGetString(...)` 显示的几项信息，现在标注了对应的 DirectX
调用来源：`DXGI GetDesc`（适配器）、`DXGI CheckInterfaceSupport`（驱动版本）、
`D3D12CreateDevice` / `D3D11CreateDevice`（所用设备）。

**ARM64 支持。** x86、x64、ARM64 三个配置共用同一份源码。ARM64 采用静态链接 SDL2
（`deps/SDL2/lib/arm64/SDL2-static.lib`），因为 MSVC 下无法链接 SDL2 的动态库；
`src/sdl2_atomic_shim.c` 负责补齐缺失的 `_Interlocked*` 符号。

**单文件启动器。** `tools/launcher` 会把 x64、x86、ARM64 三份程序作为资源嵌入同一个
可执行文件，运行时释放与当前 CPU 匹配的那一份。发布时因此只需要一个 `.exe`，而不用为每个
架构单独打包。

## 界面说明

| 控件 | 含义 |
|---|---|
| `Rendered/Screen %` | 已渲染片段数 / 屏幕像素数，以百分比表示，范围 0–500。 |
| `0%` / `100%` | 直接跳到 0% 或 100%。 |
| `Tris` | 随机三角形数量，0–200，不包含两个全屏三角形。 |
| `Auto Increment` | 每帧自动增加 `Rendered/Screen %`。 |
| `ppf` | 每帧增量：勾选 Auto Increment 后 `Rendered/Screen %` 每帧增加的数值，范围 0–10。 |
| `Frag Count` | 本帧 GPU 实际光栅化的片段数，应当不超过目标值。 |
| `clear color` | 背景色。 |
| `F2` | 打开 / 关闭选择 GPU 窗口。 |

怎么看结果：提高 `Tris`，观察 `Frag Count`。如果它随三角形数量持续上升，说明每个三角形的
覆盖像素都被着色，这是 IMR 的行为；如果它明显低于目标值并趋于平稳，说明几何被分桶、
每个像素每 tile 只着色一次，这是 TBR 的行为。

## 编译

环境要求：

- Windows 10 或 11
- Visual Studio 2022，需勾选 **使用 C++ 的桌面开发** 工作负载
- CMake 3.20 或更高版本

SDL2、Dear ImGui、GLM 已放在 `deps/` 下，无需包管理器。

```bash
git clone https://github.com/ddYIbb/TriangleBin-DX.git
cd TriangleBin-DX

cmake -S . -B build-x64 -G "Visual Studio 17 2022" -A x64
cmake --build build-x64 --config Release
```

产物为 `build-x64/demo-dx.exe`。运行前需要把 `deps/SDL2/lib/x64/SDL2.dll` 复制到它旁边。

其余架构只是换一下生成器平台和输出目录：

```bash
cmake -S . -B build-x86 -G "Visual Studio 17 2022" -A Win32
cmake --build build-x86 --config Release

cmake -S . -B build-arm64 -G "Visual Studio 17 2022" -A ARM64
cmake --build build-arm64 --config Release
```

ARM64 静态链接 SDL2，因此不需要 `SDL2.dll`。

### 启动器

`tools/launcher` 会把上面三个可执行文件和两份 `SDL2.dll` 嵌入资源（见 `launcher.rc`）。
请务必在三个架构都编译完成之后再配置它，因为资源编译器在构建时就要读取这些文件：

```bash
cmake -S tools/launcher -B tools/launcher/build -G "Visual Studio 17 2022" -A x64
cmake --build tools/launcher/build --config Release
```

产物是 `tools/launcher/build/demo-dx.exe`，单文件、可直接分发。

## 第三方组件

| 组件 | 版本 | 许可证 |
|---|---|---|
| [SDL2](https://github.com/libsdl-org/SDL) | 2.30.11 | zlib |
| [Dear ImGui](https://github.com/ocornut/imgui) | — | MIT |
| [GLM](https://github.com/g-truc/glm) | — | MIT |

## 许可协议

MIT，详见 [LICENSE](LICENSE)。

本项目基于 Swung 0x48 的 [TriangleBin](https://github.com/Swung0x48/TriangleBin)
（MIT 许可）修改而来，原作者的版权声明保留在 `LICENSE` 中。
