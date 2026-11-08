#include "ddc.hpp"
#include <cstdio>
#include <chrono>
#include <stdexcept>
using namespace rx888;
constexpr double pi=3.14159265358979323846;
static std::complex<double> measure(uint32_t rate,double center,double offset,bool flip,int mode=0,uint32_t adc=64000000) {
    Ddc ddc(adc,rate,center,flip);
    std::vector<int16_t> data(65536);
    std::vector<std::complex<float>> output;
    const double tone=center+offset;
    uint64_t input_count=0;
    const uint64_t total=mode==1?6400000:1048576;
    while(input_count<total) {
        const size_t count=std::min<size_t>(data.size(),total-input_count);
        for(size_t k=0;k<count;++k) data[k]=int16_t(std::lrint(10000*std::cos(2*pi*tone*(input_count+k)/adc)));
        // Split deliberately at non-FFT boundaries to exercise partial USB blocks.
        const size_t first=std::min<size_t>(1001,count);
        ddc.process(data.data(),first,[&](auto x){output.push_back(x);});
        ddc.process(data.data()+first,count-first,[&](auto x){output.push_back(x);});
        input_count+=count;
    }
    if(output.size()!=total/(adc/rate)) throw std::runtime_error("Wrong output sample count");
    const size_t skip=512;
    std::complex<double> correlation{}; double power=0;
    const double expected=(flip?-offset:offset);
    for(size_t i=skip;i<output.size();++i) {
        correlation+=std::complex<double>(output[i])*std::polar(1.0,-2*pi*expected*i/rate);
        power+=std::norm(output[i]);
    }
    correlation/=double(output.size()-skip);
    const double rms=std::sqrt(power/(output.size()-skip));
    std::printf("DDC adc=%u rate=%u center=%.0f offset=%.0f flip=%d coherent=%.2f rms=%.2f\n",adc,rate,center,offset,flip,std::abs(correlation),rms);
    return {std::abs(correlation),rms};
}
int main() {
    try {
        for(uint32_t rate:{500000u,1000000u,2000000u,4000000u,8000000u}) {
            for(bool flip:{false,true}) {
                auto result=measure(rate,4570000,35000,flip);
                if(result.real()<9900 || result.real()>10100 || result.real()/result.imag()<.999) throw std::runtime_error("DDC tone phase/amplitude discontinuity");
            }
            auto stop=measure(rate,4570000,rate*.70,false);
            if(stop.imag()>10) throw std::runtime_error("DDC alias rejection below 60dB");
        }
        auto hf=measure(2000000,7100000,-65000,false);
        if(hf.real()<9900) throw std::runtime_error("HF lower sideband conversion failed");
        for(uint32_t rate:{500000u,1000000u,2000000u,4000000u,8000000u}) {
            auto result=measure(rate,4570000,-35000,true,0,32000000);
            if(result.real()<9900 || result.real()/result.imag()<.999) throw std::runtime_error("32 MHz ADC DDC phase or rate failed");
        }
        for(uint32_t rate:{250000u,500000u,1000000u,2000000u,4000000u}) {
            auto result=measure(rate,4570000,-35000,true,0,16000000);
            if(result.real()<9900 || result.real()/result.imag()<.999) throw std::runtime_error("16 MHz ADC DDC phase or rate failed");
        }
        bool rejected=false;
        try { Ddc bad(64000000,16000000,4570000,false); } catch(const std::invalid_argument&) { rejected=true; }
        if(!rejected) throw std::runtime_error("Invalid rate accepted");
        std::puts("PASS: DDC counts, phase continuity, sidebands, passband and alias rejection");
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
