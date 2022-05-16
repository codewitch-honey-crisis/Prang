#include <Arduino.h>
#include <SPIFFS.h>
#include <SD.h>
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include <ESP32Encoder.h>
#include <tft_io.hpp>

#include <sfx_midi_file.hpp>
#include <sfx_midi_driver.hpp>
#include "midi_esptinyusb.hpp"
#include <ili9341.hpp>
#include <gfx_cpp14.hpp>
#include "telegrama.hpp"
#include "midi_sampler.hpp"
using namespace std;
using namespace sfx;
using namespace gfx;
using namespace arduino;

// PIN ASSIGNMENTS

#define P_SW1 48
#define P_SW2 38
#define P_SW3 39
#define P_SW4 40

#define ENC_CLK 37
#define ENC_DT 36

// SD and LCD both hooked to SPI #0
#define SPI_MOSI 7
#define SPI_MISO 10
#define SPI_CLK 6

#define LCD_CS 5
#define LCD_DC 4
#define LCD_RST 8
// BL is hooked to +3.3v
#define LCD_BL -1
#define LCD_ROTATION 3
#define SD_CS 11


using bus_t = tft_spi_ex<0,LCD_CS,SPI_MOSI,SPI_MISO,SPI_CLK,SPI_MODE0,false>;
using lcd_t = ili9341<LCD_DC,LCD_RST,LCD_BL,bus_t,LCD_ROTATION,false,400,200>;
using color_t = color<typename lcd_t::pixel_type>;

lcd_t lcd;
ESP32Encoder encoder;
int64_t encoder_old_count;
float tempo_multiplier;
midi_sampler sampler;
uint8_t* prang_font_buffer;
size_t prang_font_buffer_size;
midi_esptinyusb out;
int switches[4];
int follow_track = -1;
RingbufHandle_t signal_queue;
TaskHandle_t display_task;

