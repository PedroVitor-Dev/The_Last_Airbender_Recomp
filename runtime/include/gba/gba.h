#pragma once
// Runtime do GBA: hardware reimplementado + interpretador ARM7TDMI de referência.
//
// Organização:
//   Gba      — dono de tudo; agendador por scanline (run_frame)
//   Bus      — mapa de memória (BIOS stub, EWRAM, IWRAM, IO, PAL, VRAM, OAM, ROM, EEPROM)
//   Cpu      — estado do ARM7TDMI e interpretador (usa core::arm para decodificar)
//   Ppu      — renderizador por scanline
//   Io       — registradores, DMA, timers, IRQ, teclado, FIFO de som

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/rom.h"
#include "gba/apu.h"

namespace gba {

constexpr int kScreenW = 240;
constexpr int kScreenH = 160;
constexpr uint32_t kCyclesPerLine = 1232;
constexpr uint32_t kHDrawCycles = 960;
constexpr uint32_t kLinesPerFrame = 228;
constexpr uint32_t kCyclesPerFrame = kCyclesPerLine * kLinesPerFrame; // 280 896

// Bits de interrupção (IE/IF)
enum Irq : uint16_t {
    IRQ_VBLANK = 1 << 0, IRQ_HBLANK = 1 << 1, IRQ_VCOUNT = 1 << 2,
    IRQ_TIMER0 = 1 << 3, IRQ_TIMER1 = 1 << 4, IRQ_TIMER2 = 1 << 5, IRQ_TIMER3 = 1 << 6,
    IRQ_SERIAL = 1 << 7, IRQ_DMA0 = 1 << 8, IRQ_DMA1 = 1 << 9, IRQ_DMA2 = 1 << 10, IRQ_DMA3 = 1 << 11,
    IRQ_KEYPAD = 1 << 12, IRQ_GAMEPAK = 1 << 13,
};

// Botões (bit em KEYINPUT; 0 = pressionado no hardware)
enum Key : uint16_t {
    KEY_A = 1 << 0, KEY_B = 1 << 1, KEY_SELECT = 1 << 2, KEY_START = 1 << 3,
    KEY_RIGHT = 1 << 4, KEY_LEFT = 1 << 5, KEY_UP = 1 << 6, KEY_DOWN = 1 << 7,
    KEY_R = 1 << 8, KEY_L = 1 << 9,
};

class Gba;

// ───────────────────────────────────────────────────────────── EEPROM ──
class Eeprom {
public:
    void set_addr_bits(int bits);           // 6 (512 B) ou 14 (8 KB)
    int addr_bits() const { return addr_bits_; }
    void write_bit(uint16_t v);
    uint16_t read_bit();
    std::vector<uint8_t>& data() { return data_; }
    bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }

private:
    enum class State { Idle, Command, Address, WriteData, End, ReadDummy, ReadData };
    std::vector<uint8_t> data_ = std::vector<uint8_t>(8192, 0xFF);
    int addr_bits_ = 14;
    State state_ = State::Idle;
    uint32_t cmd_ = 0, addr_ = 0, bits_ = 0;
    uint64_t buffer_ = 0;
    bool reading_ = false, dirty_ = false;
    int read_pos_ = 0;
};

// ──────────────────────────────────────────────────────────────── Bus ──
class Bus {
public:
    explicit Bus(Gba& gba);

    uint8_t read8(uint32_t a);
    uint16_t read16(uint32_t a);
    uint32_t read32(uint32_t a);
    void write8(uint32_t a, uint8_t v);
    void write16(uint32_t a, uint16_t v);
    void write32(uint32_t a, uint32_t v);

    // Ciclos aproximados de um acesso (para o orçamento de tempo do interpretador).
    static uint32_t access_cycles(uint32_t a, bool wide);

