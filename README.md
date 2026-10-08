# display

[![CI](https://github.com/alya-lang/display/actions/workflows/ci.yml/badge.svg)](https://github.com/alya-lang/display/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/alya-lang/display?color=blue&label=License)](LICENSE)
[![Alya](https://img.shields.io/badge/dynamic/toml?url=https%3A%2F%2Fraw.githubusercontent.com%2Falya-lang%2Fdisplay%2Fmain%2Falya.toml&query=%24.package.alya-version&label=Alya&color=orange&prefix=%3E%3D)](https://github.com/alya-lang/alya)
[![Package Version](https://img.shields.io/badge/dynamic/toml?url=https%3A%2F%2Fraw.githubusercontent.com%2Falya-lang%2Fdisplay%2Fmain%2Falya.toml&query=%24.package.version&label=Version&color=brightgreen)](alya.toml)

Cross-platform display enumeration: monitors, resolution, DPI, scale, refresh rate, and color info for Alya

---

## 🌟 Features

- 🖥️ **Display Enumeration**: Monitor count, per-display records, primary detection, and name lookup on Windows, macOS, and Linux
- 📐 **Resolution & Geometry**: Device-pixel modes, virtual-desktop origins (negative-aware), bounding-box union, and total pixel counts
- 🔍 **DPI & Scale/Zoom**: Per-monitor DPI, scale percent (`100` = 1.00x, `200` = Retina), effective logical sizes, and HiDPI classification
- ⚡ **Refresh & Color Depth**: Milli-Hz refresh rates, bits-per-pixel, panel orientation (0/90/180/270), and OS color profile labels
- 🎨 **Color Profiles**: ICM filename on Windows, color-space label on macOS (`sRGB`, `Display P3`), honest `""` when unavailable
- 🧮 **Derived Metrics**: Effective sizes, physical inches, portrait/HiDPI/high-refresh filters, and max DPI/Hz queries (pure, headless-safe)
- 🛋️ **Headless-Safe**: `0` / `[]` / `null` on CI, Docker, and SSH — live assertions tolerate zero displays, synthetic vectors cover the rest
- 🧩 **Modular Architecture**: Layered multi-module design with a clean public facade (`src/lib.alya`) and canonical models (`src/types.alya`)
- 🔒 **Public/Private Visibility (`pub`)**: Fine-grained export control keeping internals encapsulated
- 🧪 **Enterprise Test & Benchmark Suite**: Real-hardware assertions (`std/test`) and micro-benchmarking
- 🚩 **Feature-Gated Extras**: Optional analytics slice (`hidpi_displays`, `scaling_report`, `layout_report`) via `[features]` and `@cfg`

---

## 📁 Project Architecture

```
display/
├── .alyalint               # Linter configuration (rules, exclusions, severity overrides)
├── .editorconfig           # Uniform formatting rules across IDEs and editors
├── .gitignore              # Ecosystem standard ignore filters
├── .vscode/                # VS Code workspace settings, DAP launch configurations & tasks
├── alya.toml               # Package manifest with dependencies, [features] and [build]
├── c/                      # Native C sources for zero-dependency FFI packages
│   ├── display.h           # Shared native declarations (count/at wire contract)
│   ├── display.c           # Common engine (smoke test + unknown-target stubs)
│   ├── win32_display.c     # Windows EnumDisplayMonitors/DPI/ICM backend
│   ├── cocoa_display.c     # macOS CoreGraphics backend (bounds/mode/DPI/color)
│   └── linux_display.c     # Linux X11 + dlopen(XRandR) backend with screen fallback
├── src/
│   ├── lib.alya            # Public API facade (enumeration, metrics, snapshots)
│   ├── types.alya          # Data models (DisplayInfo, DisplayLayout, enums, methods)
│   ├── ffi.alya            # Native extern "C" declarations
│   ├── display.alya        # Enumeration, wire parsing, primary/layout queries
│   ├── metrics.alya        # Derived metrics (effective size, filters, maxima)
│   └── core/               # Subdirectory module hierarchy
│       ├── strutil.alya    # FFI string ownership helper (str_owned)
│       ├── formatter.alya  # Scale/DPI/refresh/snapshot formatters
│       └── extras.alya     # Feature-gated analytics (HiDPI, scaling, layout reports)
├── examples/
│   └── demo.alya           # Runnable walkthrough of all package capabilities
├── tests/
│   └── test_basic.alya     # Automated test suite (deterministic + headless-safe live)
└── benches/
    └── bench_basic.alya    # Micro-benchmarks measuring performance and throughput
```

> [!NOTE]
> **Visibility & Modularity:** Symbols annotated with `pub` (`pub function`, `pub struct`, `pub enum`, `pub interface`) are exported to external consumers and re-exporting modules. Symbols without `pub` remain strictly internal to their declaring module, preventing symbol collisions and implementation leakage.

---

## 📦 Installation

Add `display` to the `[dependencies]` section in your `alya.toml`:

```toml
[dependencies]
display = { git = "https://github.com/alya-lang/display", branch = "main" }
```

Or install it directly using the Alya package CLI:

```bash
alya add display --git https://github.com/alya-lang/display --branch main
alya install
```

### Package Features

| Feature | Default | Description |
|:---|:---:|:---|
| `extras` | ✅ | Advanced analytics (`hidpi_displays`, `portrait_displays`, `high_refresh_displays`, `scaling_report`, `layout_report`). |

`summary()` and `details()` work without any feature. Enumeration, metrics, and formatters always work.

```bash
# Full build (default)
alya install
alya test

# Slim build (core enumeration only)
alya install --no-default-features
alya test --no-default-features
```

---

## 🚀 Quick Start

```alya
import "display" as pkg

function main()
    # 1. Live enumeration (headless-safe: [] on CI/Docker/SSH)
    let list = pkg::displays()
    say f"Count: {pkg::display_count()}"

    # 2. Primary display + virtual desktop
    let prim = pkg::primary_or_first()
    if prim is not null
        say f"Primary: {pkg::format_display(prim)}"
    end
    say f"Layout:  {pkg::layout().summary()}"

    # 3. Synthetic record (works everywhere)
    let hd = pkg::make_display_info("HDMI-1", 0, 0, 1920, 1080, 100, 96, 60000, 32, 1, 0, "sRGB")
    say f"Demo:    {pkg::format_display(hd)}"
end

main()
```

---

## 📖 API Reference

| Symbol | Visibility | Description |
|---|---|---|
| `displays()` | `pub function` | Display records for every monitor (possibly empty headless). |
| `display_count()` | `pub function` | Number of enumerated displays (0 when headless). |
| `display_at(i)` | `pub function` | Display at enumeration index `i`, or null when missing. |
| `primary_index()` | `pub function` | Enumeration index of the primary display (-1 when none). |
| `primary()` | `pub function` | Primary display record, or null when none. |
| `primary_or_first()` | `pub function` | Primary display, or first as fallback (null when none). |
| `has_display()` | `pub function` | 1 when at least one display is reachable, 0 otherwise. |
| `headless()` | `pub function` | 1 when no display server is reachable, 0 otherwise. |
| `names()` | `pub function` | All display names in enumeration order. |
| `find(name)` | `pub function` | Display with the given OS name, or null when absent. |
| `largest()` | `pub function` | Largest display by pixel area (null when none). |
| `total_px()` | `pub function` | Sum of all display pixel areas (0 when headless). |
| `layout()` | `pub function` | Virtual-desktop bounding box over all displays. |
| `effective(d)` | `pub function` | Effective logical size `[w, h]` after scale division. |
| `max_hz(list)` | `pub function` | Highest refresh rate in Hz (-1.0 when none known). |
| `max_display_dpi(list)` | `pub function` | Highest DPI in the list (-1 when none known). |
| `effective_size(d)` | `pub function` | Effective size `[w, h]` (unknown scale keeps pixels). |
| `has_dpi(d)` | `pub function` | 1 when the display reports a known DPI. |
| `has_refresh(d)` | `pub function` | 1 when the display reports a known refresh rate. |
| `has_color_profile(d)` | `pub function` | 1 when the display reports a color profile label. |
| `portrait_only(list)` | `pub function` | Filters to portrait panels (90/270 degrees). |
| `hidpi_only(list)` | `pub function` | Filters to HiDPI panels (scale >= 150%). |
| `refresh_at_least(list, min_hz)` | `pub function` | Filters to panels at or above `min_hz`. |
| `max_refresh_hz(list)` | `pub function` | Highest refresh rate in Hz (-1.0 when none known). |
| `max_dpi(list)` | `pub function` | Highest DPI in the list (-1 when none known). |
| `summary()` | `pub function` | One-line human-readable display snapshot. |
| `details()` | `pub function` | Detailed multi-line display report. |
| `format_scale(n)` | `pub function` | Formats scale percent as `"1.5x"`, `"unknown"` for -1. |
| `format_dpi(dpi)` | `pub function` | Formats DPI as `"96 dpi"`. |
| `format_refresh(milliHz)` | `pub function` | Formats refresh as `"60Hz"`, `"59.94Hz"`. |
| `format_display(d)` | `pub function` | One-line display overview with mode/Hz/scale/DPI/profile. |
| `format_display_list(list)` | `pub function` | One entry per line (`"(no displays)"` when empty). |
| `format_layout(layout)` | `pub function` | Formats a virtual-desktop layout summary. |
| `format_snapshot(list)` | `pub function` | Full one-line snapshot over a display list. |
| `format_report(list, layout)` | `pub function` | Multi-line report (per display + layout line). |
| `make_display_info(...)` | `pub function` | Full constructor for `DisplayInfo` (testing/snapshots). |
| `empty_display()` | `pub function` | Null-object unknown `DisplayInfo` record. |
| `make_display_layout(...)` | `pub function` | Constructor for `DisplayLayout`. |
| `hidpi_displays()` | `pub function` (`extras` feature, default-on) | HiDPI displays in enumeration order. |
| `portrait_displays()` | `pub function` (`extras` feature, default-on) | Portrait displays in enumeration order. |
| `high_refresh_displays(min_hz)` | `pub function` (`extras` feature, default-on) | Displays at or above `min_hz` (default 120Hz). |
| `scaling_report()` | `pub function` (`extras` feature, default-on) | One-line scaling report (HiDPI share, max scale/DPI). |
| `layout_report()` | `pub function` (`extras` feature, default-on) | One-line layout diagnostic (desktop + primary + px). |
| `c_add(a, b)` | `pub function` | Bundled C engine smoke test via FFI. |
| `DisplayInfo` | `pub struct` | Display model (`name`, `x`, `y`, `w`, `h`, `scale_x100`, `dpi`, `refresh_milliHz`, `bpp`, `primary`, `orientation`, `color`). |
| `DisplayInfo.is_known()` | `pub method` | 1 when mode is known (`w > 0` and `h > 0`). |
| `DisplayInfo.is_primary()` | `pub method` | 1 when primary, 0 otherwise. |
| `DisplayInfo.area()` | `pub method` | Pixel area (`w * h`), 0 when unknown. |
| `DisplayInfo.scale()` | `pub method` | Scale factor (1.0, 2.0, -1.0 unknown). |
| `DisplayInfo.refresh_hz()` | `pub method` | Refresh in Hz (-1.0 unknown). |
| `DisplayInfo.is_hidpi()` | `pub method` | 1 when scale >= 150%. |
| `DisplayInfo.is_portrait()` | `pub method` | 1 when orientation is 90/270. |
| `DisplayInfo.resolution()` | `pub method` | Mode string (`"1920x1080"`, `"unknown"`). |
| `DisplayInfo.summary()` | `pub method` | One-line summary (satisfies `Summarizable`). |
| `DisplayInfo.describe()` | `pub method` | Detailed description (satisfies `Describable`). |
| `DisplayInfo.is_valid()` | `pub method` | Usability guard (satisfies `Describable`). |
| `DisplayLayout` | `pub struct` | Virtual-desktop model (`x`, `y`, `w`, `h`, `count`, `primary_name`). |
| `DisplayLayout.is_known()` | `pub method` | 1 when covering at least one display. |
| `DisplayLayout.area()` | `pub method` | Desktop area, 0 when unknown. |
| `DisplayLayout.summary()` | `pub method` | One-line summary (satisfies `Summarizable`). |
| `DisplayLayout.describe()` | `pub method` | Detailed description (satisfies `Describable`). |
| `DisplayLayout.is_valid()` | `pub method` | Usability guard (satisfies `Describable`). |
| `DisplayOrientation` | `pub enum` | Rotations (`Landscape = 0`, `Portrait = 90`, `Inverted = 180`, `Flipped = 270`). |
| `DisplayScale` | `pub enum` | Buckets (`Native = 100`, `Scaled = 125`, `HiDpi = 150`, `Retina = 200`). |
| `Summarizable` | `pub interface` | Structural contract requiring `summary(self) -> string`. |
| `Describable` | `pub interface` | Structural contract requiring `describe(self) -> string` and `is_valid(self) -> int`. |

> [!TIP]
> **Internal Helpers & Documentation:** Public symbols are documented with `##` Markdown docstrings, enabling automatic API documentation generation via `alya doc`. Private helpers remain encapsulated without `pub`.
>
> [!NOTE]
> **FFI string ownership:** Native `str` results are zero-copy views into C static buffers. Every stored or returned FFI string is pinned to a heap copy at the module boundary via `str_owned()` (`src/core/strutil.alya`), so snapshots stay valid across further FFI calls.

---

### 🖥️ Platform Coverage

| Capability | Windows | macOS | Linux |
|---|---|---|---|
| Enumeration | `EnumDisplayMonitors` | `CGGetActiveDisplayList` | XRandR via `dlopen` + X-screen fallback |
| Bounds/origin | `GetMonitorInfo` (`rcMonitor`) | `CGDisplayBounds` | CRTC `x/y/width/height` |
| Primary | `MONITORINFOF_PRIMARY` | `CGDisplayIsMain` | `XRRGetOutputPrimary` (first fallback) |
| DPI | `GetDpiForMonitor` → `GetDeviceCaps` | Physical size (`CGDisplayScreenSize`) | Output `mm_width` → screen average |
| Scale | `dpi * 100 / 96` | Pixel ÷ points width (Retina 200) | `GDK_SCALE` or 100 (honest default) |
| Refresh | `ENUM_CURRENT_SETTINGS` frequency | `CGDisplayMode` refresh | RandR mode `dotClock/(h*v)` |
| Bit depth | `dmBitsPerPel` | Pixel-encoding string | `DefaultDepth` |
| Orientation | `dmDisplayOrientation` | `CGDisplayRotation` | CRTC rotation bits |
| Color profile | ICM filename (`GetICMProfile`) | Color-space name (`sRGB`, `Display P3`) | — (`""`, X11 has no standard query) |
| Headless | 0 monitors | 0 displays | No `DISPLAY` or `XOpenDisplay` fails → 0 |

---

## 🧪 Running Tests & Benchmarks

Run the automated test suite using `alya test`:

```bash
alya test
```

Exercise feature selection (the `extras` slice is default-on):

```bash
alya test --features extras
alya test --no-default-features
```

Generate static API documentation:

```bash
alya doc . -o docs --markdown
```

Run the benchmark suite:

```bash
alya run benches/bench_basic.alya
```

Run the example demo:

```bash
alya run examples/demo.alya
```

Check code formatting:

```bash
alya fmt . --check
```

Run static code linter:

```bash
alya lint . --check
```

---

### 💻 Developer Tooling & VS Code Integration

This package comes preconfigured with recommended workspace settings and tasks for **Visual Studio Code**:
- **LSP & Formatting**: Auto-formatting on save and real-time Language Server diagnostics via `alya-lang.vscode-alya`.
- **DAP Debugging**: Launch configurations in `.vscode/launch.json` ready for interactive step-debugging via `F5`.
- **Predefined Tasks**: Press `Ctrl+Shift+B` or run tasks (`Test`, `Lint`, `Format`, `Build Docs`) directly from the Command Palette.

---

## 🤝 Contributing

Contributions are welcome! Please follow these steps:

1. Fork the repository and clone it locally
2. Install dependencies:
   ```bash
   alya install
   ```
3. Create your feature branch (`git checkout -b feature/my-feature`)
4. Verify tests and formatting before opening a PR:
   ```bash
   alya test
   ```
5. Commit your changes (`git commit -m "feat: add feature"`) and open a Pull Request

---

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
