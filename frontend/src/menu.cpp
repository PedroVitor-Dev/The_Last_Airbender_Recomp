#include "menu.h"

#ifdef _WIN32

#include <algorithm>
#include <string>

#include <wincodec.h>

#ifndef TLA_VERSION
#define TLA_VERSION "dev"
#endif

namespace fe {

namespace {

// Paleta tirada da interface do jogo (caixa de diálogo, tela de nome e título).
constexpr COLORREF kPeach = RGB(0xFF, 0xC6, 0x9C);      // fundo da caixa de diálogo
constexpr COLORREF kPeachDark = RGB(0xE7, 0xA5, 0x73);  // sombra da caixa / seleção
constexpr COLORREF kBorderMid = RGB(0xC6, 0x8C, 0x73);  // borda clara
constexpr COLORREF kBorderDark = RGB(0x6B, 0x39, 0x31); // borda escura
constexpr COLORREF kBrown = RGB(0x5A, 0x29, 0x18);      // marrom da tela de nome
constexpr COLORREF kBrownMid = RGB(0x9C, 0x6B, 0x31);   // valores
constexpr COLORREF kInk = RGB(0x2A, 0x14, 0x08);        // texto
constexpr COLORREF kFaded = RGB(0xB0, 0x88, 0x6C);      // item indisponível
constexpr COLORREF kCream = RGB(0xFF, 0xF7, 0xBD);      // texto do título ("NEW GAME")
constexpr COLORREF kRed = RGB(0xBD, 0x21, 0x00);        // detalhe vermelho da borda
constexpr COLORREF kSky = RGB(0x8C, 0xB5, 0xFF);        // contorno azul do título
constexpr COLORREF kWarn = RGB(0xFF, 0x9C, 0x7B);

constexpr int kW = Canvas::kW, kH = Canvas::kH;
constexpr int kPanelL = 40, kPanelT = 12, kPanelR = 440, kPanelB = 308;
constexpr int kRowsTop = 58, kFootTop = kPanelB - 28;

// Ordem dos botões nas páginas de remapeamento.
constexpr int kButtonOrder[BTN_COUNT] = {BTN_A, BTN_B, BTN_L, BTN_R, BTN_START, BTN_SELECT, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT};
const wchar_t* kButtonLabel[BTN_COUNT] = {L"A Button", L"B Button", L"Select", L"Start", L"Right", L"Left",
                                           L"Up", L"Down", L"R Button", L"L Button"};

enum MainRow {
    M_RESUME, M_SAVE, M_LOAD, M_VIDEO, M_VOLUME, M_ANALOG, M_LAUNCHER, M_KEYBOARD, M_PAD, M_DEFAULTS, M_QUIT,
    M_COUNT
};
enum VideoRow { V_FULLSCREEN, V_SCALE, V_FPS, V_VSYNC, V_BACK, V_COUNT };

std::wstring widen(const char* s) {
    std::wstring w;
    for (; *s; ++s) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*s)));
    return w;
}

std::wstring from_utf8(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
    RECT rc{l, t, r, b};
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

void text(HDC dc, HFONT f, COLORREF c, int x, int y, const std::wstring& s, UINT align = TA_LEFT) {
    SelectObject(dc, f);
    SetTextAlign(dc, align | TA_TOP);
    SetTextColor(dc, c);
    TextOutW(dc, x, y, s.c_str(), static_cast<int>(s.size()));
}

HFONT make_font(int height, int weight, const wchar_t* face) {
    // Sem suavização: ampliado pixel a pixel, fica com o mesmo aspecto da imagem do jogo.
    return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH, face);
}

// Moldura no estilo da caixa de diálogo do jogo: preto, marrom escuro, marrom claro, pêssego.
void dialog_box(HDC dc, int l, int t, int r, int b) {
    fill(dc, l, t, r, b, RGB(0, 0, 0));
    fill(dc, l + 2, t + 2, r - 2, b - 2, kBorderDark);
    fill(dc, l + 5, t + 5, r - 5, b - 5, kBorderMid);
    fill(dc, l + 7, t + 7, r - 7, b - 7, kPeach);
}

