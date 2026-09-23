//! Can this host run the Direct3D executor, and how well? (ADR-013)
//!
//! The paravirtual D3D device's executor is DXVK (ADR-007), and DXVK 3.1
//! wants **Vulkan 1.3**: `DxvkVulkanApiVersion = VK_API_VERSION_1_3` goes
//! into `VkApplicationInfo::apiVersion` at instance creation, which a
//! pre-1.3 loader answers with `ERROR_INCOMPATIBLE_DRIVER`, and every
//! adapter whose `properties.apiVersion` is below it returns early out of
//! `DxvkDeviceCapabilities` with no capabilities at all. A host that
//! fails either check boots machines normally with no 3D, and QEMU's
//! `d3dpt_exec_load.c` writes "no executor" into a log nobody reads. The
//! launcher asks the same question first, so the answer can be a
//! sentence in a window.
//!
//! **A software Vulkan driver counts.** lavapipe answers 1.3 on any CPU,
//! and DXVK ranks a `CPU` device last but never excludes it, so the
//! executor runs there, slowly, because a software rasteriser competes
//! for the host CPU that TCG is already using for the guest. The verdict
//! is "available", and [`HostGpu::is_slow`] puts a warning next to it.
//!
//! Two other details:
//!
//! * The loader is opened **dynamically** (`Entry::load`). A box with no
//!   `libvulkan` gets a report, not a failed start, and the launcher has
//!   to run on exactly those boxes to explain itself.
//! * `VK_KHR_portability_enumeration` is enabled when the loader offers
//!   it, as DXVK does. Without it a Vulkan-on-Metal driver (KosmicKrisp,
//!   MoltenVK) is invisible and a Mac would be told it has no Vulkan.
//!
//! The probe decides nothing for the machine. A host below the bar still
//! runs every guest (see [`D3dBackend`] for which Direct3D 9 it gets).
//! The probe picks the sentence, not the stack.

use ash::vk;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::OnceLock;

/// What the host can offer the D3D device, worst to best.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HostGpu {
    /// No Vulkan loader on the box (no `libvulkan`, or it would not load).
    NoLoader,
    /// A loader, but it answers below 1.3, so DXVK's `vkCreateInstance`
    /// would fail with `ERROR_INCOMPATIBLE_DRIVER`.
    LoaderTooOld,
    /// A 1.3 loader that enumerates nothing: no ICD, or every driver
    /// filtered out.
    NoDevice,
    /// Devices, but none of them reaches 1.3. DXVK's own words for this
    /// are "No adapters found … A Vulkan 1.3 capable setup is required."
    DeviceTooOld,
    /// The only device at 1.3 is a software rasteriser. The executor
    /// runs; it will be very slow.
    SoftwareOnly,
    /// A hardware device at 1.3 or newer: the executor will run.
    Accelerated,
}

impl HostGpu {
    /// Whether the paravirtual Direct3D device will work at all. True for
    /// a software driver too; [`is_slow`](Self::is_slow) is the
    /// difference the caller should show.
    pub fn d3d_available(self) -> bool {
        matches!(self, HostGpu::Accelerated | HostGpu::SoftwareOnly)
    }

    /// Available, but on a software rasteriser: worth a warning next to
    /// the verdict rather than a refusal in place of it.
    pub fn is_slow(self) -> bool {
        self == HostGpu::SoftwareOnly
    }

    /// One line for a window, in the second person, saying what this host
    /// does rather than what it lacks.
    pub fn headline(self) -> &'static str {
        match self {
            HostGpu::Accelerated => "Direct3D pass-through available (Vulkan 1.3 GPU).",
            HostGpu::SoftwareOnly => {
                "Direct3D pass-through runs on a software Vulkan driver here. Expect it to be very slow."
            }
            HostGpu::DeviceTooOld => "This GPU is below Vulkan 1.3, so there is no Direct3D pass-through here.",
            HostGpu::NoDevice => "No Vulkan device found, so there is no Direct3D pass-through here.",
            HostGpu::LoaderTooOld => "Vulkan on this host is older than 1.3, so there is no Direct3D pass-through here.",
            HostGpu::NoLoader => "No Vulkan on this host, so there is no Direct3D pass-through here.",
        }
    }
}

