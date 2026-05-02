/* ================================================================
 *  Systrix OS — kernel/pci.c
 *  PCI/PCIe enumeration, BAR sizing, capability walk, ECAM support
 * ================================================================ */
#include "../include/kernel.h"

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

/* ── Legacy I/O config ───────────────────────────────────────── */
static u32 pci_io_r32(u8 bus,u8 sl,u8 fn,u8 off){
    outl(PCI_ADDR,0x80000000u|((u32)bus<<16)|((u32)sl<<11)|((u32)fn<<8)|(off&0xFC));
    return inl(PCI_DATA);
}
static void pci_io_w32(u8 bus,u8 sl,u8 fn,u8 off,u32 v){
    outl(PCI_ADDR,0x80000000u|((u32)bus<<16)|((u32)sl<<11)|((u32)fn<<8)|(off&0xFC));
    outl(PCI_DATA,v);
}

/* ── ECAM (PCIe) ─────────────────────────────────────────────── */
static u64 g_ecam=0;
static u8  g_ecam_bus0=0;
void pci_set_ecam(u64 base,u8 start){ g_ecam=base; g_ecam_bus0=start; }

static volatile u32 *ecam_ptr(u8 bus,u8 sl,u8 fn,u16 off){
    if(!g_ecam) return 0;
    u64 va=g_ecam+((u64)(bus-g_ecam_bus0)<<20)+((u64)sl<<15)+((u64)fn<<12)+(off&0xFFC);
    return (volatile u32*)va;
}

/* ── Public accessors ────────────────────────────────────────── */
u32 pci_read32(u8 bus,u8 sl,u8 fn,u16 off){
    volatile u32 *p=ecam_ptr(bus,sl,fn,off);
    return p?*p:pci_io_r32(bus,sl,fn,(u8)off);
}
void pci_write32(u8 bus,u8 sl,u8 fn,u16 off,u32 v){
    volatile u32 *p=ecam_ptr(bus,sl,fn,off);
    if(p){*p=v;return;} pci_io_w32(bus,sl,fn,(u8)off,v);
}
u16 pci_read16(u8 b,u8 s,u8 f,u16 o){return(u16)(pci_read32(b,s,f,o&~2u)>>((o&2)*8));}
u8  pci_read8 (u8 b,u8 s,u8 f,u16 o){return(u8) (pci_read32(b,s,f,o&~3u)>>((o&3)*8));}
void pci_write16(u8 b,u8 s,u8 f,u16 o,u16 v){
    u32 d=pci_read32(b,s,f,o&~2u);int sh=(o&2)*8;
    d=(d&~(0xFFFFu<<sh))|((u32)v<<sh);pci_write32(b,s,f,o&~2u,d);
}

/* ── Device table ────────────────────────────────────────────── */
#define PCI_MAX 128
typedef struct {
    u8  bus,slot,fn;
    u16 vendor,device;
    u8  class_code,subclass,prog_if,revision,header_type;
    u32 bar[6]; u64 bar64[6]; u32 bar_size[6]; u64 bar_size64[6];
    u8  bar_is64[6],bar_is_io[6];
    u8  irq_line,irq_pin,is_bridge,secondary_bus;
    u16 subsys_vendor,subsys_device;
    u8  cap_msi,cap_msix,cap_pm;
} PciDev;

static PciDev g_pdev[PCI_MAX];
static int    g_pcnt=0;

/* ── BAR sizing ──────────────────────────────────────────────── */
static void size_bars(PciDev *d){
    u16 cmd=pci_read16(d->bus,d->slot,d->fn,0x04);
    pci_write16(d->bus,d->slot,d->fn,0x04,(u16)(cmd&~3u));
    int n=(d->header_type&0x7F)?2:6;
    for(int i=0;i<n;){
        u32 bar=pci_read32(d->bus,d->slot,d->fn,(u16)(0x10+i*4));
        d->bar[i]=bar; d->bar_is_io[i]=bar&1;
        d->bar_is64[i]=(!d->bar_is_io[i])&&((bar>>1)&3)==2;
        pci_write32(d->bus,d->slot,d->fn,(u16)(0x10+i*4),0xFFFFFFFF);
        u32 sz=pci_read32(d->bus,d->slot,d->fn,(u16)(0x10+i*4));
        pci_write32(d->bus,d->slot,d->fn,(u16)(0x10+i*4),bar);
        if(d->bar_is64[i]&&i+1<n){
            u32 bhi=pci_read32(d->bus,d->slot,d->fn,(u16)(0x10+(i+1)*4));
            pci_write32(d->bus,d->slot,d->fn,(u16)(0x10+(i+1)*4),0xFFFFFFFF);
            u32 shi=pci_read32(d->bus,d->slot,d->fn,(u16)(0x10+(i+1)*4));
            pci_write32(d->bus,d->slot,d->fn,(u16)(0x10+(i+1)*4),bhi);
            d->bar64[i]=((u64)bhi<<32)|(bar&~0xFu);
            d->bar_size64[i]=~(((u64)shi<<32)|(sz&~0xFu))+1;
            d->bar[i+1]=bhi; d->bar64[i+1]=0; d->bar_size64[i+1]=0; i+=2;
        } else {
            if(d->bar_is_io[i]){d->bar_size[i]=~(sz|3u)+1;d->bar64[i]=bar&0xFFFCu;}
            else {d->bar_size[i]=~(sz|0xFu)+1;d->bar64[i]=bar&~0xFu;}
            i++;
        }
    }
    pci_write16(d->bus,d->slot,d->fn,0x04,cmd);
}

