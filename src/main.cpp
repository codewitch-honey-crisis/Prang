/*
ESP32S3 DevkitC
USBHostSheild 2.0 as shown here: https://www.pjrc.com/teensy/td_libs_USBHostShield.html
SD Card Reader
Rotary Encoder Knob
Two Momentary Microswitches
ILI9341 (designed for a 240x135 ST7789VW originally)

 USB Host Shield must be modified to power the VBUS at 5v instead of 3.3v
The resistor next to "2k2" must be removed.
A wire from 5v to the VBUS contact behind the USB port must be soldered. */

// PIN ASSIGNMENTS

// USB and LCD all hooked to HSPI
#define HSPI_MOSI 7
#define HSPI_MISO 10
#define HSPI_CLK 6

// SD hooked up to FSPI
#define FSPI_MOSI 47
#define FSPI_MISO 21
#define FSPI_CLK 48

// buttons are microswitch
// momentary pushbuttons
// closed high.
#define BUTTON_A 38
#define BUTTON_B 39
// a rotary encoder is attached
// as indicated
#define ENC_CLK 37
#define ENC_DATA 36

// LCD is configured as follows
#define LCD_CS 11
#define LCD_DC 4
#define LCD_RST 8
// BL is hooked to +3.3v
#define LCD_BL -1
#define LCD_ROTATION 3

// USB host CS
#define USB_CS 5

// SD Card reader CS
#define SD_CS 1

#include <Arduino.h>
#include <ESP32Encoder.h>
#include <SD.h>
#include <SPIFFS.h>
#include <Wire.h>
#include <midiusb.h>
#include <usbh_midi.h>
#include <usbhub.h>

#include <gfx.hpp>
#include <htcw_button.hpp>
#include <ili9341.hpp>
#include <message_queue.hpp>
#include <sfx.hpp>
#include <tft_io.hpp>
#include <thread.hpp>

#include "midi_esptinyusb.hpp"
#include "midi_quantizer.hpp"
#include "midi_sampler.hpp"
#include "telegrama.hpp"
using namespace arduino;
using namespace sfx;
using namespace gfx;
using namespace freertos;
using bus_t = tft_spi_ex<HSPI, LCD_CS, HSPI_MOSI, HSPI_MISO, HSPI_CLK, SPI_MODE0, true>;
using lcd_t = ili9342c<LCD_DC, LCD_RST, LCD_BL, bus_t, LCD_ROTATION, false, 400, 200>;
using color_t = color<typename lcd_t::pixel_type>;

midi_esptinyusb midi_out;
USB Usb;
USBHub Hub(&Usb);
USBH_MIDI midi_in(&Usb);

struct midi_file_info final {
    int type;
    int tracks;
    int32_t microtempo;
};

struct queue_info {
    int cmd;
    float value;
};
using message_queue_t = message_queue<queue_info>;
message_queue_t queue_to_thread;
message_queue_t queue_to_main;