/// Which Direct3D 9 the executor will run on here. On Windows there is a
/// second answer (ADR-007's second amendment), and on Linux and macOS a
/// third (ADR-018, track M15: the same executor in another process, on
/// Wine's own d3d9).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum D3dBackend {
    /// DXVK: this host has the Vulkan 1.3 device it wants.
    Dxvk,
    /// Windows' own `d3d9.dll`. On an old card its D3D9 driver is what
    /// the card was sold for, but it is a second rasteriser, so a frame
    /// here is not the frame the goldens were taken with.
    System,
    /// Wine's own `d3d9.dll` (WineD3D over OpenGL), in a Wine process
    /// beside QEMU: a Linux or macOS host below the floor that has a
    /// Wine installed and the executor's Windows build to run on it
    /// (`d3dpt-exec-host.exe`, `scripts/build-d3dpt-exec.sh --wine`).
    Wine,
    /// Nothing: no Vulkan 1.3, no system Direct3D 9 because this is not
    /// Windows, and no Wine to run the executor on. The guest has no
    /// Direct3D pass-through; OpenGL and Glide still work (doc 04).
    None,
}

/// A Wine on this host, for the executor in another process.
#[derive(Debug, Clone)]
pub struct Wine {
    pub path: PathBuf,
    /// What `wine --version` said, without the `wine-` it starts with.
    pub version: String,
}

/// The rule `d3dpt/exec/d3dpt_exec_remote.c`'s `find_wine` follows, kept
/// in step by hand (that library is C inside QEMU). `D3DPT_WINE` first;
/// set to a path that does not exist it means none, so a test can take
/// Wine away on a host that has one. Then the Mac apps by their fixed
/// paths, the spike's tarball in a checkout, then `wine64` and `wine` on
/// `PATH`. Never on Windows, where the host's own Direct3D 9 is the
/// fallback.
fn find_wine() -> Option<PathBuf> {
    if cfg!(windows) {
        return None;
    }
    if let Some(env) = std::env::var_os("D3DPT_WINE") {
        let p = PathBuf::from(env);
        return p.is_file().then_some(p);
    }
    let mut fixed: Vec<PathBuf> = [
        "/Applications/Wine Stable.app/Contents/Resources/wine/bin/wine",
        "/Applications/Wine Staging.app/Contents/Resources/wine/bin/wine",
        "/Applications/Wine Devel.app/Contents/Resources/wine/bin/wine",
    ]
    .iter()
    .map(PathBuf::from)
    .collect();
    if crate::paths::install_prefix().is_none() {
        fixed.push(crate::paths::checkout("build/wine/Wine Staging.app/Contents/Resources/wine/bin/wine"));
        fixed.push(crate::paths::checkout("build/wine/Wine Stable.app/Contents/Resources/wine/bin/wine"));
    }
    if let Some(p) = fixed.into_iter().find(|p| p.is_file()) {
        return Some(p);
    }
    let path = std::env::var_os("PATH")?;
    for name in ["wine64", "wine"] {
        for dir in std::env::split_paths(&path) {
            let p = dir.join(name);
            if p.is_file() {
                return Some(p);
            }
        }
    }
    None
}

/// `wine --version` prints `wine-10.0` (or `wine-11.17 (Staging)`) and
/// exits; it starts no server and makes no prefix. A Wine that cannot
/// do that counts as no Wine.
fn wine_version(path: &Path) -> Option<String> {
    let out = Command::new(path).arg("--version").output().ok()?;
    if !out.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&out.stdout);
    let line = text.lines().next()?.trim();
    Some(line.strip_prefix("wine-").unwrap_or(line).to_string())
}

