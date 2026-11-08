// RX888 MKII vendor protocol follows ik1xpv/ExtIO_sddc. GPL-3.0-or-later.
#pragma once
#include <libusb.h>
#include "../third_party/Interface.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <cstdlib>
#include <limits.h>
#include <sys/resource.h>
#include <pthread.h>
#include <unistd.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace rx888 {
// Apply NO_LPM only while the post-firmware device is being enumerated. The
// per-device quirk persists on that attachment; the kernel parameter is restored.
class EnumerationLinkPolicy {
    static constexpr const char* path="/sys/module/usbcore/parameters/quirks";
    std::string previous_, installed_;
    bool changed_=false;
public:
    EnumerationLinkPolicy() {
        std::ifstream in(path); if(!in) return;
        std::getline(in,previous_);
        installed_=previous_;
        const std::string key="04b4:00f1:";
        const auto position=installed_.find(key);
        if(position!=std::string::npos) {
            const auto end=installed_.find(',',position);
            const auto flags=installed_.substr(position+key.size(),end==std::string::npos?end:end-position-key.size());
            if(flags.find('k')!=std::string::npos) return;
            installed_.insert(end==std::string::npos?installed_.size():end,"k");
        } else {
            if(!installed_.empty()) installed_+=',';
            installed_+=key+"k";
        }
        if(installed_.size()>127) return;
        std::ofstream output(path); output<<installed_<<'\n'; output.close();
        changed_=bool(output);
        if(changed_) std::puts("USB link PM: RX888 NO_LPM enumeration workaround enabled");
    }
    ~EnumerationLinkPolicy() {
        if(!changed_) return;
        std::string current; std::ifstream(path)>>current;
        if(current==installed_) { std::ofstream output(path); output<<previous_<<'\n'; }
        std::puts("USB link PM: original kernel parameter restored");
    }
};
class Usb {
    libusb_context* ctx_=nullptr;
    libusb_device_handle* handle_=nullptr;
    int interface_=-1;
    uint8_t endpoint_=0;
    std::vector<libusb_transfer*> transfers_;
    std::vector<std::vector<uint8_t>> buffers_;
    std::atomic<bool> running_{false};
    std::thread events_;
    int active_=0; // Owned by event thread, or main while it is stopped.
    std::function<void(const int16_t*,size_t)> receive_;
    std::vector<uint8_t> ports_;
    uint8_t bus_=0;
    std::string controller_;
    std::string power_path_, previous_power_;
    void keep_awake() {
        if(ports_.empty()) return;
        std::string node=std::to_string(bus_)+"-";
        for(size_t i=0;i<ports_.size();++i) { if(i) node+='.'; node+=std::to_string(ports_[i]); }
        const std::string path="/sys/bus/usb/devices/"+node+"/power/control";
        std::string previous; std::ifstream(path)>>previous;
        if(previous.empty()) return;
        if(previous=="on") { std::puts("USB runtime PM: already on"); return; }
        std::ofstream output(path); output<<"on"; output.close();
        if(!output) { std::puts("WARNING: cannot disable RX888 USB autosuspend"); return; }
        power_path_=path; previous_power_=previous;
        std::printf("USB runtime PM: %s -> on (held until backend exits)\n",previous.c_str());
    }
    void restore_power() noexcept {
        if(power_path_.empty()) return;
        std::string current; std::ifstream(power_path_)>>current;
        if(current=="on") { std::ofstream output(power_path_); output<<previous_power_; }
        power_path_.clear();
    }
    static std::string controller(libusb_device* device) {
        char link[80], resolved[PATH_MAX];
        std::snprintf(link,sizeof link,"/sys/bus/usb/devices/usb%u",libusb_get_bus_number(device));
        if(!realpath(link,resolved)) return {};
        const std::string path=resolved;
        return path.substr(0,path.find_last_of('/'));
    }
    static uint32_t word(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
    libusb_device_handle* locate(uint16_t pid,bool same_port) {
        libusb_device** list=nullptr;
        const auto n=libusb_get_device_list(ctx_,&list);
        if(n<0) throw std::runtime_error("Cannot enumerate USB devices");
        libusb_device* selected=nullptr; int matches=0;
        for(ssize_t i=0;i<n;++i) {
            libusb_device_descriptor d{};
            if(libusb_get_device_descriptor(list[i],&d)!=0 || d.idVendor!=0x04b4 || d.idProduct!=pid) continue;
            uint8_t path[8]; int depth=libusb_get_port_numbers(list[i],path,sizeof path);
            if(depth<0) continue;
            if(same_port) {
                // FX3 boots at USB 2 speed, then moves to the companion USB 3 bus.
                // Match the physical controller and port chain, not the bus number.
                if(std::vector<uint8_t>(path,path+depth)!=ports_) continue;
                const auto candidate_controller=controller(list[i]);
                if(!controller_.empty() && !candidate_controller.empty() && candidate_controller!=controller_) continue;
            }
            selected=list[i]; ++matches;
        }
        libusb_device_handle* h=nullptr;
        int error=0;
        if(matches==1) {
            uint8_t path[8]; const int depth=libusb_get_port_numbers(selected,path,sizeof path);
            const auto previous_bus=bus_;
            ports_.assign(path,path+depth); bus_=libusb_get_bus_number(selected);
            controller_=controller(selected);
            error=libusb_open(selected,&h);
            if(error==0) std::printf("USB found: 04b4:%04x bus=%u address=%u%s\n",pid,bus_,libusb_get_device_address(selected),same_port && previous_bus!=bus_?" (USB 2 -> USB 3 companion bus)":"");
        }
        libusb_free_device_list(list,1);
        if(matches>1) throw std::runtime_error("Multiple matching FX3 devices; connect only the RX888 MKII");
        if(error<0) throw std::runtime_error(std::string("USB open: ")+libusb_error_name(error)+" (root required)");
        return h;
    }
    void upload(const std::string& filename) {
        std::ifstream f(filename,std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),{});
        if(bytes.size()<16 || bytes.size()>512*1024 || bytes[0]!='C' || bytes[1]!='Y' || bytes[3]!=0xb0)
            throw std::runtime_error("Invalid FX3 firmware image");
        size_t offset=4; uint32_t checksum=0, entry=0;
        struct Section { size_t pos,size; uint32_t address; };
        std::vector<Section> sections;
        while(true) {
            if(offset+8>bytes.size()) throw std::runtime_error("Truncated FX3 firmware");
            const uint32_t words=word(bytes.data()+offset), address=word(bytes.data()+offset+4); offset+=8;
            if(!words) {
                if(offset+4!=bytes.size() || word(bytes.data()+offset)!=checksum) throw std::runtime_error("FX3 checksum mismatch");
                entry=address; break;
            }
            const uint64_t length=uint64_t(words)*4;
            if(length>bytes.size()-offset || uint64_t(address)+length>UINT32_MAX) throw std::runtime_error("Invalid FX3 section");
            sections.push_back({offset,size_t(length),address});
            for(size_t p=offset;p<offset+length;p+=4) checksum+=word(bytes.data()+p);
            offset+=size_t(length);
        }
        std::puts("Loading standard SDDC FX3 firmware into RAM...");
        for(const auto& s:sections) for(size_t p=0;p<s.size;) {
            const uint16_t count=uint16_t(std::min<size_t>(2048,s.size-p));
            const uint32_t address=s.address+uint32_t(p);
            const int r=libusb_control_transfer(handle_,0x40,0xa0,address&65535,address>>16,bytes.data()+s.pos+p,count,2000);
            if(r!=count) throw std::runtime_error("FX3 firmware RAM upload failed");
            p+=count;
        }
        const int r=libusb_control_transfer(handle_,0x40,0xa0,entry&65535,entry>>16,nullptr,0,2000);
        // A reset may disconnect before the final status packet is returned.
        if(r<0 && r!=LIBUSB_ERROR_NO_DEVICE && r!=LIBUSB_ERROR_IO) throw std::runtime_error("FX3 start failed");
    }
    static void LIBUSB_CALL callback(libusb_transfer* t) {
        auto& u=*static_cast<Usb*>(t->user_data);
        bool retry=false;
        if(u.running_.load(std::memory_order_acquire)) {
            if(t->status==LIBUSB_TRANSFER_COMPLETED || t->status==LIBUSB_TRANSFER_TIMED_OUT) {
                if(t->status==LIBUSB_TRANSFER_TIMED_OUT) u.timeouts.fetch_add(1);
                if(t->actual_length & 1) { std::fprintf(stderr,"USB ERROR: odd transfer length=%d\n",t->actual_length); u.errors.fetch_add(1); u.failed.store(true); }
                else {
                    if(t->actual_length) { u.bytes.fetch_add(t->actual_length); u.receive_(reinterpret_cast<const int16_t*>(t->buffer),size_t(t->actual_length)/2); }
                    retry=true;
                }
            } else if(t->status!=LIBUSB_TRANSFER_CANCELLED) { std::fprintf(stderr,"USB ERROR: transfer status=%d actual_length=%d\n",int(t->status),t->actual_length); u.errors.fetch_add(1); u.failed.store(true); }
        }
        if(retry && !u.failed.load()) {
            const int result=libusb_submit_transfer(t);
            if(result==0) return;
            std::fprintf(stderr,"USB ERROR: resubmit=%d (%s)\n",result,libusb_error_name(result));
            u.errors.fetch_add(1); u.failed.store(true);
        }
        --u.active_;
    }
    void free_transfers() {
        for(auto* t:transfers_) libusb_free_transfer(t);
        transfers_.clear(); buffers_.clear();
    }
public:
    void dump_debug() {
        uint8_t data[100]{};
        const int n=libusb_control_transfer(handle_,0xc0,READINFODEBUG,0,0,data,sizeof data,2000);
        if(n>0) std::printf("FX3 DEBUG: %.*s\n",n,reinterpret_cast<char*>(data));
        else std::printf("FX3 DEBUG: result=%d\n",n);
    }
    int read_bulk(uint8_t* data,int length,int& actual) {
        return libusb_bulk_transfer(handle_,endpoint_,data,length,&actual,3000);
    }
    std::atomic<uint64_t> errors{0},timeouts{0},bytes{0};
    std::atomic<bool> failed{false};
    uint16_t firmware=0;
    uint32_t serial=0;
    Usb()=default;
    Usb(const Usb&)=delete;
    ~Usb() { stop(); if(handle_) { if(interface_>=0) libusb_release_interface(handle_,interface_); libusb_close(handle_); } restore_power(); if(ctx_) libusb_exit(ctx_); }
    void open(const std::string& firmware_path) {
        EnumerationLinkPolicy link_policy;
        if(libusb_init(&ctx_)<0) throw std::runtime_error("libusb initialization failed");
        handle_=locate(0x00f1,false);
        if(!handle_) {
            handle_=locate(0x00f3,false);
            if(!handle_) throw std::runtime_error("RX888 MKII not found; check USB 3 connection and power");
            upload(firmware_path); libusb_close(handle_); handle_=nullptr;
            std::puts("Waiting for RX888 USB 3 re-enumeration (fresh device discovery)...");
            for(int k=0;k<60 && !handle_;++k) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                // Android may not deliver the new bus's uevent to libusb's monitor.
                // A new context scans sysfs instead of trusting the old hotplug cache.
                libusb_exit(ctx_); ctx_=nullptr;
                if(libusb_init(&ctx_)<0) throw std::runtime_error("USB discovery refresh failed");
                handle_=locate(0x00f1,true);
            }
            if(!handle_) throw std::runtime_error("RX888 did not re-enumerate after firmware upload");
        }
        auto* device=libusb_get_device(handle_);
        if(libusb_get_device_speed(device)<LIBUSB_SPEED_SUPER) throw std::runtime_error("RX888 requires a USB 3 SuperSpeed connection");
        keep_awake();
        libusb_device_descriptor desc{}; libusb_get_device_descriptor(device,&desc);
        unsigned char product[256]{};
        if(desc.iProduct) libusb_get_string_descriptor_ascii(handle_,desc.iProduct,product,sizeof(product)-1);
        std::string name(reinterpret_cast<char*>(product));
        if(name.find("RX888")==std::string::npos && name.find("RX-888")==std::string::npos)
            throw std::runtime_error("USB product is not identified as RX888; refusing unrelated FX3 device");
        if(desc.iSerialNumber) {
            unsigned char sn[256]{}; const int n=libusb_get_string_descriptor_ascii(handle_,desc.iSerialNumber,sn,255);
            for(int k=0;k<n;++k) serial=(serial^sn[k])*16777619u;
        }
        libusb_config_descriptor* config=nullptr;
        if(libusb_get_active_config_descriptor(device,&config)!=0) throw std::runtime_error("Missing USB configuration");
        for(int i=0;i<config->bNumInterfaces;++i) for(int a=0;a<config->interface[i].num_altsetting;++a) {
            const auto& setting=config->interface[i].altsetting[a];
            if(setting.bAlternateSetting!=0) continue;
            for(int e=0;e<setting.bNumEndpoints;++e) {
                const auto& ep=setting.endpoint[e];
                if((ep.bmAttributes&3)==LIBUSB_TRANSFER_TYPE_BULK && (ep.bEndpointAddress&0x80)) { interface_=setting.bInterfaceNumber; endpoint_=ep.bEndpointAddress; }
            }
        }
        libusb_free_config_descriptor(config);
        if(interface_<0 || libusb_claim_interface(handle_,interface_)<0) throw std::runtime_error("Cannot claim RX888 bulk interface");
        uint8_t info[4]{};
        transfer(TESTFX3,0,0,info,4,true);
        if(info[0]!=RX888r2) throw std::runtime_error("SDDC hardware model is not RX888 MKII");
        firmware=uint16_t(info[1])*256+info[2];
        command(STOPFX3,uint32_t(0));
        std::printf("USB 3: %s, firmware %u.%u, endpoint 0x%02x\n",name.c_str(),info[1],info[2],endpoint_);
    }
    void transfer(uint8_t request,uint16_t value,uint16_t index,uint8_t* data,uint16_t length,bool read=false) {
        const int r=libusb_control_transfer(handle_,read?0xc0:0x40,request,value,index,data,length,2000);
        if(r!=length) throw std::runtime_error("RX888 vendor command "+std::to_string(request)+": "+(r<0?libusb_error_name(r):"short transfer"));
    }
    template<class T> void command(FX3Command command,T value) { transfer(uint8_t(command),0,0,reinterpret_cast<uint8_t*>(&value),sizeof value); }
    void argument(uint16_t index,uint16_t value) { uint8_t data=0; transfer(SETARGFX3,value,index,&data,1); }
    void start(std::function<void(const int16_t*,size_t)> receive) {
        stop(); receive_=std::move(receive); failed.store(false); buffers_.resize(128); running_.store(true);
        std::puts("USB receive pool: 128 x 16384 bytes (2 MiB)");
        try {
            for(size_t index=0;index<buffers_.size();++index) {
                buffers_[index].resize(16384);
                auto* data=buffers_[index].data();
                auto* t=libusb_alloc_transfer(0); if(!t) throw std::bad_alloc();
                transfers_.push_back(t);
                libusb_fill_bulk_transfer(t,handle_,endpoint_,data,16384,callback,this,0);
                const int result=libusb_submit_transfer(t);
                if(result<0) throw std::runtime_error(std::string("USB submit: ")+libusb_error_name(result));
                ++active_;
            }
            std::puts("USB buffers: ordinary application memory");
            events_=std::thread([this] {
                pthread_setname_np(pthread_self(),"rx888-usb");
                if(geteuid()==0) setpriority(PRIO_PROCESS,0,-8);
                while(running_.load() && !failed.load()) {
                    timeval timeout{0,50000};
                    const int r=libusb_handle_events_timeout_completed(ctx_,&timeout,nullptr);
                    if(r<0 && r!=LIBUSB_ERROR_INTERRUPTED) { std::fprintf(stderr,"USB ERROR: event loop=%d (%s)\n",r,libusb_error_name(r)); errors.fetch_add(1); failed.store(true); }
                }
                running_.store(false);
                for(auto* t:transfers_) libusb_cancel_transfer(t);
                while(active_>0) { timeval timeout{0,50000}; libusb_handle_events_timeout_completed(ctx_,&timeout,nullptr); }
            });
            command(STARTFX3,uint32_t(0));
        } catch(...) { stop(); throw; }
    }
    void stop() noexcept {
        running_.store(false);
        if(events_.joinable()) events_.join();
        else if(active_>0) {
            for(auto* t:transfers_) libusb_cancel_transfer(t);
            while(active_>0) { timeval timeout{0,50000}; libusb_handle_events_timeout_completed(ctx_,&timeout,nullptr); }
        }
        if(!transfers_.empty()) { try { command(STOPFX3,uint32_t(0)); } catch(...) {} }
        free_transfers();
        failed.store(false);
    }
};
}
