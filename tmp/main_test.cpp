// PIN ASSIGNMENTS

// P_SW1 48
// P_SW2 38
// P_SW3 39
// P_SW4 40

// ENC_CLK 37
// ENC_DT 36

// USB, SD and LCD all hooked to SPI #0
#define FSPI_MOSI 7
#define FSPI_MISO 10
#define FSPI_CLK 6

#define HSPI_MOSI 47
#define HSPI_MISO 21
#define HSPI_CLK 48


#define LCD_CS 11
#define LCD_DC 4
#define LCD_RST 8
// BL is hooked to +3.3v
#define LCD_BL -1
#define LCD_ROTATION 3
#define USB_CS 5
#define SD_CS 1
#include <Arduino.h>
#include <Wire.h>
#include <SPIFFS.h>
#include <SD.h>
#include <midiusb.h>
#include <usbh_midi.h>
#include <usbhub.h>
#include <tft_io.hpp>
#include <ili9341.hpp>
#include <gfx.hpp>
#include <sfx.hpp>
using namespace arduino;
using namespace sfx;
using namespace gfx;

using bus_t = tft_spi_ex<HSPI,LCD_CS,FSPI_MOSI,FSPI_MISO,FSPI_CLK,SPI_MODE0,true>;
using lcd_t = ili9342c<LCD_DC,LCD_RST,LCD_BL,bus_t,LCD_ROTATION,false,400,200>;
using color_t = color<typename lcd_t::pixel_type>;

lcd_t lcd;

MIDIusb midi_out;

USB Usb;
USBHub Hub(&Usb);
USBH_MIDI midi_in(&Usb);
int last_status = 0;
void MIDI_poll();

void onInit() {
    char buf[20];
    uint16_t vid = midi_in.idVendor();
    uint16_t pid = midi_in.idProduct();
    sprintf(buf, "VID:%04X, PID:%04X", vid, pid);
    Serial.println(buf);
}

void setup() {
    Serial.begin(115200);
    Serial.println("Prang booting");
    spi_container<FSPI>::instance().begin(HSPI_CLK,HSPI_MISO,HSPI_MOSI);
    spi_container<HSPI>::instance().begin(FSPI_CLK,FSPI_MISO,FSPI_MOSI);
    SPIFFS.begin(false);
    lcd.initialize();
    delay(200);
    if(SD.begin(SD_CS,spi_container<FSPI>::instance())){
        Serial.println("SD OK");
    } 
    
    lcd.fill(lcd.bounds(),color_t::red);
    if (Usb.Init() == -1) {
        while (1)
            ;  // halt
    }          // if (Usb.Init() == -1...
    delay(200);
#ifdef CONFIG_IDF_TARGET_ESP32S3
    midi_out.setBaseEP(3);
#endif
    char buf[256];
    strncpy(buf, "Prang MIDI Out", 255);
    midi_out.begin(buf);
#ifdef CONFIG_IDF_TARGET_ESP32S3
    midi_out.setBaseEP(3);
#endif
    delay(1000);
    Serial.println("MIDI Out registered");
    // Register onInit() function
    midi_in.attachOnInit(onInit);
    
}

void loop() {
    Usb.Task();
    if (midi_in) {
        MIDI_poll();
    }
}

// Poll USB MIDI Controler and send to serial MIDI
void MIDI_poll() {
    char buf[16];
    uint8_t bufMidi[MIDI_EVENT_PACKET_SIZE];
    uint16_t rcvd;

    if (midi_in.RecvData(&rcvd, bufMidi) == 0) {
        Serial.println("Received");
        uint8_t* p = bufMidi+1;
        if (((*p) & 0x80) != 0) {
            Serial.println("Set status");
            last_status = *p;
            ++p;
        } else {
            for (int i = 0; i < MIDI_EVENT_PACKET_SIZE; i++) {
                sprintf(buf, " %02X", bufMidi[i]);
                Serial.print(buf);
            }
            Serial.println();
        }
        bool note_on = false;
        int note;
        int vel;
        int j;
        int s = (last_status<0xF0)? last_status & 0xF0:last_status;
        switch ((midi_message_type)(s)) {
            case midi_message_type::note_on:
                note_on = true;
            case midi_message_type::note_off:
                note = *(p++);
                vel = *(p++);
                Serial.println("Send");
                tud_midi_stream_write(0,bufMidi+1,3);
                break;
            case midi_message_type::polyphonic_pressure:
            case midi_message_type::control_change:
            case midi_message_type::pitch_wheel_change:
            case midi_message_type::song_position:
                tud_midi_stream_write(0,bufMidi+1,3);
                break;
            case midi_message_type::program_change:
            case midi_message_type::channel_pressure:
            case midi_message_type::song_select:
                tud_midi_stream_write(0,bufMidi+1,2);
                break;
            case midi_message_type::system_exclusive:
                for(j=2;j<sizeof(bufMidi);++j) {
                    if(bufMidi[j]==0xF7) {
                        break;
                    }
                }  
                tud_midi_stream_write(0,bufMidi+1,j);
                break;
            case midi_message_type::reset:
            case midi_message_type::end_system_exclusive:
            case midi_message_type::active_sensing:
            case midi_message_type::start_playback:
            case midi_message_type::stop_playback:
            case midi_message_type::tune_request:
            case midi_message_type::timing_clock:
                tud_midi_stream_write(0,bufMidi+1,1);
                break;
        }
    }
}