/// The Wine the executor's other process would run on, probed once per
/// process like the GPU.
pub fn wine() -> Option<&'static Wine> {
    static ONCE: OnceLock<Option<Wine>> = OnceLock::new();
    ONCE.get_or_init(|| {
        let path = find_wine()?;
        let version = wine_version(&path)?;
        Some(Wine { path, version })
    })
    .as_ref()
}

/// The host program that process runs, with the executor's Windows
/// build beside it: `lib/2ksbox/wine/` in a package, `build/d3dpt/wine/`
/// in a checkout (`scripts/build-d3dpt-exec.sh --wine`, mingw-w64). Its
/// absence is a build without mingw, not a host without Wine.
pub fn exec_host() -> Option<PathBuf> {
    if cfg!(windows) {
        return None;
    }
    let p = crate::paths::resource("lib/2ksbox/wine/d3dpt-exec-host.exe", "build/d3dpt/wine/d3dpt-exec-host.exe");
    p.is_file().then_some(p)
}

/// One line saying how to get a Wine here, for the note and the report.
fn wine_install_hint() -> &'static str {
    if cfg!(target_os = "macos") {
        "Install Wine (WineHQ's macOS build, or CrossOver) for Direct3D through it on this Mac."
    } else {
        "Install Wine (your distribution's wine package) for Direct3D through it on this host."
    }
}

impl HostGpu {
    /// What the pass-through will run on, on this host. On Windows a
    /// software Vulkan device loses to the system's Direct3D 9: DXVK does
    /// run on lavapipe, but a real card's own Direct3D 9 driver is faster
    /// than a CPU rasteriser, and a Windows host with software Vulkan
    /// still has that driver.
    ///
    /// On Linux and macOS the answer below the bar is Wine on the host
    /// (ADR-018), when there is one and the executor's Windows build to
    /// run on it. This is the order QEMU's own loader takes
    /// (`d3dpt_exec_load.c`: DXVK when it finds any device, the software
    /// one included, Wine when it finds none), so what this says is what
    /// the machine gets.
    pub fn backend(self) -> D3dBackend {
        if self.d3d_available() && !self.is_slow() {
            D3dBackend::Dxvk
        } else if cfg!(windows) {
            D3dBackend::System
        } else if self.d3d_available() {
            D3dBackend::Dxvk // software Vulkan, and DXVK takes it before Wine
        } else if wine().is_some() && exec_host().is_some() {
            D3dBackend::Wine
        } else {
            D3dBackend::None
        }
    }

    /// Whether the guest gets Direct3D at all on this host, on whichever
    /// back end; `--host-check`'s exit code answers it. Not
    /// [`d3d_available`](Self::d3d_available), which is the Vulkan
    /// question alone and decides the Windows fallback.
    pub fn pass_through_available(self) -> bool {
        self.backend() != D3dBackend::None
    }

    /// The headline for the pass-through. On a Windows host below the bar
    /// it is not the Vulkan sentence: the answer there is yes, through
    /// another library, and "no Direct3D pass-through" would be false.
    pub fn d3d_headline(self) -> String {
        match self.backend() {
            D3dBackend::System if self.is_slow() => {
                "Direct3D pass-through runs on this PC's own Direct3D 9 (the only Vulkan here is software).".into()
            }
            D3dBackend::System => {
                "Direct3D pass-through runs on this PC's own Direct3D 9 (no Vulkan 1.3 here).".into()
            }
            D3dBackend::Wine => {
                let v = wine().map(|w| w.version.as_str()).unwrap_or("?");
                format!(
                    "Direct3D pass-through runs through Wine on this host (Wine {v}, OpenGL; no Vulkan 1.3 here). Expect it to be slower than on a Vulkan GPU."
                )
            }
            _ => self.headline().into(),
        }
    }