button<BUTTON_A> button_a;
button<BUTTON_B> button_b;
ESP32Encoder encoder;
int64_t encoder_prev_count;
lcd_t lcd;
uint8_t* prang_font_buffer;
size_t prang_font_buffer_size;
buffer_stream prang_buffer_stream;
File file;
midi_sampler sampler;
midi_quantizer quantizer;
thread midi_thread;
midi_file_info file_info;
int last_status = 0;
float tempo_multiplier;
int base_octave;
int64_t encoder_old_count;
int quantize_beats;
int quantize_next_follow_track = -1;
int* quantize_track_adv;
int* quantized_pressed;
uint32_t off_ts;
void midi_task(void* state) {
    uint8_t buffer[MIDI_EVENT_PACKET_SIZE];
    uint16_t rcvd;
    int i, j;
    while (true) {
        queue_info qi;
        if (queue_to_thread.receive(&qi, false)) {
            switch (qi.cmd) {
                case 1:
                    sampler.tempo_multiplier(qi.value);
                    break;
                default:
                    break;
            }
        }
        Usb.Task();
        if (midi_in) {
            if (midi_in.RecvData(&rcvd, buffer) == 0) {
                const uint8_t* p = buffer + 1;
                last_status = *(p++);
                int base_note = base_octave * 12;
                bool note_on = false;
                int note;
                int vel;
                unsigned long long next_off_elapsed = 0;
                int next_off_track = -1;
                queue_info qi;
                int s = last_status;
                if (s < 0xF0) {
                    s &= 0xF0;
                }
                switch ((midi_message_type)s) {
                    case midi_message_type::note_on:
                        note_on = true;
                    case midi_message_type::note_off:
                        note = *(p++);
                        vel = *(p++);
                        // is the note within our captured notes?
                        if ((last_status & 0x0F) == 0 && 
                            note >= base_note && 
                            note < base_note + sampler.tracks_count()) {
                            if (note_on && vel > 0) {
                                quantizer.start(note - base_note);
                                qi.cmd = 1;
                                qi.value = (int)quantizer.last_timing();
                                queue_to_main.send(qi, false);
                            } else {
                                quantizer.stop(note - base_note);
                            }
                        } else {
                            // just forward it
                            tud_midi_stream_write(0, buffer + 1, 3);
                        }
                        break;
                    case midi_message_type::polyphonic_pressure:
                    case midi_message_type::control_change:
                    case midi_message_type::pitch_wheel_change:
                    case midi_message_type::song_position:
                        // length 3 message - forward it
                        tud_midi_stream_write(0, buffer + 1, 3);
                        break;
                    case midi_message_type::program_change:
                    case midi_message_type::channel_pressure:
                    case midi_message_type::song_select:
                        // length 2 message - forward it
                        tud_midi_stream_write(0, buffer + 1, 2);
                        break;
                    case midi_message_type::system_exclusive:
                        for (j = 2; j < sizeof(buffer); ++j) {
                            if (buffer[j] == 0xF7) {
                                break;
                            }
                        }
                        // sysex message - forward it
                        tud_midi_stream_write(0, buffer + 1, j);
                        break;
                    case midi_message_type::reset:
                    case midi_message_type::end_system_exclusive:
                    case midi_message_type::active_sensing:
                    case midi_message_type::start_playback:
                    case midi_message_type::stop_playback:
                    case midi_message_type::tune_request:
                    case midi_message_type::timing_clock:
                        // length 1 message - forward it
                        tud_midi_stream_write(0, buffer + 1, 1);
                        break;
                }
            }
        }
        sampler.update();
        vTaskDelay(1);
    }
}