// Escurece uma faixa de pixels (para barras sobre a arte do launcher).
void shade(uint32_t* bits, int t, int b, int keep_of_10) {
    for (int y = t; y < b; ++y)
        for (int x = 0; x < kW; ++x) {
            const uint32_t p = bits[y * kW + x];
            const uint32_t r = ((p >> 16) & 0xFF) * keep_of_10 / 10, g = ((p >> 8) & 0xFF) * keep_of_10 / 10,
                           bl = (p & 0xFF) * keep_of_10 / 10;
            bits[y * kW + x] = r << 16 | g << 8 | bl;
        }
}

// Arte de fundo do launcher: JPEG 480×320 embutido no executável (recurso 2), decodificado com
// o WIC do Windows. Sem o recurso, um degradê no tom do céu da capa.
std::vector<uint32_t> load_background() {
    std::vector<uint32_t> px(static_cast<size_t>(kW) * kH);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            px[static_cast<size_t>(y) * kW + x] = (0x60 + y / 6) << 16 | (0x20 + y / 10) << 8 | 0x18;

    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(2), MAKEINTRESOURCEW(10)); // RT_RCDATA
    if (!res) return px;
    const void* data = LockResource(LoadResource(mod, res));
    const DWORD size = SizeofResource(mod, res);
    if (!data || !size) return px;

    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT w = 0, h = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)), size)) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w == kW && h == kH &&
        SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
        conv->CopyPixels(nullptr, kW * 4, static_cast<UINT>(px.size() * 4), reinterpret_cast<BYTE*>(px.data()));
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    for (auto& p : px) p &= 0xFFFFFF;
    return px;
}

} // namespace

// ───────────────────────────────────────────────────────────────── Canvas ──

Canvas::Canvas() {
    bmi_.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi_.bmiHeader.biWidth = kW;
    bmi_.bmiHeader.biHeight = -kH; // de cima para baixo
    bmi_.bmiHeader.biPlanes = 1;
    bmi_.bmiHeader.biBitCount = 32;
    bmi_.bmiHeader.biCompression = BI_RGB;
    dc_ = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    bmp_ = CreateDIBSection(dc_, &bmi_, DIB_RGB_COLORS, &bits, nullptr, 0);
    bits_ = static_cast<uint32_t*>(bits);
    old_bmp_ = static_cast<HBITMAP>(SelectObject(dc_, bmp_));
    SetBkMode(dc_, TRANSPARENT);
    font_title_ = make_font(22, FW_BOLD, L"Trebuchet MS");
    font_item_ = make_font(14, FW_BOLD, L"Tahoma");
    font_small_ = make_font(11, FW_NORMAL, L"Tahoma");
}

Canvas::~Canvas() {
    SelectObject(dc_, old_bmp_);
    DeleteObject(bmp_);
    DeleteObject(font_title_);
    DeleteObject(font_item_);
    DeleteObject(font_small_);
    DeleteDC(dc_);
}

// ─────────────────────────────────────────────────────────── SettingsMenu ──

SettingsMenu::SettingsMenu(Config& cfg, int& slot) : cfg_(cfg), slot_(slot) {}

void SettingsMenu::show(bool in_game) {
    open_ = true;
    in_game_ = in_game;
    page_ = Page::Main;
    sel_ = 0;
    capture_ = Capture::None;
    action_ = Action::None;
}

void SettingsMenu::set_status(const std::string& s) {
    status_ = from_utf8(s);
    status_until_ = GetTickCount() + 2500;
}

SettingsMenu::Action SettingsMenu::take_action() {
    const Action a = action_;
    action_ = Action::None;
    return a;
}