    /// The second line, the same way: what a host on another back end
    /// trades, or, with none, the Wine to install. `None` when the host
    /// has a real GPU and there is nothing to say.
    pub fn d3d_advice(self) -> Option<String> {
        match self.backend() {
            D3dBackend::System => Some(
                "That is the card's own driver, not the tested DXVK path. If a game draws wrong, set Direct3D to DXVK and compare.".into(),
            ),
            D3dBackend::Wine => Some(
                "That is Wine's Direct3D 9 over OpenGL, not the tested DXVK path. A game that draws wrong here may be right on a Vulkan 1.3 host.".into(),
            ),
            // Software Vulkan with a Wine at hand: two working stacks, and
            // only the box can say which is faster (ADR-013).
            D3dBackend::Dxvk if self.is_slow() && wine().is_some() && exec_host().is_some() => Some(
                "Direct3D through Wine may be faster here. To try it, add -global d3dpt-vga.exec=wine to the machine's extra QEMU arguments.".into(),
            ),
            D3dBackend::None => Some(wine_install_hint().into()),
            _ => None,
        }
    }

    /// The word `--host-check` and the grid print.
    pub fn verdict_word(self) -> &'static str {
        match (self.backend(), self.is_slow()) {
            (D3dBackend::System, _) => "available, on this PC's own Direct3D 9",
            (D3dBackend::Wine, _) => "available, through Wine on this host (OpenGL)",
            (D3dBackend::Dxvk, true) => "available, in software (slow)",
            (D3dBackend::Dxvk, false) => "available",
            (D3dBackend::None, _) => "unavailable",
        }
    }
}

/// One physical device as the loader reports it.
#[derive(Debug, Clone)]
pub struct Device {
    pub name: String,
    pub kind: vk::PhysicalDeviceType,
    /// `apiVersion` as (major, minor, patch).
    pub api: (u32, u32, u32),
}

impl Device {
    /// Vulkan 1.3 or newer, which is what DXVK checks per adapter.
    pub fn meets_bar(&self) -> bool {
        (self.api.0, self.api.1) >= (1, 3)
    }

    pub fn is_software(&self) -> bool {
        self.kind == vk::PhysicalDeviceType::CPU
    }

    pub fn kind_name(&self) -> &'static str {
        match self.kind {
            vk::PhysicalDeviceType::DISCRETE_GPU => "discrete GPU",
            vk::PhysicalDeviceType::INTEGRATED_GPU => "integrated GPU",
            vk::PhysicalDeviceType::VIRTUAL_GPU => "virtual GPU",
            vk::PhysicalDeviceType::CPU => "software",
            _ => "other",
        }
    }
}

/// The whole answer: the verdict, and the evidence behind it.
#[derive(Debug, Clone)]
pub struct Probe {
    pub gpu: HostGpu,
    /// The loader's own version, `None` when there is no loader.
    pub loader: Option<(u32, u32, u32)>,
    /// Whether that loader is the package's own copy rather than the
    /// system's ([`shipped_loader`]).
    pub own_loader: bool,
    pub devices: Vec<Device>,
}

/// The Vulkan loader this package carries, when it carries one. Stock
/// macOS has no Vulkan, so the app ships the LunarG loader beside the
/// executor's KosmicKrisp (`scripts/package-macos.sh`). A Linux package
/// ships none and uses the distribution's. `None` in a checkout.
pub fn shipped_loader() -> Option<PathBuf> {
    if cfg!(target_os = "macos") {
        crate::paths::shipped("lib/2ksbox/libvulkan.1.dylib")
    } else {
        None
    }
}

/// The ICD manifest naming the driver the package carries, the file the
/// player's `companions::announce` hands QEMU's process as
/// `VK_DRIVER_FILES`. Same rule, same file.
pub fn shipped_icd() -> Option<PathBuf> {
    if cfg!(target_os = "macos") {
        crate::paths::shipped("share/2ksbox/vulkan/icd.d/driver.json")
    } else {
        None
    }
}