    std::array<uint8_t, 0x4000> bios{};
    std::array<uint8_t, 0x40000> ewram{};
    std::array<uint8_t, 0x8000> iwram{};
    std::array<uint8_t, 0x400> palette{};
    std::array<uint8_t, 0x18000> vram{};
    std::array<uint8_t, 0x400> oam{};
    const std::vector<uint8_t>* rom = nullptr;
    Eeprom eeprom;
    uint64_t io_accesses = 0; // diagnóstico (teste diferencial): leituras/escritas em IO

private:
    static uint32_t vram_offset(uint32_t a) {
        uint32_t o = a & 0x1FFFF;
        return o >= 0x18000 ? o - 0x8000 : o;
    }
    Gba& gba_;
};

// ──────────────────────────────────────────────────────────────── CPU ──
enum Mode : uint32_t { MODE_USR = 0x10, MODE_FIQ = 0x11, MODE_IRQ = 0x12, MODE_SVC = 0x13,
                       MODE_ABT = 0x17, MODE_UND = 0x1B, MODE_SYS = 0x1F };

class Cpu {
public:
    explicit Cpu(Gba& gba);

    void reset_after_bios();       // estado que a BIOS deixa antes de pular para 0x08000000
    uint32_t step();               // executa 1 instrução (ou atende IRQ); devolve ciclos

    uint32_t cpsr() const;
    void set_cpsr(uint32_t v);
    bool thumb() const { return t_; }
    uint32_t pc() const { return r[15]; }

    std::array<uint32_t, 16> r{};
    bool halted = false;
    uint64_t instr_count = 0;

    // Trace opcional: chamado antes de cada instrução (endereço, thumb).
    std::function<void(uint32_t, bool)> on_exec;

    // Estado exposto ao código recompilado (flags, modo, banco atual).
    bool n_ = false, z_ = false, c_ = false, v_ = false;
    bool i_ = false, f_ = false, t_ = false;
    uint32_t mode_ = MODE_SYS;
    uint32_t spsr_ = 0;
    uint64_t irq_count = 0; // IRQs atendidas (para o HALT saber que acordou)

    void switch_mode(uint32_t new_mode);
    void enter_irq();
    void copy_state_from(const Cpu& o); // tudo menos a referência ao Gba e o trace

private:
    int bank_index(uint32_t mode) const;
    void branch_to(uint32_t addr, bool thumb);

    uint32_t user_reg(int n) const;
    void set_user_reg(int n, uint32_t v);

    Gba& gba_;
    // Bancos: índice 0 = usr/sys, 1 = fiq, 2 = irq, 3 = svc, 4 = abt, 5 = und
    std::array<uint32_t, 6> bank_r13_{}, bank_r14_{}, bank_spsr_{};
    std::array<uint32_t, 5> bank_fiq_r8_{}, bank_usr_r8_{};
};

// ──────────────────────────────────────────────────────────────── IO ──
struct DmaChannel {
    uint32_t sad = 0, dad = 0;   // registradores
    uint32_t cnt_l = 0;
    uint16_t cnt_h = 0;
    uint32_t src = 0, dst = 0, count = 0; // internos (latched ao habilitar)
};

struct Timer {
    uint16_t reload = 0;
    uint16_t control = 0;
    uint16_t counter = 0;
    uint64_t last_update = 0; // ciclo da última atualização do contador
    uint32_t sub = 0;         // ciclos acumulados abaixo do prescaler
};

class Io {
public:
    explicit Io(Gba& gba);

    uint16_t read16(uint32_t off);
    void write16(uint32_t off, uint16_t v);
    uint8_t read8(uint32_t off);
    void write8(uint32_t off, uint8_t v);

    uint16_t& reg(uint32_t off) { return regs_[(off & 0x3FF) >> 1]; }

    void raise(uint16_t irq);
    bool irq_ready() const;   // IME && (IE & IF)
    bool irq_any() const;     // IE & IF (acorda do HALT mesmo com IME=0)

    // Agendador
    void on_hblank(int line);
    void on_vblank();
    void update_timers(uint64_t now);
    uint64_t next_timer_event(uint64_t now) const;

