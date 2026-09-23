//! The question a keyboard close asks before the player pulls the machine's
//! plug. Alt+F4 lands on the player whenever the host's shortcuts are the
//! host's (Ctrl+Alt+K, or a host `kbcapture` can do nothing on), and pressed
//! by a hand that meant the game it was in, it took the whole machine with
//! it. A close with Alt held asks first; the title bar's button does not.
//!
//! Drawn by the player rather than borrowed from a toolkit: the player has
//! none, and Linux has no message box to call that also works inside the
//! Flatpak and over a full-screen window. A BGRA image in the VGA's own
//! 8x16 font (`vgafont16.bin` is QEMU's `ui/vgafont.h` as bytes), which
//! `Gpu` blends over the finished picture after the CRT chain, so it is
//! never shaded.

const FONT: &[u8; 4096] = include_bytes!("vgafont16.bin");
const CW: u32 = 8;
const CH: u32 = 16;
const PAD: u32 = 16;
const COLS: u32 = 42;
const LINES: [&str; 4] = [
    "Close the player?",
    "",
    "The machine turns off at once and any",
    "unsaved work in it is lost.",
];
/// The image at scale 1.
const W: u32 = 2 * PAD + COLS * CW;
const BUTTON_Y: u32 = PAD + (LINES.len() as u32 + 1) * CH;
const BUTTON_H: u32 = CH + 8;
const H: u32 = BUTTON_Y + BUTTON_H + PAD;

// 0xAARRGGBB, which little-endian is the texture's BGRA
const PANEL: u32 = 0xF01C1C22;
const BORDER: u32 = 0xFF8A8A96;
const TEXT: u32 = 0xFFEEEEEE;
const FACE: u32 = 0xFF33333C;
const EDGE: u32 = 0xFF9A9AA8;
const HOVER: u32 = 0xFF4A5A80;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Button {
    Close,
    Back,
}

/// Each button's label and rect (x, y, w, h) at scale 1: Back against the
/// right edge, Close to its left.
fn buttons() -> [(Button, &'static str, (u32, u32, u32, u32)); 2] {
    let (close, back) = ("Close (Enter)", "Back (Esc)");
    let bw = (back.len() as u32 + 2) * CW;
    let cw = (close.len() as u32 + 2) * CW;
    let bx = W - PAD - bw;
    let cx = bx - 2 * CW - cw;
    [
        (Button::Close, close, (cx, BUTTON_Y, cw, BUTTON_H)),
        (Button::Back, back, (bx, BUTTON_Y, bw, BUTTON_H)),
    ]
}

pub struct Prompt {
    /// The button under the pointer: drawn lit, and what a click answers.
    pub hover: Option<Button>,
    scale: u32,
}

impl Prompt {
    pub fn new() -> Self {
        Prompt { hover: None, scale: 1 }
    }

    /// The largest whole scale, up to twice the window's own, that leaves a
    /// tenth of the surface around the box.
    pub fn fit(&mut self, surface: (u32, u32), scale_factor: f64) {
        let mut s = (scale_factor * 2.0).round().max(1.0) as u32;
        while s > 1 && (W * s > surface.0 * 9 / 10 || H * s > surface.1 * 9 / 10) {
            s -= 1;
        }
        self.scale = s;
    }

    pub fn size(&self) -> (u32, u32) {
        (W * self.scale, H * self.scale)
    }

    /// The button under a point of the image, in its own pixels.
    pub fn hit(&self, x: f64, y: f64) -> Option<Button> {
        let (x, y) = (x / self.scale as f64, y / self.scale as f64);
        buttons()
            .into_iter()
            .find(|&(_, _, (bx, by, bw, bh))| {
                x >= bx as f64 && y >= by as f64 && x < (bx + bw) as f64 && y < (by + bh) as f64
            })
            .map(|(b, _, _)| b)
    }

    /// The image, BGRA, `size()` pixels.
    pub fn render(&self) -> Vec<u8> {
        let mut c = Canvas { px: vec![0; (W * H) as usize] };
        c.fill((0, 0, W, H), BORDER);
        c.fill((1, 1, W - 2, H - 2), PANEL);
        for (i, line) in LINES.iter().enumerate() {
            c.text(PAD, PAD + i as u32 * CH, line);
        }
        for (b, label, r) in buttons() {
            c.fill(r, EDGE);
            let face = if self.hover == Some(b) { HOVER } else { FACE };
            c.fill((r.0 + 1, r.1 + 1, r.2 - 2, r.3 - 2), face);
            c.text(r.0 + CW, r.1 + (r.3 - CH) / 2, label);
        }
        let (w, h, s) = (W * self.scale, H * self.scale, self.scale);
        let mut out = Vec::with_capacity((w * h * 4) as usize);
        for y in 0..h {
            for x in 0..w {
                out.extend_from_slice(&c.px[((y / s) * W + x / s) as usize].to_le_bytes());
            }
        }
        out
    }
}

struct Canvas {
    px: Vec<u32>,
}

impl Canvas {
    fn fill(&mut self, (x, y, w, h): (u32, u32, u32, u32), colour: u32) {
        for yy in y..y + h {
            let row = (yy * W) as usize;
            self.px[row + x as usize..row + (x + w) as usize].fill(colour);
        }
    }

    fn text(&mut self, x: u32, y: u32, s: &str) {
        for (i, ch) in s.bytes().enumerate() {
            let gx = x + i as u32 * CW;
            for r in 0..CH {
                let bits = FONT[ch as usize * CH as usize + r as usize];
                for col in 0..CW {
                    if bits & (0x80 >> col) != 0 {
                        self.px[((y + r) * W + gx + col) as usize] = TEXT;
                    }
                }
            }
        }
    }
}