/* ── Capability walk ─────────────────────────────────────────── */
static void scan_caps(PciDev *d){
    if(!(pci_read16(d->bus,d->slot,d->fn,0x06)&0x10)) return;
    u8 ptr=pci_read8(d->bus,d->slot,d->fn,0x34)&0xFC;
    for(int lim=48;ptr&&lim--;){
        u8 id=pci_read8(d->bus,d->slot,d->fn,ptr);
        if(id==0x05)d->cap_msi=ptr;
        if(id==0x11)d->cap_msix=ptr;
        if(id==0x01)d->cap_pm=ptr;
        ptr=pci_read8(d->bus,d->slot,d->fn,(u16)(ptr+1))&0xFC;
    }
}

/* ── Recursive bus scan ──────────────────────────────────────── */
static void scan_bus(u8 bus);
static void scan_fn(u8 bus,u8 sl,u8 fn){
    u32 id=pci_read32(bus,sl,fn,0);
    if(id==0xFFFFFFFF||!id||g_pcnt>=PCI_MAX) return;
    PciDev *d=&g_pdev[g_pcnt++];
    d->bus=bus;d->slot=sl;d->fn=fn;
    d->vendor=(u16)id;d->device=(u16)(id>>16);
    u32 cr=pci_read32(bus,sl,fn,8);
    d->revision=(u8)cr;d->prog_if=(u8)(cr>>8);d->subclass=(u8)(cr>>16);d->class_code=(u8)(cr>>24);
    d->header_type=(u8)(pci_read32(bus,sl,fn,0x0C)>>16);
    d->irq_line=pci_read8(bus,sl,fn,0x3C);d->irq_pin=pci_read8(bus,sl,fn,0x3D);
    d->is_bridge=(d->header_type&0x7F)==1;
    if(d->is_bridge) d->secondary_bus=pci_read8(bus,sl,fn,0x19);
    else{d->subsys_vendor=pci_read16(bus,sl,fn,0x2C);d->subsys_device=pci_read16(bus,sl,fn,0x2E);}
    size_bars(d); scan_caps(d);
    if(d->is_bridge&&d->secondary_bus) scan_bus(d->secondary_bus);
}
static void scan_bus(u8 bus){
    for(u8 sl=0;sl<32;sl++){
        if(pci_read32(bus,sl,0,0)==0xFFFFFFFF) continue;
        scan_fn(bus,sl,0);
        if(pci_read8(bus,sl,0,0x0E)&0x80)
            for(u8 fn=1;fn<8;fn++) if(pci_read16(bus,sl,fn,0)!=0xFFFF) scan_fn(bus,sl,fn);
    }
}

/* ── Public API ──────────────────────────────────────────────── */
void pci_scan_all(void){
    g_pcnt=0; scan_bus(0);
    kprintf("[PCI] "); print_hex_byte((u8)g_pcnt); kprintf(" devices\r\n");
}
void *pci_find_device(u16 v,u16 d){for(int i=0;i<g_pcnt;i++)if(g_pdev[i].vendor==v&&g_pdev[i].device==d)return&g_pdev[i];return 0;}
void *pci_find_class(u8 c,u8 s){for(int i=0;i<g_pcnt;i++)if(g_pdev[i].class_code==c&&g_pdev[i].subclass==s)return&g_pdev[i];return 0;}
void *pci_find_class_progif(u8 c,u8 s,u8 p){for(int i=0;i<g_pcnt;i++)if(g_pdev[i].class_code==c&&g_pdev[i].subclass==s&&g_pdev[i].prog_if==p)return&g_pdev[i];return 0;}
void pci_enable_device(void *dp){PciDev*d=dp;pci_write16(d->bus,d->slot,d->fn,0x04,(u16)(pci_read16(d->bus,d->slot,d->fn,0x04)|7));}
void pci_enable_bus_master(void *dp){PciDev*d=dp;pci_write16(d->bus,d->slot,d->fn,0x04,(u16)(pci_read16(d->bus,d->slot,d->fn,0x04)|4));}
u64  pci_bar_base(void *dp,int b){PciDev*d=dp;if(b<0||b>=6||((PciDev*)dp)->bar_is_io[b])return 0;return d->bar64[b];}
u64  pci_bar_size(void *dp,int b){PciDev*d=dp;if(b<0||b>=6)return 0;return d->bar_is64[b]?d->bar_size64[b]:d->bar_size[b];}
u16  pci_bar_io(void *dp,int b){PciDev*d=dp;if(b<0||b>=6||!d->bar_is_io[b])return 0;return(u16)(d->bar[b]&0xFFFC);}
u8   pci_irq(void *dp){return((PciDev*)dp)->irq_line;}
void pci_power_on(void *dp){
    PciDev*d=dp;if(!d->cap_pm)return;
    u16 pmcs=pci_read16(d->bus,d->slot,d->fn,(u16)(d->cap_pm+4));
    if((pmcs&3)==3){pci_write16(d->bus,d->slot,d->fn,(u16)(d->cap_pm+4),(u16)(pmcs&~3u));for(volatile int i=0;i<500000;i++);}
}
void pci_list_devices(void){
    kprintf("BUS SL FN VEN:DEV  CL:SB PI IRQ\r\n");
    for(int i=0;i<g_pcnt;i++){PciDev*d=&g_pdev[i];
        print_hex_byte(d->bus);kprintf(" ");print_hex_byte(d->slot);kprintf(" ");
        print_hex_byte(d->fn);kprintf(" ");
        print_hex_byte((u8)(d->vendor>>8));print_hex_byte((u8)d->vendor);kprintf(":");
        print_hex_byte((u8)(d->device>>8));print_hex_byte((u8)d->device);kprintf("  ");
        print_hex_byte(d->class_code);kprintf(":");print_hex_byte(d->subclass);kprintf(" ");
        print_hex_byte(d->prog_if);kprintf(" ");print_hex_byte(d->irq_line);kprintf("\r\n");
    }
}