    void set_keys(uint16_t pressed); // máscara de Key pressionadas
    void copy_state_from(const Io& o);

    std::array<DmaChannel, 4> dma;
    std::array<Timer, 4> timers;
    // Amostras do FIFO de som (consumidas pelo mixer de áudio do frontend, futuro)
    std::array<std::vector<int8_t>, 2> fifo;
    // Diagnóstico: amostras consumidas e quantas vezes o FIFO estava vazio no tick.
    std::array<uint64_t, 2> fifo_ticks{}, fifo_underruns{};

private:
    void dma_write_control(int ch, uint16_t v);
    void run_dma(int ch);
    void trigger_dma(int timing);
    void fifo_tick(int timer, uint64_t at);
    // `first_at`/`interval`: instante do 1º estouro e distância entre estouros (ciclos);
    // interval = 0 quando o instante exato não é conhecido (cascata).
    void timer_overflow(int t, uint32_t times, uint64_t first_at, uint64_t interval);

    Gba& gba_;
    std::array<uint16_t, 0x200> regs_{};
    uint16_t keys_pressed_ = 0;
};

// ──────────────────────────────────────────────────────────────── PPU ──
class Ppu {
public:
    explicit Ppu(Gba& gba);
    void render_line(int line);
    void latch_affine();            // início do VBlank: recarrega referência X/Y
    void on_affine_ref_write(int bg); // escrita em BGxX/BGxY
    void copy_state_from(const Ppu& o) { affine_x_ = o.affine_x_; affine_y_ = o.affine_y_; }
    const std::array<uint32_t, kScreenW * kScreenH>& frame() const { return frame_; }

private:
    friend struct PpuTestAccess; // tests/test_ppu.cpp compara a composição com a referência
    void render_text_bg(int bg, int line);
    void render_affine_bg(int bg);
    void render_bitmap(int mode, int line);
    void render_sprites(int line);
    void compose(int line);

    Gba& gba_;
    std::array<uint32_t, kScreenW * kScreenH> frame_{};
    // Linha atual: cor (BGR555 | 0x8000 = opaco) por camada e prioridade dos sprites
    std::array<std::array<uint16_t, kScreenW>, 4> bg_line_{};
    std::array<uint16_t, kScreenW> obj_line_{};
    std::array<uint8_t, kScreenW> obj_prio_{};
    std::array<bool, kScreenW> obj_semi_{};
    std::array<bool, kScreenW> obj_window_{};
    uint8_t obj_prio_used_ = 0; // bit p = algum pixel de sprite com prioridade p nesta linha
    std::array<int32_t, 2> affine_x_{}, affine_y_{};

    // Paleta de BG pré-convertida (BGR555 | opaco; 0 = transparente) para 4 bpp (índice 0 de
    // cada banco transparente) e 8 bpp (só o índice 0). Reconstruída quando a paleta muda:
    // a PPU compara com a cópia em cache uma vez por linha, então qualquer caminho de escrita
    // (CPU, DMA, BIOS, cópia de estado) é percebido.
    void refresh_bg_palette();
    std::array<uint8_t, 512> bg_pal_cache_{};
    std::array<uint16_t, 256> bg_pal4_{}, bg_pal8_{};
    bool bg_pal_valid_ = false;

    // Sprites ativos (habilitados, forma válida) com sua faixa vertical [top, bottom), em
    // ordem de OAM. Reconstruída quando a OAM muda (comparação com cópia em cache por
    // linha); cada linha percorre só esta lista em vez dos 128 registros.
    struct ObjSpan {
        int16_t top, bottom;
        uint8_t n;
    };
    void refresh_oam();
    std::array<uint8_t, 0x400> oam_cache_{};
    std::array<ObjSpan, 128> obj_spans_{};
    int obj_span_count_ = 0;
    bool oam_valid_ = false;
};

// ─────────────────────────────────────────────────── código recompilado ──
using RecompFn = void (*)(Gba&);