/// Point the loader at the driver the package ships, the way the player
/// does for QEMU's process: `VK_DRIVER_FILES` when the caller left both
/// loader variables unset. The loader reads its driver list only from
/// the environment and a few system directories, none of which an app
/// bundle owns. Without this the app's own loader, opened by [`probe`],
/// enumerates no device on a Mac with no Vulkan installed, and the
/// launcher says Direct3D is unavailable while the player runs DXVK (seen
/// with the community app on macOS 15). **Every front end calls this
/// first thing in `main`**, before a thread exists: writing the
/// environment beside another thread's `getenv` is the one race Rust's
/// `set_var` cannot lock out.
pub fn announce_driver() {
    if std::env::var_os("VK_DRIVER_FILES").is_some() || std::env::var_os("VK_ICD_FILENAMES").is_some() {
        return;
    }
    let Some(icd) = shipped_icd() else { return };
    // SAFETY: the caller's contract above: main, before any thread.
    unsafe { std::env::set_var("VK_DRIVER_FILES", icd) };
}

/// The loader to probe: the package's own by its full path when it ships
/// one, the system's `libvulkan` otherwise. `Entry::load()` alone asks
/// dyld for `libvulkan.dylib` by leaf name, which reaches the app's copy
/// only through an rpath of the calling image and under a name the app
/// does not use. On macOS 15 the packaged launcher reported "Vulkan
/// loader: not present" beside the copy the executor was running on.
///
/// # Safety
/// As `Entry::load`: a library on the search path runs its initialisers.
unsafe fn load_entry() -> Result<(ash::Entry, bool), ash::LoadingError> {
    if let Some(path) = shipped_loader() {
        return unsafe { ash::Entry::load_from(path) }.map(|e| (e, true));
    }
    unsafe { ash::Entry::load() }.map(|e| (e, false))
}

fn split(v: u32) -> (u32, u32, u32) {
    (
        vk::api_version_major(v),
        vk::api_version_minor(v),
        vk::api_version_patch(v),
    )
}

/// Ask the host, now. Costs one throwaway `VkInstance` (a few ms), so
/// anything that draws should hold the answer instead: [`cached`], or
/// the copy a window's model took when it opened.
pub fn probe() -> Probe {
    let none = |gpu| Probe {
        gpu,
        loader: None,
        own_loader: false,
        devices: Vec::new(),
    };

    // SAFETY: `load_entry` dlopens the loader; unsafe because a hostile
    // `libvulkan` on the search path could do anything. Same call the
    // player and every other Vulkan app make.
    let (entry, own_loader) = match unsafe { load_entry() } {
        Ok(e) => e,
        Err(_) => return none(HostGpu::NoLoader),
    };

    // 1.0 loaders have no `vkEnumerateInstanceVersion` at all, which is
    // what `Ok(None)` means here.
    let loader = match unsafe { entry.try_enumerate_instance_version() } {
        Ok(Some(v)) => split(v),
        Ok(None) => (1, 0, 0),
        Err(_) => return none(HostGpu::NoLoader),
    };

    // Ask for no more than the loader has: DXVK asks for 1.3 flatly and
    // takes the `ERROR_INCOMPATIBLE_DRIVER`, but we want the device list
    // even from a host we are about to turn down.
    let want = if (loader.0, loader.1) >= (1, 3) {
        vk::API_VERSION_1_3
    } else {
        vk::API_VERSION_1_0
    };

    let portability = unsafe { entry.enumerate_instance_extension_properties(None) }
        .unwrap_or_default()
        .iter()
        .any(|e| e.extension_name_as_c_str() == Ok(vk::KHR_PORTABILITY_ENUMERATION_NAME));

    let app = vk::ApplicationInfo::default()
        .application_name(c"2ksbox host probe")
        .api_version(want);
    let exts = [vk::KHR_PORTABILITY_ENUMERATION_NAME.as_ptr()];
    let mut info = vk::InstanceCreateInfo::default().application_info(&app);
    if portability {
        info = info
            .enabled_extension_names(&exts)
            .flags(vk::InstanceCreateFlags::ENUMERATE_PORTABILITY_KHR);
    }

    // SAFETY: `info` and everything it points at outlive the call.
    let instance = match unsafe { entry.create_instance(&info, None) } {
        Ok(i) => i,
        Err(_) => {
            return Probe {
                gpu: if (loader.0, loader.1) >= (1, 3) {
                    HostGpu::NoDevice
                } else {
                    HostGpu::LoaderTooOld
                },
                loader: Some(loader),
                own_loader,
                devices: Vec::new(),
            }
        }
    };

    // SAFETY: `instance` is live until `destroy_instance` below, and
    // nothing here keeps a handle past it.
    let devices: Vec<Device> = unsafe {
        instance
            .enumerate_physical_devices()
            .unwrap_or_default()
            .into_iter()
            .map(|pd| {
                let p = instance.get_physical_device_properties(pd);
                Device {
                    name: p
                        .device_name_as_c_str()
                        .map(|s| s.to_string_lossy().into_owned())
                        .unwrap_or_else(|_| "(unnamed)".into()),
                    kind: p.device_type,
                    api: split(p.api_version),
                }
            })
            .collect()
    };
    unsafe { instance.destroy_instance(None) };

    let gpu = if (loader.0, loader.1) < (1, 3) {
        // Nothing below can rescue this: DXVK's instance would not exist.
        HostGpu::LoaderTooOld
    } else if devices.is_empty() {
        HostGpu::NoDevice
    } else if devices.iter().any(|d| d.meets_bar() && !d.is_software()) {
        // A hardware device is what DXVK's own adapter order picks first,
        // so an extra software driver on the box changes nothing.
        HostGpu::Accelerated
    } else if devices.iter().any(|d| d.meets_bar()) {
        HostGpu::SoftwareOnly
    } else {
        HostGpu::DeviceTooOld
    };

    Probe {
        gpu,
        loader: Some(loader),
        own_loader,
        devices,
    }
}