std::vector<int> SettingsMenu::main_ids() const {
    std::vector<int> ids;
    for (int i = 0; i < M_COUNT; ++i)
        if (in_game_ || (i != M_RESUME && i != M_SAVE && i != M_LOAD)) ids.push_back(i);
    return ids;
}

int SettingsMenu::row_count() const {
    if (page_ == Page::Main) return static_cast<int>(main_ids().size());
    return page_ == Page::Video ? V_COUNT : BTN_COUNT + 1;
}

std::vector<SettingsMenu::Row> SettingsMenu::rows() const {
    std::vector<Row> r;
    if (page_ == Page::Main) {
        const std::wstring slot = L"Slot " + std::to_wstring(slot_);
        for (int id : main_ids()) {
            switch (id) {
            case M_RESUME: r.push_back({L"Resume", L"", false}); break;
            case M_SAVE: r.push_back({L"Save state", slot, true}); break;
            case M_LOAD: r.push_back({L"Load state", slot + (slot_exists_ ? L"" : L" (empty)"), true}); break;
            case M_VIDEO: r.push_back({L"Video", L"›", false}); break;
            case M_VOLUME:
                r.push_back({L"Volume", cfg_.volume == 0 ? L"Muted" : std::to_wstring(cfg_.volume) + L"%", true});
                break;
            case M_ANALOG: r.push_back({L"Analog stick", cfg_.analog ? L"On" : L"Off", true}); break;
            case M_LAUNCHER: r.push_back({L"Launcher at startup", cfg_.launcher ? L"On" : L"Off", true}); break;
            case M_KEYBOARD: r.push_back({L"Keyboard", L"›", false}); break;
            case M_PAD: r.push_back({L"Gamepad", L"›", false}); break;
            case M_DEFAULTS: r.push_back({L"Restore defaults", L"", false}); break;
            case M_QUIT: r.push_back({in_game_ ? quit_label_ : L"Back", L"", false}); break;
            default: break;
            }
        }
        return r;
    }
    if (page_ == Page::Video) {
        r.push_back({L"Fullscreen", fullscreen_ ? L"On" : L"Off", true});
        r.push_back({L"Window size", std::to_wstring(cfg_.scale) + L"×", true});
        r.push_back({L"Frame rate", cfg_.fps60 ? L"60 fps" : L"59.73 fps (original)", true});
        std::wstring vs = cfg_.vsync ? L"On" : L"Off";
        if (cfg_.vsync && !vsync_locked_ && display_hz_ > 0)
            vs = L"On (n/a at " + std::to_wstring(static_cast<int>(display_hz_ + 0.5)) + L" Hz)";
        r.push_back({L"V-Sync", vs, true});
        r.push_back({L"Back", L"", false});
        return r;
    }
    const bool kb = page_ == Page::Keyboard;
    for (int i = 0; i < BTN_COUNT; ++i) {
        const int b = kButtonOrder[i];
        std::wstring v;
        if ((capture_ != Capture::None) && capture_button_ == b) {
            v = kb ? L"Press a key…" : L"Press a button…";
        } else if (kb) {
            for (int k : cfg_.keyboard[b]) v += (v.empty() ? L"" : L", ") + widen(key_name(k));
        } else {
            for (uint32_t m : cfg_.pad[b]) v += (v.empty() ? L"" : L", ") + widen(pad_name(m));
        }
        r.push_back({kButtonLabel[b], v.empty() ? L"—" : v, false});
    }
    r.push_back({L"Back", L"", false});
    return r;
}