struct RecompEntry {
    uint32_t addr;  // bit 0 = Thumb
    RecompFn fn;
};

// Código da ROM copiado para a RAM e recompilado no endereço da RAM. Só é usado
// enquanto a RAM ainda contém exatamente aquela cópia.
struct RecompOverlay {
    uint32_t ram, rom, size;
};

// Lançada de dentro de sync() quando o frontend pede para parar.
struct StopRequested {};

// ──────────────────────────────────────────────────────────────── Gba ──
class Gba {
public:
    explicit Gba(const core::Rom& rom);

    // Registra as funções recompiladas (nullptr/0 = só interpretador).
    void set_recomp(const RecompEntry* table, size_t count, const RecompOverlay* overlays = nullptr,
                    size_t overlay_count = 0);
    bool has_recomp() const { return !recomp_.empty(); }

    // Executa até on_frame devolver false. on_frame é chamado ao fim de cada quadro.
    void run(const std::function<bool()>& on_frame);

    void set_keys(uint16_t pressed) { io.set_keys(pressed); }
    const std::array<uint32_t, kScreenW * kScreenH>& frame() const { return ppu.frame(); }
    uint64_t now() const { return cycles; }
    void request_sync() { next_event = cycles; }
    // Copia o estado emulado de outra instância (memórias, CPU, IO, vídeo). Não copia
    // a APU nem as tabelas de código recompilado. Usado pelo teste diferencial.
    void copy_state_from(const Gba& o);
    // Vigia: sync() lança StopRequested quando cycles passa deste valor.
    uint64_t cycle_limit = UINT64_MAX;
    bool render = true; // diagnóstico: false pula a renderização (não altera o estado do jogo)

    // ── API usada pelo código recompilado ──
    void sync();                                 // processa eventos vencidos (vídeo, timers)
    void sync_at(uint32_t pc);                   // idem + HALT + IRQ, retomando em `pc`
    void call_function(uint32_t addr, bool thumb); // despacho por endereço (ou interpretador)
    void jump_indirect(uint32_t target);         // salto indireto (bit 0 = Thumb)
    void resume_until(uint32_t ret);             // continua em r15 até voltar para `ret`
    void swi(uint32_t number, uint32_t next_pc);
    [[noreturn]] void unimplemented(uint32_t pc, const char* what);

    Bus bus;
    Cpu cpu;
    Io io;
    Ppu ppu;
    Apu apu;
    uint64_t frame_count = 0;
    uint64_t cycles = 0;
    uint64_t next_event = 0;

    // Estatísticas do fallback: endereço → nº de entradas pelo interpretador.
    std::unordered_map<uint32_t, uint64_t> fallback_entries;
    std::unordered_map<uint32_t, uint64_t> fallback_instrs; // instruções interpretadas por entrada
    std::unordered_map<uint32_t, uint64_t> interp_regions;  // início de trecho interpretado → instruções
    uint64_t recomp_calls = 0;

private:
    RecompFn find(uint32_t addr, bool thumb) const;
    void run_hybrid_until(uint32_t ret);
    void halt_wait();
    void service_irq();
    void advance_video();
    void begin_line();

    std::vector<uint8_t> rom_;
    std::unordered_map<uint32_t, RecompFn> recomp_;
    std::vector<uint8_t> recomp_bitmap_; // 1 bit por halfword da ROM: há função aqui?
    std::vector<RecompOverlay> overlays_;
    RecompFn find_overlay(uint32_t addr, bool thumb) const;
    std::function<bool()> on_frame_;

    // Vídeo
    uint32_t line_ = 0;
    bool in_hblank_ = false;
    bool frame_done_ = false;
    uint64_t line_start_ = 0;
    uint64_t video_event_ = kHDrawCycles;
};

// BIOS reimplementada (SWIs). Devolve false se a SWI não for suportada.
bool bios_swi(Gba& gba, uint32_t number);
void bios_install_stub(Bus& bus);

} // namespace gba
