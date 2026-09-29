// SPDX-License-Identifier: MIT
// Runs the firmware's own vm_valid_header against a real image built by
// tools/lib/extension.mjs, so the JavaScript writer and the C++ reader are
// checked against each other rather than each against itself.
#include <cassert>
#include <fstream>
#include <vector>
#include <cstring>
#include <cstdio>
#include <utility>
#include "../abi/vm_abi.h"

// The header re-stamped: its CRC recomputed, judged against the file size it describes.
static bool restampedValid(VmImageHeader h){
    h.header_crc=0;h.header_crc=vm_crc32(&h,sizeof h);
    return vm_valid_header(h,sizeof h+vm_image_payload_bytes(h));
}

// Bit 21 is what makes the 128 KiB window at 0x10000 available; 0x18000 is the
// 96 KiB window with or without it, and no other base is a window. Written out
// here rather than taken from the header's own constants, which are what is under test.
static void checkCodeWindows(const VmImageHeader &good){
    static_assert(VM_CODE_BASE==0x18000&&VM_CODE_BASE_128K==0x10000&&VM_CODE_LIMIT==0x30000,"the ABI's code windows");
    static_assert(VM_SERVICE_CODE_128K==1u<<21,"registry bit 21");
    for(uint32_t base:{0x18000u,0x10000u,0u,0x8000u,0x14000u,0x20000u,0x30000u})
    for(bool bit:{false,true})
    for(uint32_t window:{96u*1024,128u*1024})
    for(uint32_t over:{0u,1u}){
        const uint32_t bytes=window+over;
        const bool fits=(base==0x18000u&&bytes<=96u*1024)||(base==0x10000u&&bit&&bytes<=128u*1024);
        const std::pair<uint32_t,bool> entries[]={{base|1,true},{(base+bytes-2)|1,true},
                                                  {(base+bytes+1)|1,false},{(base-2)|1,false}};
        for(auto [entry,inside]:entries){
            auto h=good;h.code_base=base;h.code_bytes=bytes;h.entry=entry;
            h.required_services=bit?good.required_services|VM_SERVICE_CODE_128K:good.required_services&~VM_SERVICE_CODE_128K;
            assert(restampedValid(h)==(fits&&inside));
        }
    }
    // Asking for the bit does not move a 96 KiB image's entry bound down to 0x10000.
    auto h=good;h.required_services|=VM_SERVICE_CODE_128K;h.entry=0x10000|1;
    assert(!restampedValid(h));
    h.entry=good.entry;assert(restampedValid(h));
}
int main(int argc,char **argv){
    assert(argc==2);std::ifstream f(argv[1],std::ios::binary);std::vector<uint8_t>b{std::istreambuf_iterator<char>(f),{}};
    assert(b.size()>64);VmImageHeader good;memcpy(&good,b.data(),64);
    assert(vm_valid_header(good,b.size()));assert(vm_crc32(b.data()+64,b.size()-64)==good.payload_crc);
    // Every single-bit-flipped header must be refused, including in the CRC itself.
    for(unsigned i=0;i<64;i++){auto h=good;((uint8_t *)&h)[i]^=0x80;assert(!vm_valid_header(h,b.size()));}
    for(uint32_t size:{0u,63u,(uint32_t)b.size()-1,(uint32_t)b.size()+1})assert(!vm_valid_header(good,size));
    auto reject=[&](VmImageHeader h){h.header_crc=0;h.header_crc=vm_crc32(&h,64);assert(!vm_valid_header(h,b.size()));};
    auto accept=[&](VmImageHeader h){h.header_crc=0;h.header_crc=vm_crc32(&h,64);assert(vm_valid_header(h,b.size()));};
    // Requiring a service this loader does not provide is well formed.
    for(uint32_t other:{32u,64u,256u,512u,8192u,0x10000u,0x80000000u}){auto h=good;h.required_services|=other;accept(h);}
    assert(good.reserved[0]==VM_PROFILE_LEGACY||good.reserved[0]==VM_PROFILE_RAM2_RO);
    {auto h=good;h.reserved[0]=VM_PROFILE_RESERVED_AUX;reject(h);}
    if(good.reserved[0]==VM_PROFILE_LEGACY){
      auto h=good;h.reserved[1]=32;reject(h);
      h=good;h.required_services|=VM_SERVICE_RAM2_RO;reject(h);
    }else{
      auto h=good;h.reserved[1]=0;reject(h);
      h=good;h.required_services&=~VM_SERVICE_RAM2_RO;reject(h);
      h=good;h.reserved[1]=VM_RAM2_RO_BYTES+1;reject(h);
    }
    auto h=good;h.code_bytes=0xffffffff;reject(h);h=good;h.data_bytes=0xffffffff;reject(h);h=good;h.bss_bytes=VM_RAM_BYTES+1;reject(h);
    h=good;h.entry=VM_CODE_BASE-1;reject(h);h=good;h.entry&=~1;reject(h);h=good;h.entry=VM_CODE_LIMIT|1;reject(h);
    h=good;h.abi++;reject(h);h=good;h.ram_base=0x20000000;reject(h);h=good;h.code_base=0;reject(h);
    h=good;h.reserved[2]=1;reject(h);h=good;h.reserved[3]=1;reject(h);
    assert(good.code_base==VM_CODE_BASE);checkCodeWindows(good);
    b.back()^=1;assert(vm_crc32(b.data()+64,b.size()-64)!=good.payload_crc);
    puts("PASS: MVM1 image CRC, 64 header corruption cases, truncation, overflow, ABI, entry/arena bounds, "
         "both code windows by base, bit 21, size and entry, "
         "profile consistency, and services outside this loader accepted as well formed");
}
