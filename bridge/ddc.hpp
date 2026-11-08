// FFT overlap-save real ADC -> complex baseband. GPL-3.0-or-later.
#pragma once
#include <fftw3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace rx888 {
class Ddc {
    static constexpr int nfft = 8192, hop = nfft / 2;
    static constexpr double pi = 3.14159265358979323846;
    float* input_ = nullptr;
    fftwf_complex *spectrum_ = nullptr, *baseband_ = nullptr;
    fftwf_plan forward_ = nullptr, inverse_ = nullptr;
    std::vector<std::complex<float>> filter_;
    std::array<float, hop> history_{}, fresh_{};
    size_t pending_ = 0;
    uint64_t consumed_ = 0;
    int divisor_, output_fft_, bin_;
    bool conjugate_;
    double residual_, phase_ = 0;
    static double bessel(double x) {
        double sum = 1, term = 1;
        for (int k = 1; k < 40; ++k) { term *= x*x/(4*k*k); sum += term; if (term < sum*1e-15) break; }
        return sum;
    }
    std::complex<float> bin_value(int bin) const {
        bin = (bin % nfft + nfft) % nfft;
        if (bin <= nfft/2) return {spectrum_[bin][0], spectrum_[bin][1]};
        return {spectrum_[nfft-bin][0], -spectrum_[nfft-bin][1]};
    }
public:
    Ddc(uint32_t adc, uint32_t output, double center, bool conjugate) : conjugate_(conjugate) {
        if (!output || adc % output || output > 8000000 || center < 0 || center > adc/2.0)
            throw std::invalid_argument("Invalid DDC rate or center");
        divisor_ = int(adc/output);
        if (divisor_ < 4 || divisor_ > 128 || (divisor_ & (divisor_-1))) throw std::invalid_argument("Invalid DDC divisor");
        output_fft_ = nfft/divisor_;
        bin_ = int(std::llround(center*nfft/adc));
        residual_ = 2*pi*(center-double(bin_)*adc/nfft)/output;
        input_ = fftwf_alloc_real(nfft);
        spectrum_ = fftwf_alloc_complex(nfft/2+1);
        baseband_ = fftwf_alloc_complex(output_fft_);
        if (!input_ || !spectrum_ || !baseband_) { clear(); throw std::bad_alloc(); }
        forward_ = fftwf_plan_dft_r2c_1d(nfft,input_,spectrum_,FFTW_ESTIMATE);
        inverse_ = fftwf_plan_dft_1d(output_fft_,baseband_,baseband_,FFTW_BACKWARD,FFTW_ESTIMATE);
        if (!forward_ || !inverse_) { clear(); throw std::runtime_error("FFTW plan failed"); }
        // Kaiser-windowed lowpass, with transition room inside the advertised IQ rate.
        const int taps = 32*divisor_+1, mid = taps/2;
        const double cutoff = 0.43/divisor_, beta = 8.0;
        std::fill(input_,input_+nfft,0);
        double sum = 0;
        for (int k=0;k<taps;++k) {
            const double x=k-mid, r=double(k-mid)/mid;
            const double sinc = x==0 ? 2*cutoff : std::sin(2*pi*cutoff*x)/(pi*x);
            input_[k] = float(sinc*bessel(beta*std::sqrt(std::max(0.0,1-r*r)))/bessel(beta));
            sum += input_[k];
        }
        for(int k=0;k<taps;++k) input_[k] /= float(sum);
        fftwf_execute(forward_);
        filter_.resize(output_fft_);
        for(int j=0;j<output_fft_;++j) filter_[j]=bin_value(j < output_fft_/2 ? j : j-output_fft_);
    }
    Ddc(const Ddc&) = delete;
    Ddc& operator=(const Ddc&) = delete;
    ~Ddc() { clear(); }
    void clear() noexcept {
        if(forward_) fftwf_destroy_plan(forward_);
        if(inverse_) fftwf_destroy_plan(inverse_);
        fftwf_free(input_); fftwf_free(spectrum_); fftwf_free(baseband_);
        input_=nullptr; spectrum_=baseband_=nullptr; forward_=inverse_=nullptr;
    }
    void reset() { history_.fill(0); pending_=0; consumed_=0; phase_=0; }
    template<class Emit> void process(const int16_t* data,size_t count,Emit emit) {
        while(count) {
            const size_t take=std::min(count,size_t(hop)-pending_);
            for(size_t k=0;k<take;++k) fresh_[pending_+k]=float(data[k]);
            pending_+=take; data+=take; count-=take;
            if(pending_!=hop) continue;
            std::memcpy(input_,history_.data(),hop*sizeof(float));
            std::memcpy(input_+hop,fresh_.data(),hop*sizeof(float));
            history_=fresh_; pending_=0;
            fftwf_execute(forward_);
            for(int j=0;j<output_fft_;++j) {
                const int k=j < output_fft_/2 ? j : j-output_fft_;
                const auto v=bin_value(bin_+k)*filter_[j];
                baseband_[j][0]=v.real(); baseband_[j][1]=v.imag();
            }
            fftwf_execute(inverse_);
            // The FFT-bin shift has a phase relative to the start of each window.
            const int start_mod=int((consumed_+nfft-hop)%nfft);
            const double coarse=-2*pi*bin_*start_mod/nfft;
            std::complex<double> osc=std::polar(1.0,coarse-phase_);
            const auto step=std::polar(1.0,-residual_);
            for(int j=hop/divisor_;j<output_fft_;++j) {
                auto value=std::complex<double>(baseband_[j][0],baseband_[j][1])*osc*(2.0/nfft);
                if(conjugate_) value=std::conj(value);
                emit(std::complex<float>(value));
                osc*=step;
            }
            phase_=std::remainder(phase_+residual_*(hop/divisor_),2*pi);
            consumed_+=hop;
        }
    }
};
}