static void draw_error(const char* text) {
    draw::filled_rectangle(lcd,lcd.bounds(),color_t::white);
    const open_font* pf=nullptr;
    open_font prangfnt;
    const_buffer_stream cbs(prang_font_buffer,prang_font_buffer_size);
    if(prang_font_buffer!=nullptr) {
        if(gfx_result::success==open_font::open(&cbs,&prangfnt)) {
            pf = &prangfnt;
        }
    } 
    if(pf==nullptr) {
        pf = &Telegrama_otf;
    }
    float scale = pf->scale(60);
    ssize16 sz = pf->measure_text(ssize16::max(),spoint16::zero(),text,scale);
    srect16 rect = sz.bounds().center((srect16)lcd.bounds());
    draw::text(lcd,rect,spoint16::zero(),text,*pf,scale,color_t::red,color_t::white,false);
}
void setup() {
    Serial.begin(115200);
    Serial.printf("Free heap on start: %f\n",ESP.getFreeHeap()/1024.0);
    pinMode(P_SW1,INPUT_PULLDOWN);
    pinMode(P_SW2,INPUT_PULLDOWN);
    pinMode(P_SW3,INPUT_PULLDOWN);
    pinMode(P_SW4,INPUT_PULLDOWN);
    memset(switches,0,sizeof(switches));
    ESP32Encoder::useInternalWeakPullResistors=UP;
    encoder_old_count = 0;
    encoder.attachFullQuad(ENC_CLK,ENC_DT);
    SPIFFS.begin();
    // ensure the SPI bus is initialized
    lcd.initialize();
    SD.begin(SD_CS,spi_container<0>::instance());
    tempo_multiplier = 1.0;
    signal_queue = xRingbufferCreate(sizeof(float) * 8 + (sizeof(float) - 1), RINGBUF_TYPE_NOSPLIT);
    if(signal_queue==nullptr) {
        Serial.println("Unable to create signal queue");
        while(true);
    }
    if(pdPASS!=xTaskCreatePinnedToCore([](void* state){
        float scale = Telegrama_otf.scale(20);
        while(true) {
            size_t fs=sizeof(float);
            float* pf=(float*)xRingbufferReceive(signal_queue,&fs,0);
            if(nullptr!=pf) {
                float f = *pf;
                vRingbufferReturnItem(signal_queue,pf);
                char text[64];
                sprintf(text,"tempo x%0.1f",f);
                ssize16 sz = Telegrama_otf.measure_text(ssize16::max(),spoint16::zero(),text,scale);
                srect16 rect = sz.bounds().center_horizontal((srect16)lcd.bounds()).offset(0,3);
                draw::filled_rectangle(lcd,srect16(0,rect.y1,lcd.dimensions().width-1,rect.y2),color_t::white);
                draw::text(lcd,rect,spoint16::zero(),text,Telegrama_otf,scale,color_t::black,color_t::white,false);
            }
        }
    },"Display Task",4000,nullptr,0,&display_task,1-xPortGetCoreID())) {
        Serial.println("Unable to create display task");
        while(true);
    }
    
    
restart:
    lcd.fill(lcd.bounds(),color_t::white);
    File file = SPIFFS.open("/MIDI.jpg","rb");
    draw::image(lcd,rect16(0,0,319,144).center_horizontal(lcd.bounds()),&file);
    file.close();
    file = SPIFFS.open("/PaulMaul.ttf","rb");
    file.seek(0,SeekMode::SeekEnd);
    size_t sz = file.position()+1;
    file.seek(0);
    prang_font_buffer=(uint8_t*)malloc(sz);
    if(prang_font_buffer==nullptr) {
        Serial.println("Out of memory loading font");
        while(true);
    }
    file.readBytes((char*)prang_font_buffer,sz);
    prang_font_buffer_size = sz;
    file.close();
    const_buffer_stream fntstm(prang_font_buffer,prang_font_buffer_size);

    open_font prangfnt;
    gfx_result gr = open_font::open(&fntstm,&prangfnt);
    if(gr!=gfx_result::success) {
        Serial.println("Error loading font.");
        while(true);
    }
    const char* title = "pr4nG";
    float title_scale = prangfnt.scale(200);
    ssize16 title_size = prangfnt.measure_text(ssize16::max(),
                                            spoint16::zero(),
                                            title,
                                            title_scale);
    draw::text(lcd,
            title_size.bounds().center_horizontal((srect16)lcd.bounds()).offset(0,45),
            spoint16::zero(),
            title,
            prangfnt,
            title_scale,
            color_t::red,
            color_t::white,
            true,
            true);
    if(SD.cardSize()==0) {
        draw_error("insert SD card");
        while(true) {
            SD.end();
            SD.begin(SD_CS,spi_container<0>::instance());
            file=SD.open("/","r");
            if(!file) {
                delay(1);
            } else {
                file.close();
                break;
            }
        }
        
        free(prang_font_buffer);
        goto restart;
    }
    file = SD.open("/","r");
    size_t fn_count = 0;
    size_t fn_total = 0;
    while(true) {
        File f = file.openNextFile();
        if(!f) {
            break;
        }
        if(!f.isDirectory()) {
            const char* fn = f.name();
            size_t fnl = strlen(fn);
            if((fnl>5 && ((0==strcmp(".midi",fn+fnl-5) || 
                        (0==strcmp(".MIDI",fn+fnl-5) ||
                        (0==strcmp(".Midi",fn+fnl-5))))))||
            (fnl>4 && (0==strcmp(".mid",fn+fnl-4) || 
                        0==strcmp(".MID",fn+fnl-4))  || 
                        0==strcmp(".Mid",fn+fnl-4))) {
                ++fn_count;
                fn_total+=fnl+1;
            }
        }
        f.close();
    }
    file.close();
    char* fns = (char*)malloc(fn_total+1)+1;
    if(fns==nullptr) {
        draw_error("too many files");
        while(1);
    }
    midi_file* mfs = (midi_file*)malloc(fn_total*sizeof(midi_file));
    if(mfs==nullptr) {
        draw_error("too many files");
        while(1);
    }
    file = SD.open("/","r");
    char* str = fns;
    int fi = 0;
    while(true) {
        File f = file.openNextFile();
        if(!f) {
            break;
        }
        if(!f.isDirectory()) {
            const char* fn = f.name();
            size_t fnl = strlen(fn);
            if((fnl>5 && ((0==strcmp(".midi",fn+fnl-5) || 
                        (0==strcmp(".MIDI",fn+fnl-5) ||
                        (0==strcmp(".Midi",fn+fnl-5))))))||
            (fnl>4 && (0==strcmp(".mid",fn+fnl-4) || 
                        0==strcmp(".MID",fn+fnl-4))  || 
                        0==strcmp(".Mid",fn+fnl-4))) {
                memcpy(str,fn,fnl+1);
                str+=fnl+1;
                file_stream ffs(f);
                midi_file::read(&ffs,&mfs[fi]);
                ++fi; 
            }    
        }
        f.close();
    }
    file.close();
    draw::filled_rectangle(lcd,lcd.bounds(),color_t::white);
    char* curfn = fns;
    if(fn_count>1) {
        const char* seltext = "select filE";
        float fscale = prangfnt.scale(80);
        ssize16 tsz = prangfnt.measure_text(ssize16::max(),spoint16::zero(),seltext,fscale);
        srect16 trc = tsz.bounds().center_horizontal((srect16)lcd.bounds());
        draw::text(lcd,trc.offset(0,20),spoint16::zero(),seltext,prangfnt,fscale,color_t::red,color_t::white,false);
        fscale = Telegrama_otf.scale(20);
        bool done = false;
        size_t fni=0;
        int64_t ocount = encoder.getCount()/4;
        int osw = digitalRead(P_SW1) || digitalRead(P_SW2) || digitalRead(P_SW3) || digitalRead(P_SW4);
        
        while(!done) {
            tsz= Telegrama_otf.measure_text(ssize16::max(),spoint16::zero(),curfn,fscale);
            trc = tsz.bounds().center_horizontal((srect16)lcd.bounds()).offset(0,110);
            draw::filled_rectangle(lcd,srect16(0,trc.y1,lcd.dimensions().width-1,trc.y2+trc.height()+5),color_t::white);
            rgb_pixel<16> px=color_t::black;
            if(mfs[fni].type==1) {
                px=color_t::blue;
            } else if(mfs[fni].type!=2) {
                px=color_t::red;
            }
            draw::text(lcd,trc,spoint16::zero(),curfn,Telegrama_otf,fscale,px,color_t::white,false);
            char szt[64];
            sprintf(szt,"%d tracks",(int)mfs[fni].tracks_size);
            tsz= Telegrama_otf.measure_text(ssize16::max(),spoint16::zero(),szt,fscale);
            trc = tsz.bounds().center_horizontal((srect16)lcd.bounds()).offset(0,133);
            draw::text(lcd,trc,spoint16::zero(),szt,Telegrama_otf,fscale,color_t::black,color_t::white,false);
            bool inc;
            while(ocount==(encoder.getCount()/4)) {
                int sw = digitalRead(P_SW1) || digitalRead(P_SW2) || digitalRead(P_SW3) || digitalRead(P_SW4);
                if(osw!=sw && !sw) {
                    // button was released
                    done = true;
                    break;
                }
                osw=sw;
                delay(1);
            }
            if(!done) {
                int64_t count = (encoder.getCount()/4);
                inc = ocount>count;
                ocount = count;
                if(inc) {
                    if(fni<fn_count-1) {
                        ++fni;
                        curfn+=strlen(curfn)+1;
                    }
                } else {
                    if(fni>0) {
                        --fni;
                        curfn=fns;
                        for(int j = 0;j<fni;++j) {
                            curfn+=strlen(curfn)+1;
                        }
                    }
                }
            }
        }
    }
    // avoids the 1 second init delay later
    out.initialize();
    tempo_multiplier = 1.0;
    encoder_old_count = encoder.getCount()/4;
    --curfn;
    *curfn='/';
    file = SD.open(curfn, "rb");
    if(!file) {
        draw_error("re-insert SD card");
        while(true) {
            SD.end();
            SD.begin(SD_CS,spi_container<0>::instance());
            file=SD.open(curfn,"rb");
            if(!file) {
                delay(1);
            } else {
                break;
            }
        }
    }
    ::free(fns-1);
    ::free(mfs);
    draw::filled_rectangle(lcd,lcd.bounds(),color_t::white);
    const char* playing_text = "pLay1nG";
    float playing_scale = prangfnt.scale(125);
    ssize16 playing_size = prangfnt.measure_text(ssize16::max(),spoint16::zero(),playing_text,playing_scale);
    draw::text(lcd,playing_size.bounds().center((srect16)lcd.bounds()),spoint16::zero(),playing_text,prangfnt,playing_scale,color_t::red,color_t::white,false);
    free(prang_font_buffer);
    Serial.printf("Free heap before MIDI file load: %f\n",ESP.getFreeHeap()/1024.0);
    file_stream fs(file);
    sfx_result r=midi_sampler::read(&fs,&sampler);
    if(r!=sfx_result::success) {
        switch(r) {
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
    sampler.output(&out);
    Serial.printf("Free heap after MIDI file load: %f\n",ESP.getFreeHeap()/1024.0);
    
    xRingbufferSend(signal_queue,&tempo_multiplier,sizeof(tempo_multiplier),0);
    while(true) {

        int64_t ec = encoder.getCount()/4;
        if(ec!=encoder_old_count) {
            bool inc = ec<encoder_old_count;
            encoder_old_count=ec;
            if(inc && tempo_multiplier<=4.9) {
                tempo_multiplier+=.1;
                sampler.tempo_multiplier(tempo_multiplier);
                xRingbufferSend(signal_queue,&tempo_multiplier,sizeof(tempo_multiplier),0);
            } else if(tempo_multiplier>.1) {
                tempo_multiplier-=.1;
                sampler.tempo_multiplier(tempo_multiplier);
                xRingbufferSend(signal_queue,&tempo_multiplier,sizeof(tempo_multiplier),0);
            }
        }
        bool first_track = follow_track == -1;
        bool changed = false;
        int b=digitalRead(P_SW1);
        if(b!=switches[0]) {
            changed = true;
            if(b) {
                if(first_track) {
                    follow_track = 0;
                    sampler.start(0);
                } else {
                    sampler.start(0,sampler.elapsed(follow_track) % sampler.timebase(follow_track));
                }
                
            } else {
                sampler.stop(0);
            }
            switches[0]=b;
        }
        b=digitalRead(P_SW2);
        if(b!=switches[1]) {
            changed = true;
            if(b) {
                if(first_track) {
                    follow_track = 1;
                    sampler.start(1);
                } else {
                    sampler.start(1,sampler.elapsed(follow_track) % sampler.timebase(follow_track));
                }
                
            } else {
                sampler.stop(1);
            }
            switches[1]=b;
        }
        b=digitalRead(P_SW3);
        if(b!=switches[2]) {
            changed = true;
            if(b) {
                if(first_track) {
                    follow_track = 2;
                    sampler.start(2);
                } else {
                    sampler.start(2,sampler.elapsed(follow_track) % sampler.timebase(follow_track));
                }
            } else {
                sampler.stop(2);
            }
            switches[2]=b;
        }
        b=digitalRead(P_SW4);
        if(b!=switches[3]) {
            changed = true;
            if(b) {
                if(first_track) {
                    follow_track = 3;
                    sampler.start(3);
                } else {
                    sampler.start(3,sampler.elapsed(follow_track) % sampler.timebase(follow_track));
                }
            } else {
                sampler.stop(3);
            }
            switches[3]=b;
        }
        if(follow_track!=-1 && !switches[follow_track]) {
            // find the next follow track
            follow_track = -1;
            for(int i = 0;i<4;++i) {
                if(switches[i]) {
                    follow_track = i;
                    break;
                }
            }
        }
        sampler.update();    
    }
    
}
void loop() {
    ESP.restart();
}