void SettingsMenu::adjust(int row, int dir) {
    if (page_ == Page::Video) {
        switch (row) {
        case V_FULLSCREEN: action_ = Action::ToggleFullscreen; break;
        case V_SCALE: {
            const int s = std::clamp(cfg_.scale + dir, 1, 8);
            if (s != cfg_.scale) cfg_.scale = s, dirty_ = true, action_ = Action::ApplyScale;
            break;
        }
        case V_FPS: cfg_.fps60 = !cfg_.fps60, dirty_ = true; break;
        case V_VSYNC: cfg_.vsync = !cfg_.vsync, dirty_ = true; break;
        default: break;
        }
        return;
    }
    if (page_ != Page::Main) return;
    switch (main_ids()[static_cast<size_t>(row)]) {
    case M_SAVE:
    case M_LOAD: slot_ = ((slot_ + dir) % 10 + 10) % 10; break;
    case M_VOLUME: {
        const int v = std::clamp(cfg_.volume + dir * 10, 0, 100);
        if (v != cfg_.volume) cfg_.volume = v, dirty_ = true, action_ = Action::ApplyVolume;
        break;
    }
    case M_ANALOG: cfg_.analog = !cfg_.analog, dirty_ = true; break;
    case M_LAUNCHER: cfg_.launcher = !cfg_.launcher, dirty_ = true; break;
    default: break;
    }
}

void SettingsMenu::activate(int row) {
    if (page_ == Page::Main) {
        switch (main_ids()[static_cast<size_t>(row)]) {
        case M_RESUME: action_ = Action::Resume; break;
        case M_SAVE: action_ = Action::SaveState; break;
        case M_LOAD: action_ = Action::LoadState; break;
        case M_VIDEO: sel_main_ = row, page_ = Page::Video, sel_ = 0; break;
        case M_VOLUME: case M_ANALOG: case M_LAUNCHER: adjust(row, +1); break;
        case M_KEYBOARD: sel_main_ = row, page_ = Page::Keyboard, sel_ = 0; break;
        case M_PAD: sel_main_ = row, page_ = Page::Pad, sel_ = 0; break;
        case M_DEFAULTS: {
            Config d = default_config();
            d.fullscreen = cfg_.fullscreen;
            d.rom = cfg_.rom;
            cfg_ = d;
            dirty_ = true;
            action_ = Action::ApplyScale;
            set_status("Defaults restored");
            break;
        }
        case M_QUIT: action_ = in_game_ ? Action::Quit : Action::Resume; break;
        default: break;
        }
        return;
    }
    if (page_ == Page::Video) {
        if (row == V_BACK) page_ = Page::Main, sel_ = sel_main_;
        else adjust(row, +1);
        return;
    }
    if (row == BTN_COUNT) { // Back
        page_ = Page::Main;
        sel_ = sel_main_;
        return;
    }
    capture_button_ = kButtonOrder[row];
    capture_ = page_ == Page::Keyboard ? Capture::Key : Capture::Pad;
}

void SettingsMenu::nav(Nav n) {
    if (capture_ != Capture::None) return;
    const int count = row_count();
    switch (n) {
    case Nav::Up: sel_ = (sel_ + count - 1) % count; break;
    case Nav::Down: sel_ = (sel_ + 1) % count; break;
    case Nav::Left: adjust(sel_, -1); break;
    case Nav::Right: adjust(sel_, +1); break;
    case Nav::Confirm: activate(sel_); break;
    case Nav::Back:
        if (page_ != Page::Main) page_ = Page::Main, sel_ = sel_main_;
        else action_ = Action::Resume;
        break;
    }
}

void SettingsMenu::cancel_capture() { capture_ = Capture::None; }

void SettingsMenu::captured_key(int vk) {
    if (capture_ != Capture::Key) return;
    capture_ = Capture::None;
    if (vk == VK_ESCAPE) return; // cancelar
    if (vk == cfg_.key_menu) {
        set_status(std::string("That key opens the menu (") + key_name(vk) + ")");
        return;
    }
    for (auto& v : cfg_.keyboard) v.erase(std::remove(v.begin(), v.end(), vk), v.end()); // tira de outro botão
    cfg_.keyboard[capture_button_] = {vk};
    dirty_ = true;
    set_status(std::string(button_name(capture_button_)) + " = " + key_name(vk));
}

