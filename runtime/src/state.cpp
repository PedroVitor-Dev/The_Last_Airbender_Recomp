// Save states: serialização de cada componente (formato em gba/state.h).

#include "gba/state.h"

#include "gba/apu.h"
#include "gba/gba.h"

namespace gba {

// ───────────────────────────────────────────────────────────── EEPROM ──
// Só o protocolo serial. O conteúdo é o save do jogo e não é sobrescrito por um save state.

void Eeprom::save_protocol(StateWriter& w) const {
    w.put(addr_bits_);
    w.put(state_);
    w.put(cmd_);
    w.put(addr_);
    w.put(bits_);
    w.put(buffer_);
    w.put(reading_);
    w.put(read_pos_);
}

void Eeprom::load_protocol(StateReader& r) {
    r.get(addr_bits_);
    r.get(state_);
    r.get(cmd_);
    r.get(addr_);
    r.get(bits_);
    r.get(buffer_);
    r.get(reading_);
    r.get(read_pos_);
}

// ──────────────────────────────────────────────────────────────── CPU ──

void Cpu::save(StateWriter& w) const {
    w.put(r);
    w.put(halted);
    w.put(n_), w.put(z_), w.put(c_), w.put(v_), w.put(i_), w.put(f_), w.put(t_);
    w.put(mode_);
    w.put(spsr_);
    w.put(bank_r13_), w.put(bank_r14_), w.put(bank_spsr_);
    w.put(bank_fiq_r8_), w.put(bank_usr_r8_);
}

void Cpu::load(StateReader& rd) {
    rd.get(r);
    rd.get(halted);
    rd.get(n_), rd.get(z_), rd.get(c_), rd.get(v_), rd.get(i_), rd.get(f_), rd.get(t_);
    rd.get(mode_);
    rd.get(spsr_);
    rd.get(bank_r13_), rd.get(bank_r14_), rd.get(bank_spsr_);
    rd.get(bank_fiq_r8_), rd.get(bank_usr_r8_);
}

// ──────────────────────────────────────────────────────────────── IO ──

void Io::save(StateWriter& w) const {
    w.put(regs_);
    w.put(dma);
    w.put(timers);
    w.put_vector(fifo[0]);
    w.put_vector(fifo[1]);
    w.put(keys_pressed_);
}

void Io::load(StateReader& r) {
    r.get(regs_);
    r.get(dma);
    r.get(timers);
    r.get_vector(fifo[0], 32);
    r.get_vector(fifo[1], 32);
    r.get(keys_pressed_);
}

// ──────────────────────────────────────────────────────────────── PPU ──

void Ppu::save(StateWriter& w) const {
    w.put(affine_x_);
    w.put(affine_y_);
    w.put(frame_); // a tela continua a mesma logo após carregar
}

void Ppu::load(StateReader& r) {
    r.get(affine_x_);
    r.get(affine_y_);
    r.get(frame_);
    bg_pal_valid_ = false;
    oam_valid_ = false;
}

// ──────────────────────────────────────────────────────────────── APU ──

void Apu::save(StateWriter& w) const {
    w.put(sample_num_);
    w.put(last_cycle_);
    w.put(seq_cycles_);
    w.put(seq_step_);
    w.put(sq_);
    w.put(wave_);
    w.put(noise_);
    w.put(wave_ram_);
    w.put(fifo_out_);
    w.put(lp_l_);
    w.put(lp_r_);
}

void Apu::load(StateReader& r) {
    r.get(sample_num_);
    r.get(last_cycle_);
    r.get(seq_cycles_);
    r.get(seq_step_);
    r.get(sq_);
    r.get(wave_);
    r.get(noise_);
    r.get(wave_ram_);
    r.get(fifo_out_);
    r.get(lp_l_);
    r.get(lp_r_);
    out_.clear();
}

// ──────────────────────────────────────────────────────────────── Gba ──

std::vector<uint8_t> Gba::save_state() const {
    StateWriter w;
    w.put_bytes(kStateMagic, sizeof kStateMagic);
    w.put(kStateVersion);
    char sha[40] = {};
    rom_sha1_.copy(sha, sizeof sha);
    w.put_bytes(sha, sizeof sha);

    w.put(frame_count);
    w.put(cycles);
    w.put(next_event);
    w.put(line_);
    w.put(in_hblank_);
    w.put(frame_done_);
    w.put(line_start_);
    w.put(video_event_);

    w.put(bus.ewram);
    w.put(bus.iwram);
    w.put(bus.palette);
    w.put(bus.vram);
    w.put(bus.oam);
    bus.eeprom.save_protocol(w);
    cpu.save(w);
    io.save(w);
    ppu.save(w);
    apu.save(w);
    return w.data();
}

void Gba::load_state(const std::vector<uint8_t>& bytes) {
    // Transacional: se o arquivo for inválido em qualquer ponto, volta ao estado anterior.
    const std::vector<uint8_t> backup = save_state();
    try {
        apply_state(bytes);
    } catch (...) {
        apply_state(backup);
        throw;
    }
}

void Gba::apply_state(const std::vector<uint8_t>& bytes) {
    StateReader r(bytes.data(), bytes.size());
    char magic[sizeof kStateMagic];
    r.get_bytes(magic, sizeof magic);
    if (std::memcmp(magic, kStateMagic, sizeof magic) != 0) throw std::runtime_error("not a save state");
    uint32_t version = 0;
    r.get(version);
    if (version != kStateVersion)
        throw std::runtime_error("save state from another version (" + std::to_string(version) + ", expected " +
                                 std::to_string(kStateVersion) + ")");
    char sha[40];
    r.get_bytes(sha, sizeof sha);
    if (std::string(sha, sizeof sha) != rom_sha1_) throw std::runtime_error("save state from another ROM");

    r.get(frame_count);
    r.get(cycles);
    r.get(next_event);
    r.get(line_);
    r.get(in_hblank_);
    r.get(frame_done_);
    r.get(line_start_);
    r.get(video_event_);

    r.get(bus.ewram);
    r.get(bus.iwram);
    r.get(bus.palette);
    r.get(bus.vram);
    r.get(bus.oam);
    bus.eeprom.load_protocol(r);
    cpu.load(r);
    io.load(r);
    ppu.load(r);
    apu.load(r);
    if (!r.at_end()) throw std::runtime_error("corrupted save state (trailing bytes)");
    frame_done_ = false; // o quadro salvo já foi entregue ao frontend
    cycle_limit = UINT64_MAX;
}

void Gba::load_state_and_restart(const std::vector<uint8_t>& bytes) {
    // Aplica (transacional: um arquivo ruim lança exceção e a execução atual continua
    // intacta) e só então desempilha o código em execução, que pertencia ao estado antigo.
    load_state(bytes);
    throw RestartRequested{};
}

} // namespace gba
