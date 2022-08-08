#include "midi_quantizer.hpp"
using namespace sfx;
midi_quantizer::midi_quantizer(midi_quantizer&& rhs) {
    m_sampler = rhs.m_sampler;
    rhs.m_sampler = nullptr;
    m_deallocator = rhs.m_deallocator;
    rhs.m_deallocator = nullptr;
    m_follow_key = rhs.m_follow_key;
    m_last_key_ticks= rhs.m_last_key_ticks;
    m_last_timing = rhs.m_last_timing;
    m_key_advance = rhs.m_key_advance;
    rhs.m_key_advance = nullptr;
}
midi_quantizer& midi_quantizer::operator=(midi_quantizer&& rhs) {
    deallocate();
    m_sampler = rhs.m_sampler;
    rhs.m_sampler = nullptr;
    m_deallocator = rhs.m_deallocator;
    rhs.m_deallocator = nullptr;
    m_follow_key = rhs.m_follow_key;
    m_last_key_ticks= rhs.m_last_key_ticks;
    m_last_timing = rhs.m_last_timing;
    m_key_advance = rhs.m_key_advance;
    rhs.m_key_advance = nullptr;
    return *this;
}
void midi_quantizer::deallocate() {
    if(m_deallocator!=nullptr) {
        if(m_key_advance!=nullptr) {
            m_deallocator(m_key_advance);
            m_key_advance = nullptr;
        }
    }
}
sfx_result midi_quantizer::create(midi_sampler& sampler,midi_quantizer* out_quantizer, void*(*allocator)(size_t),void(*deallocator)(void*)) {
    out_quantizer->m_sampler = &sampler;
    out_quantizer->m_deallocator = deallocator;
    out_quantizer->m_quantize_beats = 4;
    out_quantizer->m_follow_key = -1;
    out_quantizer->m_last_key_ticks = 0;
    out_quantizer->m_last_timing = midi_quantizer_timing::none;
    out_quantizer->m_key_advance = (long*)allocator(sizeof(long)*sampler.tracks_count());
    if(out_quantizer->m_key_advance==nullptr) {
        return sfx_result::out_of_memory;
    }
    memset(out_quantizer->m_key_advance,0,sizeof(long)*sampler.tracks_count());
    return sfx_result::success;
}
void midi_quantizer::quantize_beats(int value) {
    if(value<0 || value > 128) {
        return;
    }
    m_quantize_beats = value;
}
sfx_result midi_quantizer::start(size_t index) {
    if(m_sampler==nullptr || 
            index<0||
            index>=m_sampler->tracks_count()) {
        return sfx_result::invalid_argument;
    }
    m_last_key_ticks = m_sampler->elapsed(index);
    if(!m_quantize_beats || m_follow_key==-1) {
        m_sampler->start(index);
        m_key_advance[index]=0;
        m_follow_key = index;
        m_last_timing = midi_quantizer_timing::exact;
        return sfx_result::success;
    }
    unsigned long long smp_elapsed; 
    unsigned long long adv=0;
    int tb = m_sampler->timebase(m_follow_key) 
                * m_quantize_beats;
    smp_elapsed=m_sampler->elapsed(m_follow_key)
                - m_key_advance[m_follow_key];
    adv= smp_elapsed % tb;
    unsigned long long adv2=adv-tb;
    if(adv>-adv2) {
        adv=adv2;
        m_last_timing = midi_quantizer_timing::early;
    } else if(adv!=0) {
        m_last_timing = midi_quantizer_timing::late;
    }
    sfx_result r = m_sampler->start(index,adv);
    if(r!=sfx_result::success) {
        return r;
    }
    m_key_advance[index]=adv;
    return sfx_result::success;
}
sfx_result midi_quantizer::stop(size_t index) {
    if(m_sampler==nullptr || 
            index<0||
            index>=m_sampler->tracks_count()) {
        return sfx_result::invalid_argument;
    }
    sfx_result r = m_sampler->stop(index);
    if(r!=sfx_result::success) {
        return r;
    }
    if(m_follow_key==index) {
        m_follow_key = -1;
        for(size_t i = 0;i<m_sampler->tracks_count();++i) {
            if(m_sampler->started(i)) {
                m_follow_key = i;
                break;
            }
        }
    }
    return sfx_result::success;
}