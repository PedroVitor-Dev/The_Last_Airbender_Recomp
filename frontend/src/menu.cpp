#include "menu.h"

#ifdef _WIN32

#include <algorithm>
#include <string>

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
constexpr COLORREF kCream = RGB(0xFF, 0xF7, 0xBD);      // texto do título ("NEW GAME")
constexpr COLORREF kRed = RGB(0xBD, 0x21, 0x00);        // detalhe vermelho da borda

constexpr int kPanelL = 40, kPanelT = 12, kPanelR = 440, kPanelB = 308;
constexpr int kRowsTop = 58, kRowH = 20;

// Ordem dos botões nas páginas de remapeamento.
constexpr int kButtonOrder[BTN_COUNT] = {BTN_A, BTN_B, BTN_L, BTN_R, BTN_START, BTN_SELECT, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT};
const wchar_t* kButtonLabel[BTN_COUNT] = {L"Botão A", L"Botão B", L"Select", L"Start", L"Direita", L"Esquerda",
                                           L"Cima", L"Baixo", L"Botão R", L"Botão L"};

enum MainRow { M_RESUME, M_SAVE, M_LOAD, M_FULLSCREEN, M_SCALE, M_VOLUME, M_ANALOG, M_KEYBOARD, M_PAD, M_DEFAULTS, M_QUIT, M_COUNT };

std::wstring widen(const char* s) {
    std::wstring w;
    for (; *s; ++s) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*s)));
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

} // namespace

SettingsMenu::SettingsMenu(Config& cfg, int& slot) : cfg_(cfg), slot_(slot) {
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

SettingsMenu::~SettingsMenu() {
    SelectObject(dc_, old_bmp_);
    DeleteObject(bmp_);
    DeleteObject(font_title_);
    DeleteObject(font_item_);
    DeleteObject(font_small_);
    DeleteDC(dc_);
}

void SettingsMenu::show() {
    open_ = true;
    page_ = Page::Main;
    sel_ = 0;
    capture_ = Capture::None;
    action_ = Action::None;
}

void SettingsMenu::set_status(const std::string& s) {
    status_.clear();
    // UTF-8 → UTF-16 (as mensagens têm acentos)
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    status_.resize(static_cast<size_t>(n));
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), status_.data(), n);
    status_until_ = GetTickCount() + 2500;
}

SettingsMenu::Action SettingsMenu::take_action() {
    const Action a = action_;
    action_ = Action::None;
    return a;
}

int SettingsMenu::row_count() const { return page_ == Page::Main ? M_COUNT : BTN_COUNT + 1; }

std::vector<SettingsMenu::Row> SettingsMenu::rows() const {
    std::vector<Row> r;
    if (page_ == Page::Main) {
        const std::wstring slot = L"Slot " + std::to_wstring(slot_);
        r.push_back({L"Continuar", L"", false});
        r.push_back({L"Salvar estado", slot, true});
        r.push_back({L"Carregar estado", slot + (slot_exists_ ? L"" : L" (vazio)"), true});
        r.push_back({L"Tela cheia", fullscreen_ ? L"Sim" : L"Não", true});
        r.push_back({L"Tamanho da janela", std::to_wstring(cfg_.scale) + L"×", true});
        r.push_back({L"Volume", cfg_.volume == 0 ? L"Mudo" : std::to_wstring(cfg_.volume) + L"%", true});
        r.push_back({L"Analógico do controle", cfg_.analog ? L"Sim" : L"Não", true});
        r.push_back({L"Teclado", L"›", false});
        r.push_back({L"Controle", L"›", false});
        r.push_back({L"Restaurar padrões", L"", false});
        r.push_back({L"Sair do jogo", L"", false});
        return r;
    }
    const bool kb = page_ == Page::Keyboard;
    for (int i = 0; i < BTN_COUNT; ++i) {
        const int b = kButtonOrder[i];
        std::wstring v;
        if ((capture_ != Capture::None) && capture_button_ == b) {
            v = kb ? L"Pressione uma tecla…" : L"Pressione um botão…";
        } else if (kb) {
            for (int k : cfg_.keyboard[b]) v += (v.empty() ? L"" : L", ") + widen(key_name(k));
        } else {
            for (uint32_t m : cfg_.pad[b]) v += (v.empty() ? L"" : L", ") + widen(pad_name(m));
        }
        r.push_back({kButtonLabel[b], v.empty() ? L"—" : v, false});
    }
    r.push_back({L"Voltar", L"", false});
    return r;
}

