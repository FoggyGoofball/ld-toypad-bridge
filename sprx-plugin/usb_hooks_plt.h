/*
 * usb_hooks_plt.h — PLT Pattern Scanner for game .text section
 *
 * Searches for the standard PowerPC PLT stub pattern:
 *   lis   r12, hi(import)   0x3D80xxxx
 *   lwz   r12, lo(import)(r12)  0x818Cxxxx
 *   mtctr r12                0x7D8903A6
 *   bctr                     0x4E800420
 *
 * The GOT address is extracted from the lis/lwz operands:
 *   GOT_addr = (hi << 16) | lo
 */

#ifndef USB_HOOKS_PLT_H
#define USB_HOOKS_PLT_H

#include <stdint.h>

/* 4-instruction PLT stub structure */
typedef struct {
    uint32_t lis_inst;   /* 0x3D80xxxx */
    uint32_t lwz_inst;   /* 0x818Cxxxx */
    uint32_t mtctr_inst; /* 0x7D8903A6 */
    uint32_t bctr_inst;  /* 0x4E800420 */
} plt_stub_t;

/* PLT stub found in game memory */
typedef struct {
    uint32_t addr;       /* Address of PLT stub in game .text */
    uint32_t got_addr;   /* GOT slot address extracted from lis/lwz */
    uint32_t got_val;    /* Current value of GOT slot (resolved function addr) */
} plt_stub_match_t;

/**
 * scan_plt_stubs - Search game memory for PLT stubs matching known patterns
 *
 * @param regions  Scan regions array
 * @param num_regions  Number of regions
 * @param our_resolved_addrs  The 4 resolved cellUsbd function addresses from our OPDs
 * @param matches  [out] Array of plt_stub_match_t to fill
 * @param max_matches  Max matches to return
 * @return Number of matches found
 */
int scan_plt_stubs(uint32_t start, uint32_t end,
                   uint32_t *matches_out, int max_matches);

/**
 * extract_got_from_plt - Extract GOT address from a PLT stub at given address
 * 
 * @param plt_addr  Address of PLT stub in game memory
 * @param out_got_addr  [out] GOT slot address
 * @return 0 on success, -1 if pattern doesn't match
 */
int extract_got_from_plt(uint32_t plt_addr, uint32_t *out_got_addr);

#endif /* USB_HOOKS_PLT_H */
