#include "usb.hpp"
int main(int argc,char** argv) {
    setvbuf(stdout,nullptr,_IOLBF,0);
    try {
        if(argc>2 && std::string(argv[2])=="--host-reset") {
            libusb_context* context=nullptr;
            if(libusb_init(&context)<0) return 1;
            auto* handle=libusb_open_device_with_vid_pid(context,0x04b4,0x00f1);
            if(!handle) { libusb_exit(context);throw std::runtime_error("No running RX888 for reset"); }
            auto* device=libusb_get_device(handle);uint8_t ports[8];
            const int depth=libusb_get_port_numbers(device,ports,8);
            std::string node=std::to_string(libusb_get_bus_number(device))+"-";
            for(int i=0;i<depth;++i) { if(i)node+='.';node+=std::to_string(ports[i]); }
            std::string product;std::getline(std::ifstream("/sys/bus/usb/devices/"+node+"/product"),product);
            if(product.find("RX888")==std::string::npos) {libusb_close(handle);libusb_exit(context);throw std::runtime_error("Cached USB product is not RX888");}
            const int result=libusb_reset_device(handle);
            std::printf("HOST RESET: %d (%s)\n",result,libusb_error_name(result));
            libusb_close(handle);libusb_exit(context);return result<0?1:0;
        }
        rx888::Usb usb;
        usb.open(argc>1?argv[1]:"SDDC_FX3.img");
        if(argc>2 && std::string(argv[2])=="--reset") {
            usb.command(RESETFX3,uint8_t(0));
            std::puts("RESET: RX888 returning to FX3 bootloader");
            return 0;
        }
        usb.command(STARTADC,uint32_t(argc>3?std::stoul(argv[3]):32000000));
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        usb.command(GPIOFX3,uint32_t(VHF_EN));
        usb.argument(DAT31_ATT,63);usb.argument(AD8340_VGA,0x83);
        usb.command(TUNERINIT,uint32_t(16000000));
        usb.command(TUNERTUNE,uint64_t(137900000));
        usb.argument(R82XX_ATTENUATOR,20);usb.argument(R82XX_VGA,8);
        if(argc>2 && std::string(argv[2])=="--async") {
            std::atomic<uint64_t> samples{0};
            usb.start([&](const int16_t*,size_t count){samples.fetch_add(count);});
            for(int k=0;k<6;++k) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                std::printf("RAW_ONLY: elapsed=%d samples=%llu bytes=%llu errors=%llu timeouts=%llu failed=%d\n",5*(k+1),(unsigned long long)samples.load(),(unsigned long long)usb.bytes.load(),(unsigned long long)usb.errors.load(),(unsigned long long)usb.timeouts.load(),usb.failed.load());
                if(usb.failed.load()) break;
            }
            usb.stop();return 0;
        }
        usb.dump_debug();
        usb.command(STARTFX3,uint8_t(0));
        std::vector<uint8_t> bytes(131072);
        for(int k=0;k<3;++k) {
            int actual=0;
            const int result=usb.read_bulk(bytes.data(),int(bytes.size()),actual);
            std::printf("BULK: result=%d (%s) bytes=%d\n",result,libusb_error_name(result),actual);
            usb.dump_debug();
        }
        usb.command(STOPFX3,uint8_t(0));
    } catch(const std::exception& e) { std::fprintf(stderr,"FATAL: %s\n",e.what());return 1; }
}