void SettingsMenu::adjust(int row, int dir) {
    if (page_ != Page::Main) return;
    switch (row) {
    case M_SAVE:
    case M_LOAD: slot_ = ((slot_ + dir) % 10 + 10) % 10; break;
    case M_FULLSCREEN: action_ = Action::ToggleFullscreen; break;
    case M_SCALE: {
        const int s = std::clamp(cfg_.scale + dir, 1, 8);
        if (s != cfg_.scale) cfg_.scale = s, dirty_ = true, action_ = Action::ApplyScale;
        break;
    }
    case M_VOLUME: {
        const int v = std::clamp(cfg_.volume + dir * 10, 0, 100);
        if (v != cfg_.volume) cfg_.volume = v, dirty_ = true, action_ = Action::ApplyVolume;
        break;
    }
    case M_ANALOG: cfg_.analog = !cfg_.analog, dirty_ = true; break;
    default: break;
    }
}

void SettingsMenu::activate(int row) {
    if (page_ == Page::Main) {
        switch (row) {
        case M_RESUME: action_ = Action::Resume; break;
        case M_SAVE: action_ = Action::SaveState; break;
        case M_LOAD: action_ = Action::LoadState; break;
        case M_FULLSCREEN: action_ = Action::ToggleFullscreen; break;
        case M_SCALE: case M_VOLUME: case M_ANALOG: adjust(row, +1); break;
        case M_KEYBOARD: sel_main_ = row, page_ = Page::Keyboard, sel_ = 0; break;
        case M_PAD: sel_main_ = row, page_ = Page::Pad, sel_ = 0; break;
        case M_DEFAULTS: {
            const bool fs = cfg_.fullscreen;
            cfg_ = default_config();
            cfg_.fullscreen = fs;
            dirty_ = true;
            action_ = Action::ApplyScale;
            set_status("Padrões restaurados");
            break;
        }
        case M_QUIT: action_ = Action::Quit; break;
        default: break;
        }
        return;
    }
    if (row == BTN_COUNT) { // Voltar
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
        set_status(std::string("Essa tecla abre o menu (") + key_name(vk) + ")");
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
    GdiFlush();

    // Moldura no estilo da caixa de diálogo do jogo: preto, marrom escuro, marrom claro, pêssego.
    fill(dc_, kPanelL, kPanelT, kPanelR, kPanelB, RGB(0, 0, 0));
    fill(dc_, kPanelL + 2, kPanelT + 2, kPanelR - 2, kPanelB - 2, kBorderDark);
    fill(dc_, kPanelL + 5, kPanelT + 5, kPanelR - 5, kPanelB - 5, kBorderMid);
    fill(dc_, kPanelL + 7, kPanelT + 7, kPanelR - 7, kPanelB - 7, kPeach);
    // Faixa do título com o detalhe vermelho embaixo.
    fill(dc_, kPanelL + 7, kPanelT + 7, kPanelR - 7, kPanelT + 37, kBrown);
    fill(dc_, kPanelL + 7, kPanelT + 37, kPanelR - 7, kPanelT + 39, kRed);
    const std::wstring title = page_ == Page::Main ? L"CONFIGURAÇÕES" : page_ == Page::Keyboard ? L"TECLADO" : L"CONTROLE";
    text(dc_, font_title_, RGB(0, 0, 0), kW / 2 + 2, kPanelT + 12, title, TA_CENTER);
    text(dc_, font_title_, kCream, kW / 2, kPanelT + 10, title, TA_CENTER);

    const auto rs = rows();
    for (size_t i = 0; i < rs.size(); ++i) {
        const int y = kRowsTop + static_cast<int>(i) * kRowH;
        const bool selected = static_cast<int>(i) == sel_;
        if (selected) {
            fill(dc_, kPanelL + 12, y - 1, kPanelR - 12, y + kRowH - 2, kBorderMid);
            fill(dc_, kPanelL + 13, y, kPanelR - 13, y + kRowH - 3, kPeachDark);
            text(dc_, font_item_, kRed, kPanelL + 17, y + 1, L"►");
        }
        text(dc_, font_item_, kInk, kPanelL + 33, y + 1, rs[i].label);
        std::wstring v = rs[i].value;
        if (selected && rs[i].adjustable) v = L"◄  " + v + L"  ►";
        text(dc_, font_item_, selected ? kBrown : kBrownMid, kPanelR - 20, y + 1, v, TA_RIGHT);
    }

    // Rodapé: mensagem de status ou dicas.
    std::wstring foot;
    if (status_until_ && GetTickCount() < status_until_) foot = status_;
    else if (capture_ == Capture::Key) foot = L"Pressione a nova tecla   ·   Esc cancela";
    else if (capture_ == Capture::Pad) foot = L"Pressione o novo botão   ·   botão Xbox/home ou Esc cancela";
    else foot = L"↑↓ escolher   ·   ←→ alterar   ·   Enter/A confirmar   ·   Esc/B voltar";
    fill(dc_, kPanelL + 7, kPanelB - 28, kPanelR - 7, kPanelB - 27, kBorderMid);
    text(dc_, font_small_, kBorderDark, kW / 2, kPanelB - 22, foot, TA_CENTER);

    GdiFlush();
    return bits_;
}

} // namespace fe

#endif
