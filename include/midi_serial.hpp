#pragma once
#include <Arduino.h>
#ifndef ARDUINO
#error "This library requires the Arduino framework"
#endif
#include <sfx.hpp>
namespace arduino {
    template<typename SerialType = HardwareSerial>
    class midi_serial_output final : public sfx::midi_output {
        SerialType& m_stream;
    public:
        constexpr static const unsigned long baud_rate = 31250;
        inline midi_serial_output(SerialType& serial) : m_stream(serial) {
        }
        inline void initialize() {
            m_stream.begin(baud_rate);
        }
        inline bool initialized() const {
            return m_stream.baudRate()==baud_rate;
        }
        virtual sfx::sfx_result send(const sfx::midi_message& message) {
            if(!m_stream.availableForWrite()) {
                return sfx::sfx_result::device_error;
            }
            uint8_t buf[3];
            if(message.type()==sfx::midi_message_type::meta_event && 
                    (message.meta.type!=0 || message.meta.data!=nullptr)) {
                return sfx::sfx_result::success;
            }
            if (message.type()==sfx::midi_message_type::system_exclusive) {
                m_stream.write(message.status);
                if(message.sysex.size) {
                    m_stream.write(message.sysex.data,message.sysex.size);
                }
                m_stream.write(0xF7);
            } else {
                // send a regular message
                // build a buffer and send it using raw midi
                buf[0] = message.status;
                switch (message.wire_size()) {
                    case 1:
                        m_stream.write(buf[0]);
                        break;
                    case 2:
                        buf[1] = message.value8;
                        m_stream.write(buf[0]);
                        m_stream.write(buf[1]);
                        break;
                    case 3:
                        buf[1] = message.msb();
                        buf[2] = message.lsb();
                        m_stream.write(buf[0]);
                        m_stream.write(buf[1]);
                        m_stream.write(buf[2]);
                        break;
                    default:
                        break;
                } 
            }
            return sfx::sfx_result::success;
        }
    };
    class midi_serial_input final : public sfx::midi_input {
        sfx::midi_buffer32 m_queue;
        HardwareSerial& m_stream;
        sfx::arduino_stream m_adapter;
        sfx::midi_message m_last_message;
    public:
        constexpr static const unsigned long baud_rate = 31250;
        inline midi_serial_input(HardwareSerial& serial) : m_stream(serial),m_adapter(&m_stream) {
            m_last_message.status = 0;
        }
        inline void initialize() {
            m_stream.begin(baud_rate);
        }
        inline bool initialized() const {
            return m_stream.baudRate()==baud_rate;
        }
        virtual sfx::sfx_result receive(sfx::midi_message* out_message);
        sfx::sfx_result update();
        bool available() const {
            return !m_queue.empty() || m_stream.available();
        }
    };
    class midi_serial_source final : public sfx::midi_source {
        sfx::midi_clock m_clock;
        sfx::midi_buffer32 m_queue;
        HardwareSerial& m_stream;
        sfx::arduino_stream m_adapter;
        sfx::midi_message m_last_message;
        unsigned long long m_last_tick;
    public:
        constexpr static const unsigned long baud_rate = 31250;
        inline midi_serial_source(HardwareSerial& serial) : m_stream(serial),m_adapter(&m_stream), m_last_tick(0) {
            m_last_message.status = 0;
        }
        inline void initialize() {
            if(!m_clock.started()) {
                m_stream.begin(baud_rate);
                m_clock.start();
            }
        }
        inline bool initialized() const {
            return m_clock.started();
        }
        virtual unsigned long long elapsed() const;
        virtual sfx::sfx_result receive(sfx::midi_event* out_event);
        virtual sfx::sfx_result reset();
            // gets the tempo
        inline double tempo() const {
            return m_clock.tempo();
        }
        // sets the tempo
        inline void tempo(double value) {
            m_clock.tempo(value);
        }
        // gets the MIDI microtempo
        inline int32_t microtempo() const {
            return m_clock.microtempo();
        }
        // sets the MIDI microtempo
        inline void microtempo(int32_t value) {
            m_clock.microtempo(value);
        }
        // gets the timebase in pulses per quarter note (PPQN)
        inline int16_t timebase() const {
            return m_clock.timebase();
        }
        // sets the timebase in pulses per quarter note (PPQN)
        inline void timebase(int16_t value) {
            m_clock.timebase(value);
        }
        sfx::sfx_result update();
    };
    
    
}
