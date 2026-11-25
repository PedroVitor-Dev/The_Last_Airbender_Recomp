#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#endif

namespace fe {

namespace {

struct Named {
    const char* name;
    uint32_t code;
};

#ifdef _WIN32
const Named kKeys[] = {
    {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"ENTER", VK_RETURN},
    {"BACKSPACE", VK_BACK}, {"SPACE", VK_SPACE}, {"TAB", VK_TAB}, {"ESC", VK_ESCAPE}, {"SHIFT", VK_SHIFT},
    {"CTRL", VK_CONTROL}, {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE}, {"HOME", VK_HOME}, {"END", VK_END},
    {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT},
};
#endif
const Named kPad[] = {
    {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000}, {"LB", 0x0100}, {"RB", 0x0200},
    {"LT", PAD_LT}, {"RT", PAD_RT}, {"BACK", 0x0020}, {"START", 0x0010}, {"LS", 0x0040}, {"RS", 0x0080},
    {"DPAD_UP", 0x0001}, {"DPAD_DOWN", 0x0002}, {"DPAD_LEFT", 0x0004}, {"DPAD_RIGHT", 0x0008},
    {"GUIDE", PAD_GUIDE},
};

const char* kButtonNames[BTN_COUNT] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L"};

std::string trim(std::string s) {
    auto ws = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](char c) { return !ws(c); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [&](char c) { return !ws(c); }).base(), s.end());
    return s;
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

int parse_key(const std::string& n) {
#ifdef _WIN32
    if (n.size() == 1 && std::isalnum(static_cast<unsigned char>(n[0]))) return n[0];
    if (n.size() >= 2 && n[0] == 'F' && std::isdigit(static_cast<unsigned char>(n[1]))) {
        const int f = std::atoi(n.c_str() + 1);
        if (f >= 1 && f <= 12) return VK_F1 + f - 1;
    }
    if (n.rfind("NUMPAD", 0) == 0 && n.size() == 7 && std::isdigit(static_cast<unsigned char>(n[6])))
        return VK_NUMPAD0 + (n[6] - '0');
    for (const auto& k : kKeys)
        if (n == k.name) return static_cast<int>(k.code);
#endif
    (void)n;
    return 0;
}

uint32_t parse_pad(const std::string& n) {
    for (const auto& p : kPad)
        if (n == p.name) return p.code;
    return 0;
}

bool parse_bool(const std::string& v, bool& out) {
    const std::string u = upper(v);
    if (u == "SIM" || u == "TRUE" || u == "1" || u == "YES") return out = true, true;
    if (u == "NAO" || u == "FALSE" || u == "0" || u == "NO") return out = false, true;
    return false;
}

int button_index(const std::string& n) {
    for (int i = 0; i < BTN_COUNT; ++i)
        if (n == kButtonNames[i]) return i;
    return -1;
}

void parse(const std::string& text, Config& c, std::vector<std::string>& warn) {
    std::istringstream in(text);
    std::string line, section;
    int lineno = 0;
    bool kb_cleared = false, pad_cleared = false; // seção no arquivo substitui o padrão
    while (std::getline(in, line)) {
        ++lineno;
        if (lineno == 1 && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
        if (auto sc = line.find(';'); sc != std::string::npos) line.resize(sc);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = upper(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const auto eq = line.find('=');
        const std::string where = "linha " + std::to_string(lineno);
        if (eq == std::string::npos) {
            warn.push_back(where + ": falta '='");
            continue;
        }
        const std::string key = upper(trim(line.substr(0, eq)));
        const std::string val = trim(line.substr(eq + 1));
        auto bad = [&] { warn.push_back(where + ": valor inválido para " + key); };

        if (section == "JANELA") {
            if (key == "ESCALA") {
                const int s = std::atoi(val.c_str());
                if (s >= 1 && s <= 10) c.scale = s;
                else bad();
            } else if (key == "TELA_CHEIA") {
                if (!parse_bool(val, c.fullscreen)) bad();
            } else bad();
        } else if (section == "AUDIO") {
            if (key == "VOLUME") {
                c.volume = std::clamp(std::atoi(val.c_str()), 0, 100);
            } else if (key == "MUDO") { // compatibilidade
                bool m = false;
                if (!parse_bool(val, m)) bad();
                else if (m) c.volume = 0;
            } else bad();
        } else if (section == "TECLADO" || section == "CONTROLE") {
            const bool kb = section == "TECLADO";
            if (!kb && key == "ANALOGICO") {
                if (!parse_bool(val, c.analog)) bad();
                continue;
            }
            if (!kb && key == "ZONA_MORTA") {
                c.deadzone = std::clamp(std::atoi(val.c_str()), 0, 32767);
                continue;
            }
            const int b = button_index(key);
            if (b < 0) {
                bad();
                continue;
            }
            if (kb && !kb_cleared) {
                for (auto& v : c.keyboard) v.clear();
                kb_cleared = true;
            }
            if (!kb && !pad_cleared) {
                for (auto& v : c.pad) v.clear();
                pad_cleared = true;
            }
            std::istringstream items(val);
            std::string item;
            while (std::getline(items, item, ',')) {
                item = upper(trim(item));
                if (item.empty()) continue;
                if (kb) {
                    if (const int vk = parse_key(item)) c.keyboard[b].push_back(vk);
                    else warn.push_back(where + ": tecla desconhecida '" + item + "'");
                } else {
                    if (const uint32_t m = parse_pad(item)) c.pad[b].push_back(m);
                    else warn.push_back(where + ": botão desconhecido '" + item + "'");
                }
            }
        } else if (section == "ATALHOS") {
            const int vk = parse_key(upper(val));
            int* target = key == "MENU" ? &c.key_menu
                        : key == "SALVAR_ESTADO" ? &c.key_save
                        : key == "CARREGAR_ESTADO" ? &c.key_load
                        : key == "SLOT_ANTERIOR" ? &c.key_slot_prev
                        : key == "PROXIMO_SLOT" ? &c.key_slot_next
                        : key == "TURBO" ? &c.key_turbo
                        : key == "PAUSA" ? &c.key_pause
                        : key == "TELA_CHEIA" ? &c.key_fullscreen
                        : key == "CAPTURA" ? &c.key_screenshot : nullptr;
            if (!target || !vk) bad();
            else *target = vk;
        } else {
            warn.push_back(where + ": fora de uma seção conhecida");
        }
    }
}

std::string join_keys(const std::vector<int>& v) {
    std::string s;
    for (int k : v) s += (s.empty() ? "" : ", ") + std::string(key_name(k));
    return s;
}

std::string join_pad(const std::vector<uint32_t>& v) {
    std::string s;
    for (uint32_t m : v) s += (s.empty() ? "" : ", ") + std::string(pad_name(m));
    return s;
}

} // namespace

Config default_config() {
    Config c;
#ifdef _WIN32
    c.keyboard[BTN_A] = {'X'};
    c.keyboard[BTN_B] = {'Z'};
    c.keyboard[BTN_L] = {'A'};
    c.keyboard[BTN_R] = {'S'};
    c.keyboard[BTN_START] = {VK_RETURN};
    c.keyboard[BTN_SELECT] = {VK_BACK};
    c.keyboard[BTN_UP] = {VK_UP};
    c.keyboard[BTN_DOWN] = {VK_DOWN};
    c.keyboard[BTN_LEFT] = {VK_LEFT};
    c.keyboard[BTN_RIGHT] = {VK_RIGHT};
    c.key_menu = VK_F1;
    c.key_save = VK_F5;
    c.key_load = VK_F9;
    c.key_slot_prev = VK_F6;
    c.key_slot_next = VK_F7;
    c.key_turbo = VK_TAB;
    c.key_pause = 'P';
    c.key_fullscreen = VK_F11;
    c.key_screenshot = VK_F12;
#endif
    c.pad[BTN_A] = {0x1000};            // A
    c.pad[BTN_B] = {0x2000, 0x4000};    // B, X
    c.pad[BTN_L] = {0x0100, PAD_LT};    // LB, LT
    c.pad[BTN_R] = {0x0200, PAD_RT};    // RB, RT
    c.pad[BTN_START] = {0x0010};
    c.pad[BTN_SELECT] = {0x0020};
    c.pad[BTN_UP] = {0x0001};
    c.pad[BTN_DOWN] = {0x0002};
    c.pad[BTN_LEFT] = {0x0004};
    c.pad[BTN_RIGHT] = {0x0008};
    return c;
}

const char* key_name(int vk) {
#ifdef _WIN32
    static char buf[16];
    if (vk >= VK_F1 && vk <= VK_F12) {
        std::snprintf(buf, sizeof buf, "F%d", vk - VK_F1 + 1);
        return buf;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        std::snprintf(buf, sizeof buf, "NUMPAD%d", vk - VK_NUMPAD0);
        return buf;
    }
    for (const auto& k : kKeys)
        if (static_cast<int>(k.code) == vk) return k.name;
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        buf[0] = static_cast<char>(vk);
        buf[1] = 0;
        return buf;
    }
#endif
    (void)vk;
    return "?";
}

const char* pad_name(uint32_t mask) {
    for (const auto& p : kPad)
        if (p.code == mask) return p.name;
    return "?";
}

const char* button_name(int b) { return b >= 0 && b < BTN_COUNT ? kButtonNames[b] : "?"; }

bool save_config(const std::filesystem::path& path, const Config& c) {
    std::ostringstream o;
    o << "; tlagba.ini — configuração do Avatar: The Last Airbender Recomp.\n"
         "; Também editável pela tela de configurações do jogo (Esc, F1 ou o botão Xbox/home).\n"
         "; Apague este arquivo para voltar aos padrões. Várias teclas por ação: separe por vírgula.\n\n"
         "[janela]\n"
      << "escala = " << c.scale << "            ; tamanho da janela (múltiplo de 240×160)\n"
      << "tela_cheia = " << (c.fullscreen ? "sim" : "nao") << "\n\n"
      << "[audio]\n"
      << "volume = " << c.volume << "          ; 0–100 (0 = mudo)\n\n"
      << "[teclado]\n"
         "; Teclas: A–Z, 0–9, F1–F12, UP, DOWN, LEFT, RIGHT, ENTER, BACKSPACE, SPACE, TAB, ESC,\n"
         ";         SHIFT, CTRL, INSERT, DELETE, HOME, END, PAGEUP, PAGEDOWN, NUMPAD0–NUMPAD9\n";
    const int order[BTN_COUNT] = {BTN_A, BTN_B, BTN_L, BTN_R, BTN_START, BTN_SELECT, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT};
    char line[160];
    for (int b : order) {
        std::snprintf(line, sizeof line, "%-6s = %s\n", kButtonNames[b], join_keys(c.keyboard[b]).c_str());
        o << line;
    }
    o << "\n[controle]\n"
         "; Botões (XInput, primeiro controle conectado): A, B, X, Y, LB, RB, LT, RT, BACK, START,\n"
         ";   LS, RS (clique dos analógicos), DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT\n";
    for (int b : order) {
        std::snprintf(line, sizeof line, "%-6s = %s\n", kButtonNames[b], join_pad(c.pad[b]).c_str());
        o << line;
    }
    o << "analogico = " << (c.analog ? "sim" : "nao") << "       ; analógico esquerdo também move\n"
      << "zona_morta = " << c.deadzone << "     ; 0–32767\n\n"
      << "[atalhos]\n"
      << "menu            = " << key_name(c.key_menu) << "   ; tela de configurações (Esc e o botão Xbox/home também)\n"
      << "salvar_estado   = " << key_name(c.key_save) << "\n"
      << "carregar_estado = " << key_name(c.key_load) << "\n"
      << "slot_anterior   = " << key_name(c.key_slot_prev) << "\n"
      << "proximo_slot    = " << key_name(c.key_slot_next) << "\n"
      << "turbo           = " << key_name(c.key_turbo) << "   ; segure para acelerar\n"
      << "pausa           = " << key_name(c.key_pause) << "\n"
      << "tela_cheia      = " << key_name(c.key_fullscreen) << "   ; Alt+Enter também funciona\n"
      << "captura         = " << key_name(c.key_screenshot) << "   ; salva PNG em screenshots\\\n";
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << o.str();
    return static_cast<bool>(f);
}

Config load_config(const std::filesystem::path& path, std::vector<std::string>& warnings) {
    Config c = default_config();
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        save_config(path, c); // primeira execução: cria o arquivo comentado
        return c;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::vector<std::string> w;
    parse(ss.str(), c, w);
    for (auto& s : w) warnings.push_back(path.filename().string() + ", " + s);
    return c;
}

} // namespace fe
