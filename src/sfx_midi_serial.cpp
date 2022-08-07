#include <midi_serial.hpp>
using namespace sfx;
namespace arduino {
    
    sfx_result midi_serial_input::receive(midi_message* out_message) {
        while(m_queue.empty()) {
            update();
        }
        midi_event_ex ex;
        if(m_queue.get(&ex)) {
            *out_message = ex.message;
            return sfx_result::success;
        }
        return sfx_result::end_of_stream;
    }
    sfx_result midi_serial_input::update() {
        midi_event_ex ex;
        if(m_stream.available()) {
            if(m_queue.full()) {
                m_queue.get(&ex);
            }
            midi_stream::decode_message(false,m_adapter,&m_last_message);    
            ex.message = m_last_message;
            m_queue.put(ex);
        }
        return sfx_result::success;
    }
    unsigned long long midi_serial_source::elapsed() const {
        return m_clock.elapsed();
    }
    sfx_result midi_serial_source::receive(midi_event* out_event) {
        while(m_queue.empty()) {
            update();
        }
        midi_event_ex ex;
        if(m_queue.get(&ex)) {
            out_event->message = ex.message;
            out_event->delta = ex.delta;
            return sfx_result::success;
        }
        return sfx_result::end_of_stream;

    }
    sfx_result midi_serial_source::reset() {
        m_clock.stop();
        m_clock.start();
        m_queue.clear();
        return sfx_result::success;
    }
    sfx_result midi_serial_source::update() {
        midi_event_ex ex;
        if(m_stream.available()) {
            if(m_queue.full()) {
                m_queue.get(&ex);
            }
            midi_stream::decode_message(false,m_adapter,&m_last_message);
            
            ex.absolute = m_clock.elapsed();
            ex.delta = ex.absolute- m_last_tick;
            ex.message = m_last_message;
            m_queue.put(ex);
            m_last_tick = ex.absolute;
        }
        m_clock.update();
        return sfx_result::success;
    }
}