void onInit() {
    char buf[20];
    uint16_t vid = midi_in.idVendor();
    uint16_t pid = midi_in.idProduct();
    sprintf(buf, "VID:%04X, PID:%04X", vid, pid);
    Serial.println(buf);
}
sfx_result scan_file(File& file, midi_file_info* out_info) {
    midi_file mf;
    // try to load the file into a memory buffer
    // and word from that
    file_stream fs(file);
    size_t len = (size_t)fs.seek(0,seek_origin::end);
    fs.seek(0,seek_origin::start);
    uint8_t* buf = (uint8_t*)malloc(len);
    buffer_stream bs(buf,len);
    if(buf!=nullptr) {
        if(len!=fs.read(buf,len)) {
            file.close();
            free(buf);
            return sfx_result::end_of_stream;
        }
        file.close();
    }
    stream& stm = buf==nullptr?(stream&)fs:bs;
    sfx_result r = midi_file::read(stm, &mf);
    if (r != sfx_result::success) {
        if(buf!=nullptr) {
            free(buf);
        } else {
            file.close();
        }
        return r;
    }
    out_info->tracks = (int)mf.tracks_size;
    int32_t file_mt = 500000;
    for (size_t i = 0; i < mf.tracks_size; ++i) {
        if (mf.tracks[i].offset != stm.seek(mf.tracks[i].offset)) {
            if(buf!=nullptr) {
                free(buf);
            } else {
                file.close();
            }
            return sfx_result::end_of_stream;
        }
        bool found_tempo = false;
        int32_t mt = 500000;
        midi_event_ex me;
        me.absolute = 0;
        me.delta = 0;
        while (stm.seek(0, seek_origin::current) < mf.tracks[i].size) {
            size_t sz = midi_stream::decode_event(true, stm, &me);
            if (sz == 0) {
                if(buf!=nullptr) {
                    free(buf);
                } else {
                    file.close();
                }
                return sfx_result::unknown_error;
            }
            if (me.message.status == 0xFF && me.message.meta.type == 0x51) {
                int32_t mt2 = (me.message.meta.data[0] << 16) |
                              (me.message.meta.data[1] << 8) |
                              me.message.meta.data[2];
                if (!found_tempo) {
                    found_tempo = true;
                    mt = mt2;
                    file_mt = mt;
                } else {
                    if (mt != file_mt) {
                        mt = 0;
                        file_mt = 0;
                        break;
                    }
                    if (mt != mt2) {
                        mt = 0;
                        file_mt = 0;
                        break;
                    }
                }
            }
        }
    }
    out_info->microtempo = file_mt;
    out_info->type = mf.type;
    if(buf!=nullptr) {
        free(buf);
    } else {
        file.close();
    }
    return sfx_result::success;
}
void load_prang_font(open_font* out_font) {
    file = SPIFFS.open("/PaulMaul.ttf", "rb");
    file.seek(0, SeekMode::SeekEnd);
    size_t sz = file.position() + 1;
    file.seek(0);
    prang_font_buffer = (uint8_t*)malloc(sz);
    if (prang_font_buffer == nullptr) {
        Serial.println("Out of memory loading font");
        while (true)
            ;
    }
    file.readBytes((char*)prang_font_buffer, sz);
    prang_font_buffer_size = sz;
    file.close();
    prang_buffer_stream.set(prang_font_buffer, prang_font_buffer_size);
    if (gfx_result::success != open_font::open(&prang_buffer_stream, out_font)) {
        Serial.println("Error loading font");
        while (true)
            ;
    }
}
void update_tempo_mult(bool send = true) {
    if (send) {
        queue_info qi;
        qi.cmd = 1;
        qi.value = tempo_multiplier;
        queue_to_thread.send(qi, true);
    }
    char sz[32];
    sprintf(sz, "x%0.2f", tempo_multiplier);
    open_text_info oti;
    oti.font = &Telegrama_otf;
    oti.scale = Telegrama_otf.scale(25);
    oti.transparent_background = false;
    oti.text = sz;
    ssize16 tsz = Telegrama_otf.measure_text(ssize16::max(), spoint16::zero(), oti.text, oti.scale);
    srect16 trc = tsz.bounds();
    trc.offset_inplace(lcd.dimensions().width - tsz.width - 2, 2);
    draw::filled_rectangle(lcd, trc.inflate(100, 0), color_t::white);
    draw::text(lcd, trc, oti, color_t::black, color_t::white);
}

