#pragma once
#include "midi_sampler.hpp"
#include <string.h>
enum struct midi_quantizer_timing {
    none = -2,
    early = -1,
    exact = 0,
    late = 1
};
class midi_quantizer final {
    midi_sampler* m_sampler;
    size_t m_quantize_beats;
    size_t m_follow_key;
    long* m_key_advance;
    midi_quantizer_timing m_last_timing;
    unsigned long long m_last_key_ticks;
    void(*m_deallocator)(void*);
    void deallocate();
    midi_quantizer(const midi_quantizer& rhs)=delete;
    midi_quantizer& operator=(const midi_quantizer& rhs)=delete;
public:
    inline midi_quantizer() : m_sampler(nullptr),m_key_advance(nullptr),m_deallocator(nullptr) {}
    midi_quantizer(midi_quantizer&& rhs);
    midi_quantizer& operator=(midi_quantizer&& rhs);
    inline ~midi_quantizer() { deallocate(); }
    inline size_t quantize_beats() const { return m_quantize_beats; }
    inline midi_sampler& sampler() const { return *m_sampler; }
    inline unsigned long long last_key_ticks() const { return m_last_key_ticks;}
    inline midi_quantizer_timing last_timing() const { return m_last_timing;}
    void quantize_beats(int value);
    sfx::sfx_result start(size_t index);
    sfx::sfx_result stop(size_t index);
    static sfx::sfx_result create(midi_sampler& sampler,midi_quantizer* out_quantizer, void*(*allocator)(size_t)=::malloc,void(*deallocator)(void*)=::free);
};