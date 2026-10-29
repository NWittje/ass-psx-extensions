#include "runtime.hpp"
#include "board.hpp"
#include "trim/instrument.hpp"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <algorithm>
#include <atomic>
#include <cstring>

namespace trim::esp32 {
static constexpr Role role=Role(TRIM_BUILD_ROLE);
static constexpr std::uint32_t firmware_version=0x01000000;
static std::uint64_t now() noexcept {return std::uint64_t(esp_timer_get_time());}
struct RenderReport {std::uint32_t session{},sequence{},stack{};bool applied{},fault{true};Mode mode{Mode::unavailable};};
static QueueHandle_t observations{}, reports{};
static TaskHandle_t render_handle{};
static std::atomic<std::uint32_t> completed{};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

class UartTransport {
public:
    bool initialize() noexcept {
        uart_config_t config{};config.baud_rate=board::baud;config.data_bits=UART_DATA_8_BITS;config.parity=UART_PARITY_DISABLE;
        config.stop_bits=UART_STOP_BITS_1;config.flow_ctrl=UART_HW_FLOWCTRL_DISABLE;config.source_clk=UART_SCLK_DEFAULT;
        // With the console disabled, IDF resets RX to a constant-high matrix input.
        // Explicit pads reconnect the USB bridge; NO_CHANGE would leave RX disconnected.
        return uart_param_config(board::uart,&config)==ESP_OK &&
            uart_set_pin(board::uart,board::uart_tx,board::uart_rx,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE)==ESP_OK &&
            uart_driver_install(board::uart,1024,0,16,&events_,0)==ESP_OK;
    }
    void step(Application& app,std::uint32_t& errors) noexcept {
        uart_event_t event{};
        if(xQueueReceive(events_,&event,pdMS_TO_TICKS(10))==pdTRUE){
            if(event.type==UART_FIFO_OVF||event.type==UART_BUFFER_FULL||event.type==UART_FRAME_ERR||event.type==UART_PARITY_ERR){
                ++errors;uart_flush_input(board::uart);xQueueReset(events_);app.link().reset_transport();
            }
        }
        std::array<std::uint8_t,256> input{};
        // Bounded draining leaves deadlines and status service runnable even under malformed traffic.
        for(unsigned i=0;i<4;++i){const int n=uart_read_bytes(board::uart,input.data(),input.size(),0);if(n<=0)break;app.link().receive(Bytes(input.data(),std::size_t(n)),now());}
        const auto pending=app.link().pending_tx();
        if(!pending.empty()){
            const int sent=uart_tx_chars(board::uart,reinterpret_cast<const char*>(pending.data()),std::uint32_t(pending.size()));
            if(sent>0)app.link().consume_tx(std::size_t(sent));
        }
    }
private:
    QueueHandle_t events_{};
};

class LcdOutput {
public:
    bool initialize() noexcept {
        if(initialized_)return true;
        gpio_config_t pins{};pins.pin_bit_mask=(1ull<<board::touch_cs)|(1ull<<board::sd_cs);pins.mode=GPIO_MODE_OUTPUT;
        if(gpio_config(&pins)!=ESP_OK)return false;
        gpio_set_level(gpio_num_t(board::touch_cs),1);gpio_set_level(gpio_num_t(board::sd_cs),1);
        if(!bus_){spi_bus_config_t bus{};bus.mosi_io_num=board::mosi;bus.miso_io_num=board::miso;bus.sclk_io_num=board::sck;
            bus.quadwp_io_num=-1;bus.quadhd_io_num=-1;bus.max_transfer_sz=max_strip_pixels*2;
            if(spi_bus_initialize(board::spi,&bus,SPI_DMA_CH_AUTO)!=ESP_OK)return false;
            bus_=true;}
        if(!buffers_[0])buffers_[0]=static_cast<std::uint16_t*>(heap_caps_aligned_alloc(4,max_strip_pixels*2,MALLOC_CAP_DMA|MALLOC_CAP_8BIT));
        if(!buffers_[1])buffers_[1]=static_cast<std::uint16_t*>(heap_caps_aligned_alloc(4,max_strip_pixels*2,MALLOC_CAP_DMA|MALLOC_CAP_8BIT));
        if(!buffers_[0]||!buffers_[1])return false;
        if(!io_){esp_lcd_panel_io_spi_config_t io{};io.cs_gpio_num=board::cs;io.dc_gpio_num=board::dc;io.spi_mode=0;io.pclk_hz=board::spi_hz;
            io.trans_queue_depth=2;io.lcd_cmd_bits=8;io.lcd_param_bits=8;io.on_color_trans_done=transfer_done;
            if(esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t(board::spi),&io,&io_)!=ESP_OK)return false;}
        if(!panel_){esp_lcd_panel_dev_config_t panel{};panel.reset_gpio_num=GPIO_NUM_NC;panel.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_BGR;panel.bits_per_pixel=16;
            if(esp_lcd_new_panel_st7796(io_,&panel,&panel_)!=ESP_OK)return false;}
        // Vendor portrait MADCTL is 0x48: BGR plus MX, with no XY exchange.
        if(esp_lcd_panel_reset(panel_)!=ESP_OK||esp_lcd_panel_init(panel_)!=ESP_OK||esp_lcd_panel_swap_xy(panel_,false)!=ESP_OK||esp_lcd_panel_mirror(panel_,true,false)!=ESP_OK||esp_lcd_panel_disp_on_off(panel_,true)!=ESP_OK)return false;
        if(!pwm_){ledc_timer_config_t timer{};timer.speed_mode=LEDC_LOW_SPEED_MODE;timer.duty_resolution=LEDC_TIMER_8_BIT;timer.timer_num=LEDC_TIMER_0;timer.freq_hz=10000;timer.clk_cfg=LEDC_AUTO_CLK;
            if(ledc_timer_config(&timer)!=ESP_OK)return false;
            ledc_channel_config_t channel{};channel.gpio_num=board::backlight;channel.speed_mode=LEDC_LOW_SPEED_MODE;channel.channel=LEDC_CHANNEL_0;channel.timer_sel=LEDC_TIMER_0;channel.duty=0;
            if(ledc_channel_config(&channel)!=ESP_OK)return false;
            pwm_=true;}
        initialized_=true;return true;
    }
    bool draw(TrimScene& scene,const Visual& visual,bool full) noexcept {
        const auto dirty=scene.changes(visual,full);
        for(std::size_t r=0;r<dirty.count;++r){const auto region=dirty.rects[r];
            for(int y=region.y;y<region.y+region.h;y+=strip_rows){
                const auto deadline=now()+500000;int slot=-1;
                do{drain();slot=slots_.claim();if(slot>=0)break;ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(10));}while(now()<deadline);
                if(slot<0)return false;
                const Rect strip{region.x,y,region.w,std::min(strip_rows,region.y+region.h-y)};
                auto pixels=std::span<std::uint16_t>(buffers_[slot],std::size_t(strip.w*strip.h));
                if(!scene.compose(strip,visual,pixels)){slots_.abandon(slot);return false;}
                // Panel consumes RGB565 MSB first; portable composition uses native uint16_t.
                for(auto& p:pixels)p=std::uint16_t((p<<8)|(p>>8));
                if(!slots_.submit(slot))return false;
                if(esp_lcd_panel_draw_bitmap(panel_,strip.x,strip.y,strip.x+strip.w,strip.y+strip.h,pixels.data())!=ESP_OK)return false;
            }
        }
        const auto deadline=now()+500000;
        while(!slots_.idle()&&now()<deadline){drain();if(!slots_.idle())ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(10));}
        if(!slots_.idle())return false;
        if(last_duty_!=visual.brightness){
            if(ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,visual.brightness)!=ESP_OK||ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0)!=ESP_OK)return false;
            last_duty_=visual.brightness;
        }
        scene.commit(visual);return true;
    }
    bool recover() noexcept {
        drain();if(!slots_.idle())return false; // Unknown DMA ownership must not be freed or overwritten.
        initialized_=false;
        return initialize();
    }