static void draw_error(const char* text) {
    draw::filled_rectangle(lcd, lcd.bounds(), color_t::white);
    const open_font* pf = nullptr;
    open_font prangfnt;
    const_buffer_stream cbs(prang_font_buffer, prang_font_buffer_size);
    if (prang_font_buffer != nullptr) {
        if (gfx_result::success == open_font::open(&cbs, &prangfnt)) {
            pf = &prangfnt;
        }
    }
    if (pf == nullptr) {
        pf = &Telegrama_otf;
    }
    float scale = pf->scale(60);
    ssize16 sz = pf->measure_text(ssize16::max(), spoint16::zero(), text, scale);
    srect16 rect = sz.bounds().center((srect16)lcd.bounds());
    draw::text(lcd, rect, spoint16::zero(), text, *pf, scale, color_t::red, color_t::white, false);
}
void setup() {
    Serial.begin(115200);
    Serial.println("Prang booting");
    spi_container<FSPI>::instance().begin(FSPI_CLK, FSPI_MISO, FSPI_MOSI);
    spi_container<HSPI>::instance().begin(HSPI_CLK, HSPI_MISO, HSPI_MOSI);
    button_a.initialize();
    button_b.initialize();
    button_a.update();
    button_b.update();
    queue_to_main.initialize();
    queue_to_thread.initialize();
    bool reset_on_boot = false;
    if (button_a.pressed() || button_b.pressed()) {
        reset_on_boot = true;
    }
    ESP32Encoder::useInternalWeakPullResistors = UP;
    encoder_old_count = 0;
    encoder.attachFullQuad(ENC_CLK, ENC_DATA);
    SPIFFS.begin(false);
    lcd.initialize();
    open_font fnt;
    load_prang_font(&fnt);
    delay(200);
    draw_error("inSerT SD c4rD");
    while (!SD.begin(SD_CS, spi_container<FSPI>::instance())) {
        delay(25);
    }
    Serial.println("SD OK");
    lcd.fill(lcd.bounds(), color_t::red);
    if (Usb.Init() == -1) {
        Serial.println("USB Host initialization failure");
        while (true)
            ;  // halt
    }
    delay(200);
    midi_out.initialize("Prang MIDI Out");
    Serial.println("MIDI Out registered");
    // Register onInit() function
    midi_in.attachOnInit(onInit);
    free(prang_font_buffer);
    prang_font_buffer = nullptr;

restart:
    draw::filled_rectangle(lcd, lcd.bounds(), color_t::white);
    file = SPIFFS.open("/MIDI.jpg", "rb");
    gfx_result gr = draw::image(lcd, rect16(32, 0, lcd.dimensions().width - 1, lcd.dimensions().height - 1), &file);
    if (gr != gfx_result::success) {
        Serial.printf("Error loading MIDI.jpg (%d)\n", (int)gr);
        while (true)
            ;
    }
    file.close();
    load_prang_font(&fnt);
    float scale = fnt.scale(lcd.dimensions().height / 1.3);
    static const char* prang_txt = "pr4nG";
    ssize16 txt_sz = fnt.measure_text(ssize16::max(), spoint16::zero(), prang_txt, scale);
    srect16 txt_rct = txt_sz.bounds().center_horizontal((srect16)lcd.bounds());
    txt_rct.offset_inplace(0, lcd.dimensions().height - txt_sz.height);
    open_text_info pti;
    pti.font = &fnt;
    pti.scale = scale;
    pti.text = prang_txt;
    pti.no_antialiasing = true;
    draw::text(lcd, txt_rct, pti, color_t::red);
    if (SD.cardSize() == 0) {
        draw_error("insert SD card");
        while (true) {
            SD.end();
            SD.begin(SD_CS, spi_container<FSPI>::instance());
            file = SD.open("/", "r");
            if (!file) {
                delay(1);
            } else {
                file.close();
                break;
            }
        }

        free(prang_font_buffer);
        goto restart;
    }
    bool has_settings = false;
    if (SD.exists("/prang.csv")) {
        if (reset_on_boot) {
            SD.remove("/prang.csv");
        } else {
            file = SD.open("/prang.csv", "r");
            base_octave = file.parseInt();
            if (',' == file.read()) {
                quantize_beats = file.parseInt();
            }
            file.close();
            has_settings = true;
        }
    }
    free(prang_font_buffer);
    prang_font_buffer = nullptr;
    file = SD.open("/", "r");
    size_t fn_count = 0;
    size_t fn_total = 0;
    while (true) {
        File f = file.openNextFile();
        if (!f) {
            break;
        }
        if (!f.isDirectory()) {
            const char* fn = f.name();
            size_t fnl = strlen(fn);
            if ((fnl > 5 && ((0 == strcmp(".midi", fn + fnl - 5) ||
                              (0 == strcmp(".MIDI", fn + fnl - 5) ||
                               (0 == strcmp(".Midi", fn + fnl - 5)))))) ||
                (fnl > 4 && (0 == strcmp(".mid", fn + fnl - 4) ||
                             0 == strcmp(".MID", fn + fnl - 4)) ||
                 0 == strcmp(".Mid", fn + fnl - 4))) {
                ++fn_count;
                fn_total += fnl + 1;
            }
        }
        f.close();
    }
    file.close();
    char* fns = (char*)malloc(fn_total + 1) + 1;
    if (fns == nullptr) {
        draw_error("too many files");
        while (1)
            ;
    }
    midi_file_info* mfs = (midi_file_info*)malloc(fn_total * sizeof(midi_file_info));
    if (mfs == nullptr) {
        draw_error("too many files");
        while (1)
            ;
    }
    float loading_scale = Telegrama_otf.scale(15);
    char loading_buf[64];
    sprintf(loading_buf, "loading file 0 of %d", (int)fn_count);
    ssize16 loading_size = Telegrama_otf.measure_text(ssize16::max(), spoint16::zero(), loading_buf, loading_scale);
    srect16 loading_rect = loading_size.bounds().center_horizontal((srect16)lcd.bounds()).offset(0, lcd.dimensions().height - loading_size.height);
    draw::text(lcd, loading_rect, spoint16::zero(), loading_buf, Telegrama_otf, loading_scale, color_t::blue, color_t::white, false);
    file = SD.open("/", "r");
    char* str = fns;
    int fi = 0;
    int fli = 0;
    while (true) {
        File f = file.openNextFile();
        if (!f) {
            break;
        }
        if (!f.isDirectory()) {
            const char* fn = f.name();
            size_t fnl = strlen(fn);
            if ((fnl > 5 && ((0 == strcmp(".midi", fn + fnl - 5) ||
                              (0 == strcmp(".MIDI", fn + fnl - 5) ||
                               (0 == strcmp(".Midi", fn + fnl - 5)))))) ||
                (fnl > 4 && (0 == strcmp(".mid", fn + fnl - 4) ||
                             0 == strcmp(".MID", fn + fnl - 4)) ||
                 0 == strcmp(".Mid", fn + fnl - 4))) {
                ++fli;
                sprintf(loading_buf, "loading file %d of %d", fli, (int)fn_count);
                draw::filled_rectangle(lcd, loading_rect, color_t::white);
                loading_size = Telegrama_otf.measure_text(ssize16::max(), spoint16::zero(), loading_buf, loading_scale);
                loading_rect = loading_size.bounds().center_horizontal((srect16)lcd.bounds()).offset(0, lcd.dimensions().height - loading_size.height);
                draw::text(lcd, loading_rect, spoint16::zero(), loading_buf, Telegrama_otf, loading_scale, color_t::blue, color_t::white, false);
                if (sfx_result::success == scan_file(f, &mfs[fi])) {
                    memcpy(str, fn, fnl + 1);
                    str += fnl + 1;
                    ++fi;
                } else {
                    --fn_count;
                }
            }
        }
        f.close();
    }
    file.close();
    draw::filled_rectangle(lcd, lcd.bounds(), color_t::white);

    base_octave = 4;
    tempo_multiplier = 1.0;

    char* curfn = fns;
    size_t fni = 0;

    load_prang_font(&fnt);

    if (fn_count > 1) {
        static const char* seltext = "select filE";
        float fscale = fnt.scale(50);
        ssize16 tsz = fnt.measure_text(ssize16::max(), spoint16::zero(), seltext, fscale);
        srect16 trc = tsz.bounds().center_horizontal((srect16)lcd.bounds());
        draw::text(lcd, trc.offset(0, 5), spoint16::zero(), seltext, fnt, fscale, color_t::red, color_t::white, false);
        fscale = Telegrama_otf.scale(12);
        bool done = false;
        int64_t ocount = encoder.getCount() / 4;
        button_a.update();
        button_b.update();
        int osw = button_a.pressed() || button_b.pressed();

        while (!done) {
            tsz = Telegrama_otf.measure_text(ssize16::max(), spoint16::zero(), curfn, fscale);
            trc = tsz.bounds().center_horizontal((srect16)lcd.bounds()).offset(0, 58);
            draw::filled_rectangle(lcd, srect16(0, trc.y1, lcd.dimensions().width - 1, trc.y2 + trc.height() + 5).inflate(100, 0), color_t::white);
            rgb_pixel<16> px = color_t::black;
            if (mfs[fni].type == 1) {
                px = color_t::blue;
            } else if (mfs[fni].type != 2) {
                px = color_t::red;
            }
            draw::text(lcd, trc, spoint16::zero(), curfn, Telegrama_otf, fscale, px, color_t::white, false);
            char szt[64];
            sprintf(szt, "%d tracks", (int)mfs[fni].tracks);
            tsz = Telegrama_otf.measure_text(ssize16::max(), spoint16::zero(), szt, fscale);
            trc = tsz.bounds().center_horizontal((srect16)lcd.bounds()).offset(0, 73);
            draw::text(lcd, trc, spoint16::zero(), szt, Telegrama_otf, fscale, color_t::black, color_t::white, false);
            int32_t mt = mfs[fni].microtempo;
            if (mt == 0) {
                strcpy(szt, "tempo: varies");
            } else {
                sprintf(szt, "tempo: %0.1f", midi_utility::microtempo_to_tempo(mt));
            }
            tsz = Telegrama_otf.measure_text(ssize16::max(), spoint16::zero(), szt, fscale);
            trc = tsz.bounds().center_horizontal((srect16)lcd.bounds()).offset(0, 88);
            draw::filled_rectangle(lcd, srect16(0, trc.y1, lcd.dimensions().width - 1, trc.y2 + trc.height() + 5).inflate(100, 0), color_t::white);
            draw::text(lcd, trc, spoint16::zero(), szt, Telegrama_otf, fscale, color_t::black, color_t::white, false);
            bool inc;
            while (ocount == (encoder.getCount() / 4)) {
                button_a.update();
                button_b.update();
                int sw = button_a.pressed() || button_b.pressed();
                if (osw != sw && !sw) {
                    // button was released
                    done = true;
                    break;
                }
                osw = sw;
                delay(1);
            }
            if (!done) {
                int64_t count = (encoder.getCount() / 4);
                inc = (ocount > count);
                ocount = count;
                if (inc) {
                    if (fni < fn_count - 1) {
                        ++fni;
                        curfn += strlen(curfn) + 1;
                    }
                } else {
                    if (fni > 0) {
                        --fni;
                        curfn = fns;
                        for (int j = 0; j < fni; ++j) {
                            curfn += strlen(curfn) + 1;
                        }
                    }
                }
            }
        }
    }
    encoder_old_count = encoder.getCount() / 4;
    Serial.print("File: ");
    Serial.println(curfn);
    --curfn;
    *curfn = '/';
    file = SD.open(curfn, "rb");
    if (!file) {
        draw_error("re-insert SD card");
        while (true) {
            SD.end();
            SD.begin(SD_CS, spi_container<FSPI>::instance());
            file = SD.open(curfn, "rb");
            if (!file) {
                delay(1);
            } else {
                break;
            }
        }
    }
    file_info = mfs[fni];
    ::free(fns - 1);
    ::free(mfs);
    draw::filled_rectangle(lcd, lcd.bounds(), color_t::white);
    if (!has_settings) {
        static const char* oct_text = "base oct4vE";
        float fscale = fnt.scale(50);
        ssize16 tsz = fnt.measure_text(ssize16::max(), spoint16::zero(), oct_text, fscale);
        srect16 trc = tsz.bounds().center_horizontal((srect16)lcd.bounds());
        draw::text(lcd, trc.offset(0, 5), spoint16::zero(), oct_text, fnt, fscale, color_t::red, color_t::white, false);
        fscale = fnt.scale(50);
        bool done = false;
        int64_t ocount = encoder.getCount() / 4;
        button_a.update();
        button_b.update();
        int osw = button_a.pressed() || button_b.pressed();
        char sz[33];
        while (!done) {
            sprintf(sz, "%d", base_octave);
            fscale = Telegrama_otf.scale(25);
            tsz = Telegrama_otf.measure_text(ssize16::max(), spoint16::zero(), sz, fscale);
            trc = tsz.bounds().center_horizontal((srect16)lcd.bounds()).offset(0, 58);
            draw::filled_rectangle(lcd, srect16(0, trc.y1, lcd.dimensions().width - 1, trc.y2 + trc.height() + 5).inflate(100, 0), color_t::white);
            draw::text(lcd, trc, spoint16::zero(), sz, Telegrama_otf, fscale, color_t::black, color_t::white, false);

            bool inc;
            while (ocount == (encoder.getCount() / 4)) {
                button_a.update();
                button_b.update();
                int sw = button_a.pressed() || button_b.pressed();
                if (osw != sw && !sw) {
                    // button was released
                    done = true;
                    break;
                }
                osw = sw;
                delay(1);
            }
            if (!done) {
                int64_t count = (encoder.getCount() / 4);
                inc = (ocount > count);
                ocount = count;
                if (inc) {
                    if (base_octave < 10) {
                        ++base_octave;
                    }
                } else {
                    if (base_octave > 0) {
                        --base_octave;
                    }
                }
            }
        }
        draw::filled_rectangle(lcd, lcd.bounds(), color_t::white);
        static const char* qnt_text = "qu4ntiZE";
        fscale = fnt.scale(50);
        tsz = fnt.measure_text(ssize16::max(), spoint16::zero(), qnt_text, fscale);
        trc = tsz.bounds().center_horizontal((srect16)lcd.bounds());
        draw::text(lcd, trc.offset(0, 5), spoint16::zero(), qnt_text, fnt, fscale, color_t::red, color_t::white, false);
        fscale = fnt.scale(50);
        done = false;
        ocount = encoder.getCount() / 4;
        button_a.update();
        button_b.update();
        osw = button_a.pressed() || button_b.pressed();
        quantize_beats = 4;
        while (!done) {
            if (quantize_beats == 0) {
                strcpy(sz, "off");
            } else {
                sprintf(sz, "%d beats", quantize_beats);
            }
            fscale = Telegrama_otf.scale(25);
            tsz = Telegrama_otf.measure_text(ssize16::max(), spoint16::zero(), sz, fscale);
            trc = tsz.bounds().center_horizontal((srect16)lcd.bounds()).offset(0, 58);
            draw::filled_rectangle(lcd, srect16(0, trc.y1, lcd.dimensions().width - 1, trc.y2 + trc.height() + 5).inflate(100, 0), color_t::white);
            draw::text(lcd, trc, spoint16::zero(), sz, Telegrama_otf, fscale, color_t::black, color_t::white, false);

            bool inc;
            while (ocount == (encoder.getCount() / 4)) {
                button_a.update();
                button_b.update();
                int sw = button_a.pressed() || button_b.pressed();
                if (osw != sw && !sw) {
                    // button was released
                    done = true;
                    break;
                }
                osw = sw;
                delay(1);
            }
            if (!done) {
                int64_t count = (encoder.getCount() / 4);
                inc = (ocount > count);
                ocount = count;
                if (inc) {
                    if (quantize_beats < 16) {
                        ++quantize_beats;
                    }
                } else {
                    if (quantize_beats > 0) {
                        --quantize_beats;
                    }
                }
            }
        }
        draw::filled_rectangle(lcd, lcd.bounds(), color_t::white);
        static const char* save_text = "savE?";
        static const char* yes_text = "yes";
        static const char* no_text = "no";
        fscale = fnt.scale(50);
        tsz = fnt.measure_text(ssize16::max(), spoint16::zero(), save_text, fscale);
        trc = tsz.bounds().center((srect16)lcd.bounds());
        draw::text(lcd, trc, spoint16::zero(), save_text, fnt, fscale, color_t::red, color_t::white, false);
        fscale = Telegrama_otf.scale(25);
        tsz = Telegrama_otf.measure_text(ssize16::max(), spoint16::zero(), yes_text, fscale);
        trc = tsz.bounds();
        trc.offset_inplace(10, 0);
        open_text_info oti;
        oti.font = &Telegrama_otf;
        oti.transparent_background = false;
        oti.scale = fscale;
        oti.text = yes_text;
        draw::text(lcd, trc, oti, color_t::black, color_t::white);
        tsz = Telegrama_otf.measure_text(ssize16::max(), spoint16::zero(), no_text, fscale);
        trc = tsz.bounds();
        oti.text = no_text;
        trc.offset_inplace(10, lcd.dimensions().height - tsz.height);
        draw::text(lcd, trc, oti, color_t::black, color_t::white);
        int save = -1;
        while (button_a.pressed() || button_b.pressed()) {
            button_a.update();
            button_b.update();
        }
        while (0 > save) {
            button_a.update();
            button_b.update();
            if (button_a.pressed()) {
                save = 1;
            } else if (button_b.pressed()) {
                save = 0;
            }
            delay(1);
        }
        if (save) {
            if (SD.exists("/prang.csv")) {
                SD.remove("/prang.csv");
            }
            File file2 = SD.open("/prang.csv", "w", true);
            file2.print(base_octave);
            file2.print(",");
            file2.println(quantize_beats);
            file2.close();
        }
    }

    last_status = 0;
    const char* playing_text = "pLay1nG";
    float playing_scale = fnt.scale(100);
    ssize16 playing_size = fnt.measure_text(ssize16::max(), spoint16::zero(), playing_text, playing_scale);
    draw::filled_rectangle(lcd, lcd.bounds(), color_t::white);
    draw::text(lcd, playing_size.bounds().center((srect16)lcd.bounds()), spoint16::zero(), playing_text, fnt, playing_scale, color_t::red, color_t::white, false);

    free(prang_font_buffer);
    prang_font_buffer = nullptr;
    quantize_track_adv = (int*)malloc(file_info.tracks * sizeof(int));
    if (quantize_track_adv == nullptr) {
        draw_error("MIDI file too big");
        delay(5000);
        goto restart;
    }
    memset(quantize_track_adv, file_info.tracks, sizeof(int));

    quantized_pressed = (int*)malloc(file_info.tracks * sizeof(int));
    if (quantized_pressed == nullptr) {
        draw_error("MIDI file too big");
        delay(5000);
        goto restart;
    }
    memset(quantize_track_adv, file_info.tracks, sizeof(int));

    file_stream fs(file);
    sfx_result r = midi_sampler::read(fs, &sampler);
    if (r != sfx_result::success) {
        switch (r) {
            case sfx_result::out_of_memory:
                file.close();
                draw_error("file too big");
                delay(3000);
                goto restart;
            default:
                file.close();
                draw_error("not a MIDI file");
                delay(3000);
                goto restart;
        }
    }
    file.close();
    r = midi_quantizer::create(sampler, &quantizer);
    if (r != sfx_result::success) {
        draw_error("file too big");
        delay(3000);
        goto restart;
    }
    quantizer.quantize_beats(quantize_beats);
    Serial.printf("Free heap after MIDI file load: %f\n", ESP.getFreeHeap() / 1024.0);
    sampler.output(&midi_out);
    for (int i = 0; i < sampler.tracks_count(); ++i) {
        sampler.stop(i);
    }
    sampler.tempo_multiplier(tempo_multiplier);
    encoder_old_count = encoder.getCount() / 4;
    update_tempo_mult(false);
    off_ts = 0;
    midi_thread = thread::create_affinity(1 - thread::current().affinity(), midi_task, nullptr, 24, 4000);
    midi_thread.start();
}

void loop() {
    queue_info qi;

    if (queue_to_main.receive(&qi, false)) {
        if (qi.cmd == 1) {
            off_ts = millis() + 1000;
            auto px = color_t::white;
            switch ((int)qi.value) {
                case 0:
                    px = color_t::green;
                    break;
                case -1:
                    px = color_t::blue;
                    break;
                case 1:
                    px = color_t::red;
                    break;
                default:
                    break;
            }
            draw::filled_ellipse(lcd, rect16(point16(20, 20), 10), px);
        }
    }
    if (off_ts != 0 && millis() >= off_ts) {
        off_ts = 0;
        draw::filled_ellipse(lcd, rect16(point16(20, 20), 10), color_t::white);
    }
    bool inc;
    int64_t ec = (encoder.getCount() / 4);
    if (encoder_old_count != ec) {
        inc = (encoder_old_count > ec);
        encoder_old_count = ec;
        if (inc) {
            if (tempo_multiplier < 4.99) {
                tempo_multiplier += .01;
                update_tempo_mult();
            }
        } else {
            if (tempo_multiplier > .01) {
                tempo_multiplier -= .01;
                update_tempo_mult();
            }
        }
    }
}