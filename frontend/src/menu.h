#pragma once
// Telas do frontend desenhadas com GDI em 480×320 (o dobro do GBA) e ampliadas pixel a pixel
// como a imagem do jogo, com a paleta da interface do próprio jogo (caixa de diálogo pêssego,
// bordas marrons, título amarelo-claro):
//
//   SettingsMenu — configurações; no jogo (que fica congelado) ou a partir do launcher.
//   Launcher     — tela de abertura do AvatarRecomp.exe: jogar, configurações, escolher a ROM.

#ifdef _WIN32

#include <cstdint>
#include <string>
#include <vector>

#include "config.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace fe {

enum class Nav { Up, Down, Left, Right, Confirm, Back };

// Superfície de desenho 480×320 ARGB com um DC do GDI.
class Canvas {
public:
    static constexpr int kW = 480, kH = 320;
    Canvas();
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    const BITMAPINFO& bitmap_info() const { return bmi_; }

protected:
    HDC dc_ = nullptr;
    uint32_t* bits_ = nullptr;
    HFONT font_title_ = nullptr, font_item_ = nullptr, font_small_ = nullptr;

private:
    HBITMAP bmp_ = nullptr, old_bmp_ = nullptr;
    BITMAPINFO bmi_{};
};

class SettingsMenu : public Canvas {
public:
    enum class Action { None, Resume, SaveState, LoadState, ToggleFullscreen, ApplyScale, ApplyVolume, Quit };

    SettingsMenu(Config& cfg, int& slot);

    bool is_open() const { return open_; }
    // in_game: com continuar, save states e sair; fora do jogo (launcher), só as opções.
    void show(bool in_game);
    void hide() { open_ = false; capture_ = Capture::None; }
    void set_quit_label(const wchar_t* label) { quit_label_ = label; }

    void nav(Nav n);
    // Remapeamento: enquanto espera uma tecla/botão, o próximo pressionado vai para cá.
    bool capturing_key() const { return capture_ == Capture::Key; }
    bool capturing_pad() const { return capture_ == Capture::Pad; }
    void captured_key(int vk);
    void captured_pad(uint32_t mask);
    void cancel_capture();

    Action take_action();
    void set_status(const std::string& s);
    void set_fullscreen_flag(bool on) { fullscreen_ = on; }
    void set_slot_exists(bool exists) { slot_exists_ = exists; }
    // Taxa do monitor e se o V-Sync conseguiu travar nela (para mostrar na página Video).
    void set_display(double hz, bool vsync_locked) { display_hz_ = hz, vsync_locked_ = vsync_locked; }
    bool config_dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }

    // Desenha sobre o quadro congelado do jogo (240×160) ou sobre uma imagem 480×320.
    const uint32_t* render(const uint32_t* frozen);
    const uint32_t* render_over(const uint32_t* image);

private:
    enum class Page { Main, Video, Keyboard, Pad };
    enum class Capture { None, Key, Pad };

    struct Row {
        std::wstring label;
        std::wstring value;
        bool adjustable = false; // mostra ◄ ►
    };

    std::vector<int> main_ids() const; // linhas visíveis da página principal
    std::vector<Row> rows() const;
    int row_count() const;
    void activate(int row);
    void adjust(int row, int dir);
    const uint32_t* draw_panel();

    Config& cfg_;
    int& slot_;
    bool open_ = false, in_game_ = true;
    const wchar_t* quit_label_ = L"Quit game";
    Page page_ = Page::Main;
    int sel_ = 0;
    int sel_main_ = 0; // seleção a restaurar ao voltar de uma subpágina
    Capture capture_ = Capture::None;
    int capture_button_ = 0;
    Action action_ = Action::None;
    bool fullscreen_ = false, slot_exists_ = false, dirty_ = false;
    double display_hz_ = 0;
    bool vsync_locked_ = true;
    std::wstring status_;
    DWORD status_until_ = 0;
};

class Launcher : public Canvas {
public:
    enum class Action { None, Play, Settings, SelectRom, Quit };

    Launcher();

    void nav(Nav n);
    Action take_action();
    // ROM atual: nome do arquivo (vazio = nenhuma) e se é a ROM esperada.
    void set_rom(const std::wstring& name, bool ok);
    void set_status(const std::string& s);

    const uint32_t* render();
    const uint32_t* background() const { return bg_.data(); } // arte da capa, 480×320

private:
    std::vector<uint32_t> bg_;
    int sel_ = 0;
    Action action_ = Action::None;
    std::wstring rom_name_, status_;
    bool rom_ok_ = false;
    DWORD status_until_ = 0;
};

} // namespace fe

#endif
