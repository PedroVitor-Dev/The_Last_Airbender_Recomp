#pragma once
// Serialização binária dos save states.
//
// Formato: "TLASTATE" + versão (u32) + SHA-1 da ROM (40 bytes) + seções na ordem fixa em que
// Gba::save_state as grava. Estruturas internas são copiadas byte a byte, então um estado só
// vale para a mesma versão do formato (kStateVersion) — mudou o layout, sobe a versão.

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace gba {

constexpr uint32_t kStateVersion = 1;
constexpr char kStateMagic[8] = {'T', 'L', 'A', 'S', 'T', 'A', 'T', 'E'};

class StateWriter {
public:
    template <typename T> void put(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>, "use put_bytes/put_vector");
        put_bytes(&v, sizeof v);
    }
    void put_bytes(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        data_.insert(data_.end(), b, b + n);
    }
    template <typename T> void put_vector(const std::vector<T>& v) {
        put(static_cast<uint32_t>(v.size()));
        if (!v.empty()) put_bytes(v.data(), v.size() * sizeof(T));
    }
    const std::vector<uint8_t>& data() const { return data_; }

private:
    std::vector<uint8_t> data_;
};

class StateReader {
public:
    StateReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    template <typename T> void get(T& v) {
        static_assert(std::is_trivially_copyable_v<T>, "use get_bytes/get_vector");
        get_bytes(&v, sizeof v);
    }
    void get_bytes(void* dst, size_t n) {
        if (pos_ + n > n_) throw std::runtime_error("truncated or corrupted save state");
        std::memcpy(dst, p_ + pos_, n);
        pos_ += n;
    }
    template <typename T> void get_vector(std::vector<T>& v, size_t max_elems) {
        uint32_t n = 0;
        get(n);
        if (n > max_elems) throw std::runtime_error("corrupted save state (invalid size)");
        v.resize(n);
        if (n) get_bytes(v.data(), n * sizeof(T));
    }
    bool at_end() const { return pos_ == n_; }

private:
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
};

} // namespace gba