void SettingsMenu::captured_pad(uint32_t mask) {
    if (capture_ != Capture::Pad) return;
    capture_ = Capture::None;
    if (mask == PAD_GUIDE) return; // cancelar
    for (auto& v : cfg_.pad) v.erase(std::remove(v.begin(), v.end(), mask), v.end());
    cfg_.pad[capture_button_] = {mask};
    dirty_ = true;
    set_status(std::string(button_name(capture_button_)) + " = " + pad_name(mask));
}

const uint32_t* SettingsMenu::render(const uint32_t* frozen) {
    // Fundo: o quadro congelado, ampliado 2× e escurecido.
    for (int y = 0; y < kH; ++y) {
        const uint32_t* src = frozen + (y / 2) * 240;
        uint32_t* dst = bits_ + y * kW;
        for (int x = 0; x < kW; ++x) {
            const uint32_t p = src[x / 2];
            const uint32_t r = ((p >> 16) & 0xFF) * 3 / 10, g = ((p >> 8) & 0xFF) * 3 / 10, b = (p & 0xFF) * 3 / 10;
            dst[x] = (r + 18) << 16 | (g + 8) << 8 | b;
        }
    }
    return draw_panel();
}

const uint32_t* SettingsMenu::render_over(const uint32_t* image) {
    std::copy(image, image + kW * kH, bits_);
    shade(bits_, 0, kH, 4);
    return draw_panel();
}

const uint32_t* SettingsMenu::draw_panel() {
    GdiFlush();
    dialog_box(dc_, kPanelL, kPanelT, kPanelR, kPanelB);
    // Faixa do título com o detalhe vermelho embaixo.
    fill(dc_, kPanelL + 7, kPanelT + 7, kPanelR - 7, kPanelT + 37, kBrown);
    fill(dc_, kPanelL + 7, kPanelT + 37, kPanelR - 7, kPanelT + 39, kRed);
    const std::wstring title = page_ == Page::Main       ? L"SETTINGS"
                               : page_ == Page::Video    ? L"VIDEO"
                               : page_ == Page::Keyboard ? L"KEYBOARD"
                                                         : L"GAMEPAD";
    text(dc_, font_title_, RGB(0, 0, 0), kW / 2 + 2, kPanelT + 12, title, TA_CENTER);
    text(dc_, font_title_, kCream, kW / 2, kPanelT + 10, title, TA_CENTER);

    const auto rs = rows();
    const int row_h = std::min(20, (kFootTop - kRowsTop) / static_cast<int>(rs.size()));
    for (size_t i = 0; i < rs.size(); ++i) {
        const int y = kRowsTop + static_cast<int>(i) * row_h;
        const bool selected = static_cast<int>(i) == sel_;
        if (selected) {
            fill(dc_, kPanelL + 12, y - 1, kPanelR - 12, y + row_h - 2, kBorderMid);
            fill(dc_, kPanelL + 13, y, kPanelR - 13, y + row_h - 3, kPeachDark);
            text(dc_, font_item_, kRed, kPanelL + 17, y, L"►");
        }
        text(dc_, font_item_, kInk, kPanelL + 33, y, rs[i].label);
        std::wstring v = rs[i].value;
        if (selected && rs[i].adjustable) v = L"◄  " + v + L"  ►";
        text(dc_, font_item_, selected ? kBrown : kBrownMid, kPanelR - 20, y, v, TA_RIGHT);
    }

    // Rodapé: mensagem de status ou dicas.
    std::wstring foot;
    if (status_until_ && GetTickCount() < status_until_) foot = status_;
    else if (capture_ == Capture::Key) foot = L"Press the new key   ·   Esc cancels";
    else if (capture_ == Capture::Pad) foot = L"Press the new button   ·   Xbox/home button or Esc cancels";
    else foot = L"↑↓ select   ·   ←→ change   ·   Enter/A confirm   ·   Esc/B back";
    fill(dc_, kPanelL + 7, kFootTop, kPanelR - 7, kFootTop + 1, kBorderMid);
    text(dc_, font_small_, kBorderDark, kW / 2, kPanelB - 22, foot, TA_CENTER);

    GdiFlush();
    return bits_;
}

