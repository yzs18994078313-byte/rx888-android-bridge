// Receive on the phone itself so wireless ADB is not in the IQ data path.
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <stdexcept>
static void exact(int fd,void* data,size_t n) {
    auto* p=static_cast<unsigned char*>(data);
    while(n) { const auto k=recv(fd,p,n,0); if(k<=0) throw std::runtime_error("Socket read failed"); p+=k;n-=k; }
}
static void command(int fd,uint32_t cmd,const void* data,uint32_t n) {
    std::array<uint32_t,2> h{cmd,n};
    std::vector<unsigned char> bytes(8+n);
    std::memcpy(bytes.data(),h.data(),8);std::memcpy(bytes.data()+8,data,n);
    size_t offset=0;
    while(offset<bytes.size()) { auto k=send(fd,bytes.data()+offset,bytes.size()-offset,MSG_NOSIGNAL);if(k<=0)throw std::runtime_error("Socket send failed");offset+=k; }
}
static void setting(int fd,uint32_t id,uint32_t value) { std::array<uint32_t,2> data{id,value};command(fd,2,data.data(),8); }
int main(int argc,char** argv) {
    setvbuf(stdout,nullptr,_IOLBF,0);
    const unsigned decimation=argc>1?unsigned(std::atoi(argv[1])):2;
    const unsigned seconds=argc>2?unsigned(std::atoi(argv[2])):15;
    int fd=-1;
    try {
        fd=socket(AF_INET,SOCK_STREAM,0);timeval timeout{5,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(5557);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof address)) throw std::runtime_error("Connect failed");
        uint32_t version=(2u<<24)|1700;command(fd,0,&version,4);
        setting(fd,0,1);setting(fd,100,2);setting(fd,102,decimation);setting(fd,1,1);
        using Clock=std::chrono::steady_clock;
        auto begin=Clock::now(),next=begin;
        uint32_t base_rate=4000000;bool first=true;uint64_t samples=0;int peak=0;unsigned probes=0,retunes=0;double sum=0;
        std::vector<unsigned char> body(1024*1024);
        while(true) {
            std::array<uint32_t,5> header{};exact(fd,header.data(),20);
            if(header[4]>body.size()) throw std::runtime_error("Oversized packet");
            exact(fd,body.data(),header[4]);
            if(header[1]==0 && header[4]==48) std::memcpy(&base_rate,body.data()+8,4);
            if(header[1]!=101)continue;
            if(first) { begin=Clock::now();next=begin+std::chrono::seconds(5);first=false; }
            samples+=header[4]/4;
            if(probes<65536) for(unsigned i=0;i+1<header[4];i+=2) {
                const auto value=int16_t(uint16_t(body[i])|uint16_t(body[i+1])<<8);
                peak=std::max(peak,std::abs(int(value)));sum+=double(value)*value;++probes;
            }
            const auto now=Clock::now();
            if(argc>3 && retunes<2 && now-begin>=std::chrono::seconds(seconds*(retunes+1)/3)) {
                const uint32_t frequency=retunes?137900000:138000000;
                setting(fd,101,frequency);setting(fd,2,retunes?30:25);++retunes;
                std::printf("RETUNE: frequency=%u gain=%u\n",frequency,retunes==1?25:30);
            }
            if(now>=next) { std::printf("LOCAL_RX: rate=%u samples=%llu elapsed=%.2f\n",base_rate>>decimation,(unsigned long long)samples,std::chrono::duration<double>(now-begin).count());next+=std::chrono::seconds(5); }
            if(now-begin>=std::chrono::seconds(seconds)) break;
        }
        const auto elapsed=std::chrono::duration<double>(Clock::now()-begin).count();
        setting(fd,1,0);close(fd);fd=-1;
        const double delivered=samples/elapsed,expected=base_rate>>decimation;
        std::printf("RESULT: requested=%.0f delivered=%.0f duration=%.3f samples=%llu peak=%d rms=%.2f\n",expected,delivered,elapsed,(unsigned long long)samples,peak,std::sqrt(sum/probes));
        if(!peak || (argc<=3 && (delivered<expected*.95 || delivered>expected*1.10))) throw std::runtime_error("Unexpected IQ rate or zero data");
        std::puts("PASS: phone-local real IQ stream");
    } catch(const std::exception& e) { if(fd>=0)close(fd);std::fprintf(stderr,"FAIL: %s\n",e.what());return 1; }
}