/// The same answer, probed once per process. Nothing about a host's GPU
/// changes while the launcher is open.
pub fn cached() -> &'static Probe {
    static ONCE: OnceLock<Probe> = OnceLock::new();
    ONCE.get_or_init(probe)
}

/// The report `--host-check` prints: the verdict, then every device the
/// loader offered and what it counted for.
pub fn report_text(p: &Probe) -> String {
    let mut s = String::new();
    s.push_str(&format!("Direct3D pass-through: {}\n", p.gpu.verdict_word()));
    s.push_str(&format!("{}\n", p.gpu.d3d_headline()));
    if let Some(a) = p.gpu.d3d_advice() {
        s.push_str(&format!("{}\n", a));
    }
    s.push('\n');
    match p.loader {
        Some((a, b, c)) if p.own_loader => s.push_str(&format!("Vulkan loader: {a}.{b}.{c} (the app's own)\n")),
        Some((a, b, c)) => s.push_str(&format!("Vulkan loader: {a}.{b}.{c}\n")),
        None => s.push_str("Vulkan loader: not present\n"),
    }
    if !cfg!(windows) {
        // The other process's two halves, for a host below the bar
        // (ADR-018): which Wine, and whether this build brought the
        // executor's Windows build to run on it.
        match wine() {
            Some(w) => s.push_str(&format!("Wine: {} ({})\n", w.path.display(), w.version)),
            None => s.push_str("Wine: none found (D3DPT_WINE, wine64/wine on PATH, a Wine app in /Applications)\n"),
        }
        match exec_host() {
            Some(exe) => s.push_str(&format!("Executor for Wine: {}\n", exe.display())),
            None => s.push_str("Executor for Wine: not built (scripts/build-d3dpt-exec.sh --wine needs mingw-w64)\n"),
        }
    }
    s.push_str("Required: a 1.3 device (DXVK 3.1's minimum)\n");
    if p.devices.is_empty() {
        s.push_str("Devices: none\n");
    } else {
        s.push_str("Devices:\n");
        for d in &p.devices {
            let (a, b, c) = d.api;
            let why = if !d.meets_bar() {
                "below 1.3"
            } else if d.is_software() {
                "software, usable but slow"
            } else {
                "meets the bar"
            };
            s.push_str(&format!(
                "  {}: {}, Vulkan {a}.{b}.{c} ({why})\n",
                d.name,
                d.kind_name()
            ));
        }
    }
    s
}