// ─────────────────────────────────────────────────────────────── Launcher ──

namespace {
enum LauncherRow { L_PLAY, L_SETTINGS, L_ROM, L_QUIT, L_COUNT };
const wchar_t* kLauncherLabel[L_COUNT] = {L"Play", L"Settings", L"Select ROM…", L"Quit"};
constexpr int kLPanelL = 14, kLPanelT = 196, kLPanelR = 190, kLPanelB = 292, kLRowH = 20;
constexpr int kBarT = 298;
} // namespace

Launcher::Launcher() : bg_(load_background()) {}

void Launcher::set_rom(const std::wstring& name, bool ok) {
    rom_name_ = name;
    rom_ok_ = ok;
    sel_ = ok ? L_PLAY : L_ROM;
}

void Launcher::set_status(const std::string& s) {
    status_ = from_utf8(s);
    status_until_ = GetTickCount() + 3500;
}

Launcher::Action Launcher::take_action() {
    const Action a = action_;
    action_ = Action::None;
    return a;
}

void Launcher::nav(Nav n) {
    switch (n) {
    case Nav::Up: sel_ = (sel_ + L_COUNT - 1) % L_COUNT; break;
    case Nav::Down: sel_ = (sel_ + 1) % L_COUNT; break;
    case Nav::Confirm:
        switch (sel_) {
        case L_PLAY:
            if (rom_ok_) action_ = Action::Play;
            else set_status("Select your ROM first");
            break;
        case L_SETTINGS: action_ = Action::Settings; break;
        case L_ROM: action_ = Action::SelectRom; break;
        case L_QUIT: action_ = Action::Quit; break;
        default: break;
        }
        break;
    case Nav::Back: sel_ = L_QUIT; break;
    default: break;
    }
}

const uint32_t* Launcher::render() {
    std::copy(bg_.begin(), bg_.end(), bits_);
    shade(bits_, kBarT, kH, 3); // barra de status embaixo
    GdiFlush();

    dialog_box(dc_, kLPanelL, kLPanelT, kLPanelR, kLPanelB);
    for (int i = 0; i < L_COUNT; ++i) {
        const int y = kLPanelT + 9 + i * kLRowH;
        const bool selected = i == sel_;
        const bool disabled = i == L_PLAY && !rom_ok_;
        if (selected) {
            fill(dc_, kLPanelL + 10, y - 1, kLPanelR - 10, y + kLRowH - 2, kBorderMid);
            fill(dc_, kLPanelL + 11, y, kLPanelR - 11, y + kLRowH - 3, kPeachDark);
            text(dc_, font_item_, kRed, kLPanelL + 15, y + 1, L"►");
        }
        text(dc_, font_item_, disabled ? kFaded : kInk, kLPanelL + 31, y + 1, kLauncherLabel[i]);
    }

    // Barra de status: ROM atual (ou mensagem) à esquerda, versão à direita.
    std::wstring left;
    COLORREF color = kCream;
    if (status_until_ && GetTickCount() < status_until_) {
        left = status_;
        color = kPeach;
    } else if (rom_name_.empty()) {
        left = L"No ROM selected — choose \"Select ROM…\"";
        color = kWarn;
    } else if (!rom_ok_) {
        left = L"✗ " + rom_name_ + L" is not the expected ROM (USA, BQZE)";
        color = kWarn;
    } else {
        left = L"✓ " + rom_name_;
    }
    text(dc_, font_small_, color, 10, kBarT + 5, left);
    text(dc_, font_small_, kSky, kW - 10, kBarT + 5, L"v" TLA_VERSION, TA_RIGHT);

    GdiFlush();
    return bits_;
}

} // namespace fe

#endif