private:
    static bool transfer_done(esp_lcd_panel_io_handle_t,esp_lcd_panel_io_event_data_t*,void*) noexcept {
        completed.fetch_add(1,std::memory_order_release);BaseType_t woke=pdFALSE;
        if(render_handle)vTaskNotifyGiveFromISR(render_handle,&woke);
        return woke==pdTRUE;
    }
    void drain() noexcept {auto count=completed.exchange(0,std::memory_order_acquire);while(count--){if(!slots_.complete())break;}}
    esp_lcd_panel_io_handle_t io_{};
    esp_lcd_panel_handle_t panel_{};
    std::array<std::uint16_t*,2> buffers_{};
    DmaSlots slots_{};
    unsigned last_duty_{256};
    bool bus_{},pwm_{},initialized_{};
};

static void render_task(void*) {
    LcdOutput lcd;TrimScene scene(role);Observation latest{};latest.value.role=role;RenderReport report{};
    bool panel=false, full=true;std::uint64_t retry=0;
    for(;;){
        auto wait_ms=1000u;const auto before=now();
        if(panel&&latest.active&&latest.has_state&&before>=latest.received_us&&before-latest.received_us<stale_us)
            wait_ms=std::uint32_t(std::min<std::uint64_t>(1000,(stale_us-(before-latest.received_us)+999)/1000));
        if(full&&(panel||retry==0))wait_ms=0;
        xQueueReceive(observations,&latest,pdMS_TO_TICKS(wait_ms));const auto time=now();
        if(!panel&&time>=retry){panel=lcd.recover();full=true;retry=time+1000000;}
        if(panel){const auto visual=evaluate(latest,role,time);if(lcd.draw(scene,visual,full)){
            full=false;report.fault=false;report.session=latest.session;report.sequence=latest.sequence;report.mode=visual.mode;
            report.applied=latest.active&&latest.has_state;
        }else{panel=false;report.fault=true;report.applied=false;report.mode=Mode::unavailable;retry=time+1000000;}}
        report.stack=std::uint32_t(uxTaskGetStackHighWaterMark(nullptr));xQueueOverwrite(reports,&report);
    }
}
static void communication_task(void*) {
    psx::hardware::Descriptor descriptor{};std::array<std::uint8_t,6> mac{};
    esp_read_mac(mac.data(),ESP_MAC_WIFI_STA);for(auto b:mac)descriptor.device=(descriptor.device<<8)|b;
    descriptor.boot=(std::uint64_t(esp_random())<<32)|esp_random();descriptor.firmware=firmware_version;
    descriptor.board=0x3501;descriptor.profile=1;descriptor.profile_revision=1;descriptor.metadata_size=33;descriptor.metadata[0]=std::uint8_t(role);
    const auto hash=assets::identity(role);std::copy(hash.begin(),hash.end(),descriptor.metadata.begin()+1);
    Application app(descriptor,role);UartTransport uart;
    if(!uart.initialize()){vTaskDelete(nullptr);return;}
    std::uint32_t errors=0;std::uint64_t status_due=now();RenderReport rendered{};Observation published{};
    for(;;){uart.step(app,errors);const auto time=now();app.link().service(time);
        const auto& current=app.observation();
        if(current.received_us!=published.received_us||current.sequence!=published.sequence||current.session!=published.session||current.active!=published.active||current.has_state!=published.has_state){xQueueOverwrite(observations,&current);published=current;}
        xQueuePeek(reports,&rendered,0);
        if(time>=status_due){app.report(rendered.session,rendered.sequence,rendered.applied,rendered.fault,rendered.mode,
            std::uint32_t(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)),std::uint32_t(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),errors,
            std::uint32_t(uxTaskGetStackHighWaterMark(nullptr)),rendered.stack);status_due=time+1000000;}
    }
}
void run() noexcept {
    observations=xQueueCreate(1,sizeof(Observation));reports=xQueueCreate(1,sizeof(RenderReport));
    if(!observations||!reports)return;
    if(xTaskCreate(render_task,"trim-render",6144,nullptr,4,&render_handle)!=pdPASS)return;
    if(xTaskCreate(communication_task,"hardware-link",6144,nullptr,5,nullptr)!=pdPASS)vTaskDelete(render_handle);
}
}
