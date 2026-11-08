// RX888 MKII: asynchronous USB -> bounded raw queue -> DDC -> bounded IQ queue.
// GPL-3.0-or-later.
#pragma once
#include "sample_queue.hpp"
#include "ddc.hpp"
#include "usb.hpp"
#include <sys/resource.h>
#include <pthread.h>
#include <unistd.h>
#include <memory>
#include <mutex>

namespace rx888 {
class RawQueue {
public:
    struct Block { std::vector<int16_t> data=std::vector<int16_t>(65536); size_t count=0; uint64_t offset=0; };
    std::array<Block,128> blocks;
    alignas(64) std::atomic<uint64_t> head{0},tail{0};
    void reset() { head.store(0); tail.store(0); }
    bool push(const int16_t* data,size_t count,uint64_t offset) {
        const auto h=head.load(std::memory_order_relaxed);
        if(h-tail.load(std::memory_order_acquire)>=blocks.size()) return false;
        auto& b=blocks[h%blocks.size()];
        std::memcpy(b.data.data(),data,count*sizeof(int16_t)); b.count=count; b.offset=offset;
        head.store(h+1,std::memory_order_release); return true;
    }
    const Block* front() {
        const auto t=tail.load(std::memory_order_relaxed);
        return t==head.load(std::memory_order_acquire) ? nullptr : &blocks[t%blocks.size()];
    }
    void release() { tail.store(tail.load(std::memory_order_relaxed)+1,std::memory_order_release); }
};
struct Radio {
    bool synthetic=false,streaming=false;
    uint32_t maximum_rate=4000000,frequency=137900000,gain=30,decimation=2;
    uint32_t serial=0,maximum_gain=49;
    double rate=1000000;
    std::string firmware_path;
    SampleQueue queue;
    std::atomic<uint64_t> received{0},queue_dropped{0},raw_dropped{0};
    std::atomic<size_t> peak_blocks{0};
    uint64_t retune_discarded=0;
    explicit Radio(bool(*stop)()) : external_stop_(stop) {}
    ~Radio() { halt(); }
    void initialize() {
        rate=maximum_rate/double(1u<<decimation);
        if(synthetic) { std::puts("SYNTHETIC: 35 kHz carrier; no RX888 is opened"); return; }
        usb_=std::make_unique<Usb>();
        usb_->open(firmware_path); serial=usb_->serial;
        apply_settings();
        std::printf("ADC %u MS/s real, USB payload %u MB/s; IQ output limited to 4 MS/s\n",adc_rate_/1000000,adc_rate_/500000);
    }
    uint64_t usb_errors() const { return usb_?usb_->errors.load():0; }
    uint64_t usb_bytes() const { return usb_?usb_->bytes.load():0; }
    void halt() noexcept {
        receiving_.store(false,std::memory_order_release);
        if(usb_) usb_->stop();
        if(worker_.joinable()) worker_.join();
        streaming=false;
    }
    void apply() {
        const bool resume=streaming; halt();
        rate=maximum_rate/double(1u<<decimation);
        if(resume) start();
        std::printf("IQ rate: %.0f S/s\n",rate);
    }
    void set_gain(uint32_t value) { gain=std::min(value,maximum_gain); if(usb_) configure_gain(); }
    void set_frequency(uint32_t value) {
        const bool resume=streaming; halt(); frequency=value;
        retune_discarded+=queue.discard_ready();
        if(usb_) apply_settings();
        if(resume) start();
    }
    void start() {
        if(streaming) return;
        const size_t chunk=std::clamp<size_t>(size_t(rate*0.016),1024,16384);
        queue.reset(std::clamp<size_t>(size_t(std::ceil(rate*0.5/chunk)),8,256));
        raw_.reset(); peak_blocks.store(0); failed_.store(false);
        receiving_.store(true); streaming=true;
        try {
            if(!synthetic) {
                ddc_=std::make_unique<Ddc>(adc_rate_,uint32_t(rate),vhf_?4570000:frequency,vhf_);
                uint64_t offset=0;
                usb_->start([this,offset](const int16_t* data,size_t count) mutable {
                    if(!receiving_.load()) return;
                    if(!raw_.push(data,count,offset)) raw_dropped.fetch_add(count);
                    offset+=count;
                });
            }
            worker_=std::thread([this,chunk] { capture(chunk); });
        } catch(...) { halt(); throw; }
        std::printf("RX started: %.0f S/s IQ; HF/VHF mode=%s\n",rate,vhf_?"VHF tuner":"HF direct");
    }
    void check_receiver() {
        if(usb_ && usb_->failed.load()) throw std::runtime_error("USB stream failed; check connection/power and export log");
        if(failed_.load(std::memory_order_acquire)) { std::lock_guard<std::mutex> lock(error_mutex_); throw std::runtime_error(error_); }
    }
private:
    bool(*external_stop_)();
    std::atomic<bool> receiving_{false},failed_{false};
    std::mutex error_mutex_;
    std::string error_;
    std::unique_ptr<Usb> usb_;
    std::unique_ptr<Ddc> ddc_;
    RawQueue raw_;
    std::thread worker_;
    bool vhf_=true;
    uint32_t gpio_=0;
    uint32_t adc_rate_=0;
    static int nearest(const float* steps,int count,float value) {
        int best=0;
        for(int i=1;i<count;++i) if(std::abs(steps[i]-value)<std::abs(steps[best]-value)) best=i;
        return best;
    }
    void configure_gain() {
        if(vhf_) {
            static constexpr float steps[]={0,.9f,1.4f,2.7f,3.7f,7.7f,8.7f,12.5f,14.4f,15.7f,16.6f,19.7f,20.7f,22.9f,25.4f,28,29.7f,32.8f,33.8f,36.4f,37.2f,38.6f,40.2f,42.1f,43.4f,43.9f,44.5f,48,49.6f};
            usb_->argument(R82XX_ATTENUATOR,uint16_t(nearest(steps,29,float(gain))));
            usb_->argument(R82XX_VGA,8); // Fixed 16.3 dB IF gain; SpyServer controls RF gain.
        } else {
            float steps[127];
            for(int i=0;i<127;++i) steps[i]=20*std::log10(i>18?.409f*(i-15):.059f*(i+1));
            const int index=nearest(steps,127,float(std::min(gain,33u)));
            usb_->argument(AD8340_VGA,uint16_t(index>18?(0x80|(index-15)):(index+1)));
            usb_->argument(DAT31_ATT,0);
        }
    }
    void apply_settings() {
        vhf_=frequency>=32000000;
        const uint32_t target_adc=vhf_?16000000:64000000;
        if(adc_rate_!=target_adc) {
            usb_->command(STARTADC,target_adc);
            // The stock FX3 handler settles the Si5351 for one second after EP0 ACK.
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            adc_rate_=target_adc;
        }
        gpio_=vhf_?VHF_EN:0; // Bias tee, ADC randomizer, dither and PGA disabled.
        if(vhf_) {
            usb_->argument(DAT31_ATT,63);
            usb_->command(GPIOFX3,gpio_);
            usb_->argument(AD8340_VGA,0x83);
            usb_->command(TUNERINIT,uint32_t(16000000));
            usb_->command(TUNERTUNE,uint64_t(frequency));
        } else {
            usb_->command(TUNERSTDBY,uint8_t(0));
            usb_->command(GPIOFX3,gpio_);
        }
        configure_gain();
    }
    void capture(size_t chunk) noexcept {
        pthread_setname_np(pthread_self(),"rx888-ddc");
        if(geteuid()==0) setpriority(PRIO_PROCESS,0,-4);
        SampleQueue::Block* out=nullptr; size_t filled=0;
        auto emit=[&](std::complex<float> v) {
            if(!filled) out=queue.writable();
            auto quantize=[](float x) { return int16_t(std::lrint(std::clamp(x,-32768.0f,32767.0f))); };
            if(out) out->data[filled]={quantize(v.real()),quantize(v.imag())};
            ++filled;
            if(filled==chunk) {
                received.fetch_add(filled,std::memory_order_relaxed);
                if(out) { queue.publish(filled); peak_blocks.store(std::max(peak_blocks.load(),queue.depth())); }
                else queue_dropped.fetch_add(filled);
                filled=0;
            }
        };
        try {
            if(synthetic) {
                double phase=0; auto due=std::chrono::steady_clock::now();
                const double step=2*3.14159265358979323846*35000/rate;
                while(receiving_.load() && !external_stop_()) {
                    auto osc=std::polar(6500.0,phase),rot=std::polar(1.0,step);
                    for(size_t k=0;k<chunk;++k) { emit(std::complex<float>(osc)); osc*=rot; }
                    phase=std::arg(osc);
                    due+=std::chrono::nanoseconds(uint64_t(chunk*1e9/rate)); std::this_thread::sleep_until(due);
                }
            } else {
                uint64_t expected=0; auto last=std::chrono::steady_clock::now();
                while(receiving_.load() && !external_stop_()) {
                    const auto* b=raw_.front();
                    if(!b) {
                        if(std::chrono::steady_clock::now()-last>std::chrono::seconds(3)) throw std::runtime_error("No USB ADC samples for 3 seconds");
                        std::this_thread::sleep_for(std::chrono::microseconds(200)); continue;
                    }
                    if(b->offset!=expected) ddc_->reset();
                    expected=b->offset+b->count;
                    ddc_->process(b->data.data(),b->count,emit);
                    raw_.release(); last=std::chrono::steady_clock::now();
                }
            }
        } catch(const std::exception& e) {
            { std::lock_guard<std::mutex> lock(error_mutex_); error_=e.what(); }
            failed_.store(true,std::memory_order_release);
        }
        receiving_.store(false);
    }
};
}
