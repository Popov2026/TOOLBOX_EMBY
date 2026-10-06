/*
 * stemu - minimal headless Atari ST emulator for reverse engineering.
 * CPU: Musashi.  TOS: high level emulation (GEMDOS/BIOS/XBIOS/Line-A subset).
 * Hardware: shifter (low res), MFP 68901 (timers, ints), IKBD via ACIA, YM regs.
 *
 * usage: stemu [options] program.prg
 *   --dir D            GEMDOS root directory (files opened relative to it)
 *   --frames N         number of frames to run
 *   --shot F:file.ppm  screenshot at end of frame F (repeatable)
 *   --shots-every N:prefix   screenshot every N frames
 *   --key F:SC:down|up press/release scancode (hex) at frame F
 *   --joy F:mask       joystick 1 state at frame F (bit0 up,1 down,2 left,3 right,7 fire)
 *   --dump F:file      dump 1MB RAM at end of frame F
 *   --trace-traps      log TOS calls
 *   --bp ADDR          log when PC reaches ADDR (registers)
 *   --pclog F0:F1:file  log every executed PC (unique counts) between frames
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <dirent.h>
#include <strings.h>
#include "m68k.h"

#define RAMSIZE 0x100000
static uint8_t ram[RAMSIZE];
static uint8_t rom[0x40000]; /* at 0xFC0000 */
#define ROMBASE 0xFC0000

static const char *rootdir = ".";
static uint8_t *diskimg; static long disksize;
static int trace_traps = 0;
static long frame = 0;
static long line_in_frame = 0;

/* ---------- logging of unknown io ---------- */
static uint8_t io_seen[0x8000];
static void io_log(const char *what, uint32_t a, uint32_t v)
{
    uint32_t o = (a - 0xFF8000) & 0x7fff;
    if (!io_seen[o]) { io_seen[o] = 1; fprintf(stderr, "[io] first %s %06X = %X (frame %ld)\n", what, a, v, frame); }
}

/* ---------- video ---------- */
static uint8_t vbase_hi, vbase_mid, vbase_lo;
static uint16_t palette[16];
static uint8_t shifter_res, sync_mode;
static uint32_t vid_latched;

/* ---------- YM ---------- */
static uint8_t ym_sel, ym_reg[16];

/* ---------- MFP ---------- */
static uint8_t mfp_gpip = 0xff, mfp_aer, mfp_ddr, mfp_ier[2], mfp_ipr[2], mfp_isr[2], mfp_imr[2], mfp_vr;
static uint8_t mfp_tcr[4], mfp_tdr[4], mfp_tcnt[4];
static double mfp_tacc[4];
static uint8_t mfp_ucr, mfp_rsr, mfp_tsr, mfp_udr;
static int irq_vbl = 0, irq_hbl = 0;

static void update_irq(void);

/* channel numbering 0..15: 8..15 = register A bits 0..7, 0..7 = register B bits 0..7 */
static void mfp_request(int ch)
{
    int r = ch >= 8 ? 0 : 1, b = ch & 7;
    if (mfp_ier[r] & (1 << b)) { mfp_ipr[r] |= 1 << b; update_irq(); }
}
static int mfp_pending_channel(void)
{
    for (int ch = 15; ch >= 0; ch--) {
        int r = ch >= 8 ? 0 : 1, b = ch & 7;
        /* an in-service channel blocks itself and lower ones */
        if (mfp_isr[r] & (1 << b)) return -1;
        if ((mfp_ipr[r] & mfp_imr[r]) & (1 << b)) return ch;
    }
    return -1;
}
static void update_irq(void)
{
    int lvl = 0;
    if (irq_hbl) lvl = 2;
    if (irq_vbl) lvl = 4;
    if (mfp_pending_channel() >= 0) lvl = 6;
    m68k_set_irq(lvl);
}
int int_ack(int level)
{
    if (level == 6) {
        int ch = mfp_pending_channel();
        if (ch < 0) { update_irq(); return M68K_INT_ACK_SPURIOUS; }
        int r = ch >= 8 ? 0 : 1, b = ch & 7;
        mfp_ipr[r] &= ~(1 << b);
        if (mfp_vr & 8) mfp_isr[r] |= 1 << b;
        update_irq();
        return (mfp_vr & 0xf0) | ch;
    }
    if (level == 4) { irq_vbl = 0; update_irq(); return 24 + 4; }
    if (level == 2) { irq_hbl = 0; update_irq(); return 24 + 2; }
    return M68K_INT_ACK_AUTOVECTOR;
}
static const int mfp_prescale[8] = {0, 4, 10, 16, 50, 64, 100, 200};
static const int timer_ch[4] = {13, 8, 5, 4}; /* A,B,C,D */
static void mfp_timer_fire(int t)
{
    mfp_request(timer_ch[t]);
}
/* advance delay-mode timers by cpu cycles */
static void mfp_run(int cycles)
{
    for (int t = 0; t < 4; t++) {
        int mode;
        if (t < 2) mode = mfp_tcr[t] & 0x0f;
        else if (t == 2) mode = (mfp_tcr[2] >> 4) & 7;
        else mode = mfp_tcr[2] & 7;
        if (mode == 0 || mode >= 8) continue; /* stopped or event mode */
        mfp_tacc[t] += cycles * (2457600.0 / 8000000.0) / mfp_prescale[mode & 7];
        while (mfp_tacc[t] >= 1.0) {
            mfp_tacc[t] -= 1.0;
            mfp_tcnt[t]--;
            if (mfp_tcnt[t] == 0) { mfp_tcnt[t] = mfp_tdr[t]; mfp_timer_fire(t); }
        }
    }
}
static void mfp_event_tb(void)
{
    if ((mfp_tcr[1] & 0x0f) == 8) {
        mfp_tcnt[1]--;
        if (mfp_tcnt[1] == 0) { mfp_tcnt[1] = mfp_tdr[1]; mfp_timer_fire(1); }
    }
}
static uint8_t mfp_read(uint32_t a)
{
    switch (a & 0x3f) {
    case 0x01: return mfp_gpip;
    case 0x03: return mfp_aer;
    case 0x05: return mfp_ddr;
    case 0x07: return mfp_ier[0];
    case 0x09: return mfp_ier[1];
    case 0x0b: return mfp_ipr[0];
    case 0x0d: return mfp_ipr[1];
    case 0x0f: return mfp_isr[0];
    case 0x11: return mfp_isr[1];
    case 0x13: return mfp_imr[0];
    case 0x15: return mfp_imr[1];
    case 0x17: return mfp_vr;
    case 0x19: return mfp_tcr[0];
    case 0x1b: return mfp_tcr[1];
    case 0x1d: return mfp_tcr[2];
    case 0x1f: return mfp_tcnt[0];
    case 0x21: return mfp_tcnt[1];
    case 0x23: return mfp_tcnt[2];
    case 0x25: return mfp_tcnt[3];
    case 0x29: return mfp_ucr;
    case 0x2b: return mfp_rsr;
    case 0x2d: return mfp_tsr | 0x80;
    case 0x2f: return mfp_udr;
    }
    return 0xff;
}
static void mfp_write(uint32_t a, uint8_t v)
{
    switch (a & 0x3f) {
    case 0x01: mfp_gpip = v; break;
    case 0x03: mfp_aer = v; break;
    case 0x05: mfp_ddr = v; break;
    case 0x07: mfp_ier[0] = v; mfp_ipr[0] &= v; break;
    case 0x09: mfp_ier[1] = v; mfp_ipr[1] &= v; break;
    case 0x0b: mfp_ipr[0] &= v; break;
    case 0x0d: mfp_ipr[1] &= v; break;
    case 0x0f: mfp_isr[0] &= v; break;
    case 0x11: mfp_isr[1] &= v; break;
    case 0x13: mfp_imr[0] = v; break;
    case 0x15: mfp_imr[1] = v; break;
    case 0x17: mfp_vr = v; break;
    case 0x19: if ((mfp_tcr[0] & 0xf) == 0 && (v & 0xf)) mfp_tcnt[0] = mfp_tdr[0]; mfp_tcr[0] = v & 0x1f; break;
    case 0x1b: if ((mfp_tcr[1] & 0xf) == 0 && (v & 0xf)) mfp_tcnt[1] = mfp_tdr[1]; mfp_tcr[1] = v & 0x1f; break;
    case 0x1d: mfp_tcr[2] = v & 0x77; break;
    case 0x1f: mfp_tdr[0] = v; if (!(mfp_tcr[0] & 0xf)) mfp_tcnt[0] = v; break;
    case 0x21: mfp_tdr[1] = v; if (!(mfp_tcr[1] & 0xf)) mfp_tcnt[1] = v; break;
    case 0x23: mfp_tdr[2] = v; if (!(mfp_tcr[2] & 0x70)) mfp_tcnt[2] = v; break;
    case 0x25: mfp_tdr[3] = v; if (!(mfp_tcr[2] & 0x07)) mfp_tcnt[3] = v; break;
    case 0x29: mfp_ucr = v; break;
    case 0x2b: mfp_rsr = v; break;
    case 0x2d: mfp_tsr = v; break;
    case 0x2f: mfp_udr = v; break;
    }
    update_irq();
}

/* ---------- IKBD / ACIA ---------- */
static uint8_t acia_ctrl;
static uint8_t kq[4096]; static int kq_r, kq_w;   /* bytes waiting to be sent by ikbd */
static int acia_rx_full; static uint8_t acia_rx;
static int ikbd_joy_event = 1, ikbd_mouse_off = 0;
static uint8_t joy_state[2];
static uint8_t ikbd_cmd[8]; static int ikbd_cmd_len, ikbd_cmd_need;
static int ikbd_delay;
static void kq_push(uint8_t b) { kq[kq_w++ & 4095] = b; }
static void ikbd_send_joy(void) { kq_push(0xff); kq_push(joy_state[1]); }
static void ikbd_command(void)
{
    uint8_t c = ikbd_cmd[0];
    switch (c) {
    case 0x80: if (ikbd_cmd[1] == 1) { kq_push(0xf1); ikbd_joy_event = 1; } break; /* reset */
    case 0x12: ikbd_mouse_off = 1; break;
    case 0x14: ikbd_joy_event = 1; break;
    case 0x15: ikbd_joy_event = 0; break;
    case 0x16: kq_push(0xfd); kq_push(joy_state[0]); kq_push(joy_state[1]); break;
    case 0x1a: ikbd_joy_event = 0; break;
    default: break;
    }
}
static int ikbd_cmd_size(uint8_t c)
{
    switch (c) {
    case 0x07: return 2; case 0x08: return 1; case 0x09: return 5; case 0x0a: return 3; case 0x0b: return 3;
    case 0x0c: return 3; case 0x0d: return 1; case 0x0e: return 6; case 0x0f: return 1; case 0x10: return 1;
    case 0x11: return 1; case 0x12: return 1; case 0x13: return 1; case 0x14: return 1; case 0x15: return 1;
    case 0x16: return 1; case 0x17: return 2; case 0x18: return 1; case 0x19: return 7; case 0x1a: return 1;
    case 0x1b: return 7; case 0x1c: return 1; case 0x20: return 4; case 0x21: return 3; case 0x22: return 3;
    case 0x80: return 2;
    }
    return 1;
}
static void ikbd_recv(uint8_t b)
{
    if (ikbd_cmd_len == 0) ikbd_cmd_need = ikbd_cmd_size(b);
    ikbd_cmd[ikbd_cmd_len++ & 7] = b;
    if (ikbd_cmd_len >= ikbd_cmd_need) { ikbd_command(); ikbd_cmd_len = 0; }
}
static void acia_update_irq(void)
{
    /* GPIP4 low when ACIA irq; MFP channel 6 */
    if (acia_rx_full && (acia_ctrl & 0x80)) { mfp_gpip &= ~0x10; mfp_request(6); }
    else mfp_gpip |= 0x10;
}
static void ikbd_tick(void) /* called each scanline */
{
    if (ikbd_delay > 0) { ikbd_delay--; return; }
    if (!acia_rx_full && kq_r != kq_w) {
        acia_rx = kq[kq_r++ & 4095]; acia_rx_full = 1; ikbd_delay = 2; acia_update_irq();
    }
}

/* ---------- TOS HLE ---------- */
#define STUB_GEMDOS (ROMBASE + 0x10)
#define STUB_BIOS   (ROMBASE + 0x20)
#define STUB_XBIOS  (ROMBASE + 0x30)
#define STUB_LINEA  (ROMBASE + 0x40)
#define STUB_RTE    (ROMBASE + 0x50)  /* plain rte (no hle) */
#define STUB_UNHANDLED (ROMBASE + 0x60)
#define STUB_VBL    (ROMBASE + 0x70)
#define STUB_TIMERC (ROMBASE + 0x80)
#define STUB_ACIA   (ROMBASE + 0x90)
#define STUB_RTS    (ROMBASE + 0xa0)  /* plain rts */
#define STUB_KBDRET (ROMBASE + 0xb0)  /* hle: return from ikbd sub-vector call */
#define STUB_EXIT   (ROMBASE + 0xc0)
#define LINEA_VARS  0x2000
#define KBDVECS     0x1e00
#define IKBD_PKT    0x1f00
#define DTA_DEFAULT 0x1f80

static uint32_t rd8(uint32_t a);
static uint32_t rd16(uint32_t a);
static uint32_t rd32(uint32_t a);
static void wr8(uint32_t a, uint32_t v);
static void wr16(uint32_t a, uint32_t v);
static void wr32(uint32_t a, uint32_t v);

static uint32_t memtop;
static uint32_t dta = DTA_DEFAULT;
static FILE *fh[64];
static uint32_t malloc_ptr;
static int halted = 0;

static void rdstr(uint32_t a, char *out, int n)
{
    int i; for (i = 0; i < n - 1; i++) { out[i] = rd8(a + i); if (!out[i]) break; } out[i] = 0;
}
static void host_path(const char *st, char *out)
{
    const char *p = st;
    if (p[1] == ':') p += 2;
    while (*p == '\\' || *p == '/') p++;
    char tmp[512]; int j = 0;
    for (; *p && j < 500; p++) tmp[j++] = (*p == '\\') ? '/' : *p;
    tmp[j] = 0;
    /* case-insensitive match of each component */
    char cur[1024]; snprintf(cur, sizeof cur, "%s", rootdir);
    char *save, *tok = strtok_r(tmp, "/", &save);
    while (tok) {
        DIR *d = opendir(cur); char found[256] = "";
        if (d) { struct dirent *e; while ((e = readdir(d))) if (!strcasecmp(e->d_name, tok)) { snprintf(found, sizeof found, "%s", e->d_name); break; } closedir(d); }
        size_t L = strlen(cur);
        snprintf(cur + L, sizeof cur - L, "/%s", found[0] ? found : tok);
        tok = strtok_r(NULL, "/", &save);
    }
    strcpy(out, cur);
}

/* keyboard buffer for TOS console */
static uint8_t keybuf[256]; static int kb_r, kb_w;
static const char sc2ascii[128] = {
 0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8,9,'q','w','e','r','t','y','u','i','o','p','[',']',13,0,'a','s',
 'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v','b','n','m',',','.','/',0,0,0,' ',0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,'-',0,0,0,'+',0,0,0,0,0x7f,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };

/* pending ikbd packet assembly for HLE acia handler */
static uint8_t pk[8]; static int pk_len, pk_need, pk_type;
static uint32_t saved_regs[16]; static int in_kbd_call;

/* set SR swapping stacks like the real cpu (musashi's set_reg(SR) does not) */
static void set_sr(uint32_t nsr)
{
    uint32_t osr = m68k_get_reg(NULL, M68K_REG_SR);
    if ((osr & 0x2000) && !(nsr & 0x2000)) {
        uint32_t ssp = m68k_get_reg(NULL, M68K_REG_A7), usp = m68k_get_reg(NULL, M68K_REG_USP);
        m68k_set_reg(M68K_REG_SR, nsr); m68k_set_reg(M68K_REG_A7, usp); m68k_set_reg(M68K_REG_ISP, ssp);
    } else if (!(osr & 0x2000) && (nsr & 0x2000)) {
        uint32_t usp = m68k_get_reg(NULL, M68K_REG_A7), ssp = m68k_get_reg(NULL, M68K_REG_ISP);
        m68k_set_reg(M68K_REG_SR, nsr); m68k_set_reg(M68K_REG_A7, ssp); m68k_set_reg(M68K_REG_USP, usp);
    } else m68k_set_reg(M68K_REG_SR, nsr);
}
static void hle_return(uint32_t sr, uint32_t pc, uint32_t d0)
{
    m68k_set_reg(M68K_REG_D0, d0);
    set_sr(sr);
    m68k_set_reg(M68K_REG_PC, pc);
}

static uint32_t load_prg(const char *path, uint32_t bp, uint32_t parent);

static void do_gemdos(uint32_t sr, uint32_t pc, uint32_t args)
{
    int fn = rd16(args);
    uint32_t d0 = 0;
    char s[300], hp[1100];
    if (trace_traps) fprintf(stderr, "[gemdos] f=%02X pc=%06X frame %ld\n", fn, pc, frame);
    switch (fn) {
    case 0x00: case 0x4c: case 0x31:
        fprintf(stderr, "[gemdos] Pterm (%X) frame %ld\n", fn, frame); halted = 1; d0 = 0; break;
    case 0x01: case 0x07: case 0x08:
        if (kb_r == kb_w) { /* block: re-execute trap */ hle_return(sr, pc - 2, 0); m68k_set_reg(M68K_REG_A7, m68k_get_reg(NULL, M68K_REG_A7)); return; }
        d0 = keybuf[kb_r++ & 255]; d0 = ((d0 >> 8) << 16) | d0; break;
    case 0x02: case 0x03: case 0x04: case 0x05: fputc(rd16(args + 2) & 0xff, stdout); break;
    case 0x06: { int c = rd16(args + 2); if (c == 0xff) { d0 = (kb_r != kb_w) ? keybuf[kb_r++ & 255] : 0; } else fputc(c, stdout); } break;
    case 0x09: rdstr(rd32(args + 2), s, 300); fputs(s, stdout); break;
    case 0x0b: d0 = (kb_r != kb_w) ? 0xffffffff : 0; break;
    case 0x0e: d0 = 3; break;
    case 0x19: d0 = 0; break;
    case 0x1a: dta = rd32(args + 2); break;
    case 0x2f: d0 = dta; break;
    case 0x30: d0 = 0x1300; break;
    case 0x2a: d0 = (9 << 9) | (1 << 5) | 1; break;
    case 0x2c: d0 = 0; break;
    case 0x36: { uint32_t b = rd32(args + 2); wr32(b, 300); wr32(b + 4, 711); wr32(b + 8, 512); wr32(b + 12, 2); } break;
    case 0x3b: d0 = 0; break;
    case 0x47: wr8(rd32(args + 2), 0); break;
    case 0x20: { /* Super */
        uint32_t a = rd32(args + 2);
        uint32_t cursp = m68k_get_reg(NULL, M68K_REG_A7);
        if (a == 1) { d0 = (sr & 0x2000) ? 0xffffffff : 0; break; }
        if (!(sr & 0x2000)) { /* user -> super */
            uint32_t usp = m68k_get_reg(NULL, M68K_REG_USP);
            d0 = cursp; m68k_set_reg(M68K_REG_A7, a ? a : usp);
            m68k_set_reg(M68K_REG_D0, d0); m68k_set_reg(M68K_REG_SR, sr | 0x2000); m68k_set_reg(M68K_REG_PC, pc); return;
        } else { /* super -> user, a = old ssp */
            /* caller's stack (current ssp) becomes usp, ssp = a */
            m68k_set_reg(M68K_REG_SR, sr & ~0x2000);   /* no swap */
            m68k_set_reg(M68K_REG_A7, cursp); m68k_set_reg(M68K_REG_ISP, a);
            m68k_set_reg(M68K_REG_D0, 0); m68k_set_reg(M68K_REG_PC, pc); return;
        }
    }
    case 0x3d: case 0x3c: {
        rdstr(rd32(args + 2), s, 300); host_path(s, hp);
        int h; for (h = 6; h < 64 && fh[h]; h++);
        FILE *f = fopen(hp, fn == 0x3c ? "w+b" : (rd16(args + 6) ? "r+b" : "rb"));
        if (!f && fn == 0x3d) f = fopen(hp, "rb");
        if (!f || h >= 64) { d0 = -33; if (f) fclose(f); }
        else { fh[h] = f; d0 = h; }
        fprintf(stderr, "[gemdos] %s '%s' -> %d\n", fn == 0x3c ? "Fcreate" : "Fopen", s, (int)d0);
        break; }
    case 0x3e: { int h = rd16(args + 2); if (h < 64 && fh[h]) { fclose(fh[h]); fh[h] = 0; } d0 = 0; break; }
    case 0x3f: case 0x40: {
        int h = rd16(args + 2); uint32_t n = rd32(args + 4), b = rd32(args + 8);
        if (h >= 64 || !fh[h]) { d0 = -37; break; }
        uint32_t i;
        if (fn == 0x3f) { for (i = 0; i < n; i++) { int c = fgetc(fh[h]); if (c == EOF) break; wr8(b + i, c); } }
        else { for (i = 0; i < n; i++) fputc(rd8(b + i), fh[h]); fflush(fh[h]); }
        d0 = i;
        if (trace_traps || 1) fprintf(stderr, "[gemdos] %s h=%d n=%X buf=%06X -> %X\n", fn == 0x3f ? "Fread" : "Fwrite", h, n, b, d0);
        break; }
    case 0x41: d0 = 0; break;
    case 0x42: { int32_t off = rd32(args + 2); int h = rd16(args + 6), m = rd16(args + 8);
        if (h >= 64 || !fh[h]) { d0 = -37; break; }
        fseek(fh[h], off, m == 0 ? SEEK_SET : m == 1 ? SEEK_CUR : SEEK_END); d0 = ftell(fh[h]); break; }
    case 0x48: { int32_t n = rd32(args + 2);
        if (n == -1) d0 = memtop - malloc_ptr;
        else { n = (n + 1) & ~1; if (malloc_ptr + n > memtop) d0 = 0; else { d0 = malloc_ptr; malloc_ptr += n; } }
        fprintf(stderr, "[gemdos] Malloc(%d) -> %X\n", n, d0);
        break; }
    case 0x49: d0 = 0; break;
    case 0x4a: { uint32_t b = rd32(args + 4), n = rd32(args + 8); fprintf(stderr, "[gemdos] Mshrink(%X,%X)\n", b, n); malloc_ptr = b + n; if (malloc_ptr < 0x1000) malloc_ptr = 0x1000; d0 = 0; break; }
    case 0x4b: { int mode = rd16(args + 2); rdstr(rd32(args + 4), s, 300);
        fprintf(stderr, "[gemdos] Pexec(%d,'%s')\n", mode, s);
        /* mode 0: load and go.  We emulate by loading at malloc_ptr and jumping; child Pterm halts emulation */
        host_path(s, hp);
        uint32_t bp = (malloc_ptr + 0xff) & ~0xff;
        uint32_t entry = load_prg(hp, bp, 0);
        if (!entry) { d0 = -33; break; }
        if (mode == 3) { d0 = bp; break; }
        /* run child in user mode with its own stack */
        uint32_t usp = memtop - 4;
        wr32(usp, bp); usp -= 4; wr32(usp, STUB_EXIT);
        m68k_set_reg(M68K_REG_USP, usp);
        hle_return(sr & ~0x2000, entry, 0); return; }
    case 0x4e: case 0x4f: {
        static DIR *sd; static char pat[64];
        if (fn == 0x4e) { rdstr(rd32(args + 2), s, 300); char *b = strrchr(s, '\\'); snprintf(pat, sizeof pat, "%s", b ? b + 1 : s);
            if (sd) closedir(sd); sd = opendir(rootdir); }
        d0 = -49;
        if (sd) { struct dirent *e; while ((e = readdir(sd))) {
            if (e->d_name[0] == '.') continue;
            /* simple match: pattern "*.*" or exact name or "*.EXT" */
            int ok = 0; if (!strcmp(pat, "*.*") || !strcasecmp(pat, e->d_name)) ok = 1;
            else if (pat[0] == '*' && pat[1] == '.') { char *x = strrchr(e->d_name, '.'); if (x && !strcasecmp(x + 1, pat + 2)) ok = 1; }
            if (!ok) continue;
            char p2[1100]; snprintf(p2, sizeof p2, "%s/%s", rootdir, e->d_name); FILE *f = fopen(p2, "rb"); long sz = 0; if (f) { fseek(f, 0, 2); sz = ftell(f); fclose(f); }
            for (int i = 0; i < 44; i++) wr8(dta + i, 0);
            wr8(dta + 21, 0); wr32(dta + 26, sz); for (int i = 0; i < 13 && e->d_name[i]; i++) wr8(dta + 30 + i, toupper(e->d_name[i]));
            d0 = 0; break; } }
        break; }
    default:
        fprintf(stderr, "[gemdos] UNIMPLEMENTED f=%02X pc=%06X\n", fn, pc); d0 = -32;
    }
    hle_return(sr, pc, d0);
}

static void do_bios(uint32_t sr, uint32_t pc, uint32_t args)
{
    int fn = rd16(args); uint32_t d0 = 0;
    if (trace_traps) fprintf(stderr, "[bios] f=%02X pc=%06X\n", fn, pc);
    switch (fn) {
    case 1: d0 = (kb_r != kb_w) ? 0xffffffff : 0; break;
    case 2: if (kb_r == kb_w) { hle_return(sr, pc - 2, 0); return; } d0 = keybuf[kb_r++ & 255]; break;
    case 3: fputc(rd16(args + 4) & 0xff, stdout); break;
    case 5: { int v = rd16(args + 2); uint32_t a = rd32(args + 4); d0 = rd32(v * 4); if (a != 0xffffffff) wr32(v * 4, a);
        fprintf(stderr, "[bios] Setexc(%X,%X)\n", v, a); break; }
    case 6: d0 = 20; break;
    case 8: d0 = -1; break;
    case 10: d0 = 3; break;
    case 11: d0 = 0; break;
    default: fprintf(stderr, "[bios] UNIMPLEMENTED f=%02X\n", fn); d0 = -32;
    }
    hle_return(sr, pc, d0);
}

static void do_xbios(uint32_t sr, uint32_t pc, uint32_t args)
{
    int fn = rd16(args); uint32_t d0 = 0;
    if (trace_traps) fprintf(stderr, "[xbios] f=%02X pc=%06X frame %ld\n", fn, pc, frame);
    switch (fn) {
    case 2: case 3: d0 = (vbase_hi << 16) | (vbase_mid << 8); break;
    case 4: d0 = shifter_res & 3; break;
    case 5: { uint32_t l = rd32(args + 2), p = rd32(args + 6); int r = (int16_t)rd16(args + 10);
        if (p != 0xffffffff) { vbase_hi = p >> 16; vbase_mid = p >> 8; }
        if (r >= 0) shifter_res = r;
        fprintf(stderr, "[xbios] Setscreen(%X,%X,%d)\n", l, p, r); break; }
    case 6: { uint32_t p = rd32(args + 2); for (int i = 0; i < 16; i++) palette[i] = rd16(p + i * 2); break; }
    case 7: { int n = rd16(args + 2), c = (int16_t)rd16(args + 4); d0 = palette[n & 15]; if (c >= 0) palette[n & 15] = c; break; }
    case 14: d0 = 0x1f00 + 0x60; break; /* Iorec: fake */
    case 16: d0 = 0x1c00; wr32(0x1c00, 0x1c40); wr32(0x1c04, 0x1cc0); wr32(0x1c08, 0x1d40);
        for (int k = 0; k < 128; k++) { wr8(0x1c40 + k, sc2ascii[k]); wr8(0x1cc0 + k, toupper(sc2ascii[k])); wr8(0x1d40 + k, toupper(sc2ascii[k])); } break; /* Keytbl */
    case 17: d0 = rand() & 0xffffff; break;
    case 21: d0 = 0; break; /* Cursconf */
    case 25: { int n = rd16(args + 2); uint32_t p = rd32(args + 4); for (int i = 0; i <= n; i++) ikbd_recv(rd8(p + i)); break; }
    case 26: { int b = rd16(args + 2) & 15; mfp_ier[b >= 8 ? 0 : 1] &= ~(1 << (b & 7)); mfp_imr[b >= 8 ? 0 : 1] &= ~(1 << (b & 7)); update_irq(); break; }
    case 27: { int b = rd16(args + 2) & 15; mfp_ier[b >= 8 ? 0 : 1] |= (1 << (b & 7)); mfp_imr[b >= 8 ? 0 : 1] |= (1 << (b & 7)); update_irq(); break; }
    case 29: d0 = ym_reg[14]; ym_reg[14] &= rd16(args + 2); break;
    case 30: d0 = ym_reg[14]; ym_reg[14] |= rd16(args + 2); break;
    case 31: { int t = rd16(args + 2), ctrl = rd16(args + 4), data = rd16(args + 6); uint32_t v = rd32(args + 8);
        int ch = timer_ch[t & 3];
        fprintf(stderr, "[xbios] Xbtimer(%d,%X,%X,%X)\n", t, ctrl, data, v);
        if (v != 0xffffffff) wr32(((mfp_vr & 0xf0) + ch) * 4, v);
        if (t == 0) { mfp_tdr[0] = data; mfp_tcnt[0] = data; mfp_tcr[0] = ctrl; }
        if (t == 1) { mfp_tdr[1] = data; mfp_tcnt[1] = data; mfp_tcr[1] = ctrl; }
        if (t == 2) { mfp_tdr[2] = data; mfp_tcnt[2] = data; mfp_tcr[2] = (mfp_tcr[2] & 7) | (ctrl << 4); }
        if (t == 3) { mfp_tdr[3] = data; mfp_tcnt[3] = data; mfp_tcr[3] = (mfp_tcr[2] & 0x70) | (ctrl & 7); }
        int r = ch >= 8 ? 0 : 1; mfp_ier[r] |= 1 << (ch & 7); mfp_imr[r] |= 1 << (ch & 7); update_irq();
        break; }
    case 8: case 9: { /* Floprd / Flopwr */
        uint32_t buf = rd32(args + 2); int dev = rd16(args + 10), sec = rd16(args + 12), trk = rd16(args + 14), side = rd16(args + 16), cnt = rd16(args + 18);
        fprintf(stderr, "[xbios] %s buf=%X dev=%d sec=%d trk=%d side=%d cnt=%d pc=%06X frame %ld\n", fn == 8 ? "Floprd" : "Flopwr", buf, dev, sec, trk, side, cnt, pc, frame);
        if (!diskimg) { d0 = -2; break; }
        long off = ((long)(trk * 2 + side) * 9 + (sec - 1)) * 512;
        if (sec < 1 || sec > 9 || off + cnt * 512 > disksize) { d0 = -8; break; }
        for (int i = 0; i < cnt * 512; i++) { if (fn == 8) wr8(buf + i, diskimg[off + i]); else diskimg[off + i] = rd8(buf + i); }
        d0 = 0; break; }
    case 32: break; /* Dosound */
    case 34: d0 = KBDVECS; break;
    case 37: { /* Vsync: block until next vbl -> we approximate by returning */ break; }
    case 38: { /* Supexec */
        uint32_t f = rd32(args + 2);
        uint32_t ssp = m68k_get_reg(NULL, M68K_REG_A7);
        /* push original frame back + return to rte stub */
        ssp -= 6; wr16(ssp, sr); wr32(ssp + 2, pc);
        ssp -= 4; wr32(ssp, STUB_RTE);
        m68k_set_reg(M68K_REG_A7, ssp);
        set_sr(sr | 0x2000);
        m68k_set_reg(M68K_REG_PC, f);
        return; }
    case 64: d0 = 0; break; /* Blitmode */
    default: fprintf(stderr, "[xbios] UNIMPLEMENTED f=%02X\n", fn); d0 = -32;
    }
    hle_return(sr, pc, d0);
}

/* call a 68k subroutine from HLE (ikbd vectors). Frame (sr,pc) already popped. */
static void call_kbd_vector(uint32_t vec, uint32_t a0, uint32_t sr, uint32_t pc)
{
    uint32_t ssp = m68k_get_reg(NULL, M68K_REG_A7);
    ssp -= 6; wr16(ssp, sr); wr32(ssp + 2, pc);           /* original interrupt frame */
    for (int i = 0; i < 16; i++) saved_regs[i] = m68k_get_reg(NULL, M68K_REG_D0 + i);
    ssp -= 4; wr32(ssp, STUB_KBDRET);
    m68k_set_reg(M68K_REG_A7, ssp);
    m68k_set_reg(M68K_REG_A0, a0);
    in_kbd_call = 1;
    m68k_set_reg(M68K_REG_PC, vec);
}

static void do_acia(uint32_t sr, uint32_t pc)
{
    /* emulates TOS ikbd interrupt handler: assemble packets, dispatch */
    while (acia_rx_full) {
        uint8_t b = acia_rx; acia_rx_full = 0; acia_update_irq();
        if (pk_len == 0) {
            if (b >= 0xf6) {
                pk_type = b;
                switch (b) { case 0xf6: pk_need = 8; break; case 0xf7: pk_need = 6; break; case 0xfc: pk_need = 7; break;
                    case 0xfd: pk_need = 3; break; case 0xfe: case 0xff: pk_need = 2; break; default: pk_need = 3; }
                if (b >= 0xf8 && b <= 0xfb) pk_need = 3;
                pk[pk_len++] = b;
            } else {
                /* key */
                if (!(b & 0x80)) { uint8_t c = sc2ascii[b & 0x7f]; keybuf[kb_w++ & 255] = c; }
                /* real TOS stores scancode in high word; keep simple */
            }
        } else {
            pk[pk_len++] = b;
            if (pk_len >= pk_need) {
                for (int i = 0; i < pk_len; i++) wr8(IKBD_PKT + i, pk[i]);
                uint32_t vec = 0;
                if (pk_type == 0xfe || pk_type == 0xff || pk_type == 0xfd) vec = rd32(KBDVECS + 24);
                else if (pk_type >= 0xf8 && pk_type <= 0xfb) vec = rd32(KBDVECS + 16);
                else if (pk_type == 0xf6) vec = rd32(KBDVECS + 12);
                else if (pk_type == 0xfc) vec = rd32(KBDVECS + 20);
                pk_len = 0;
                if (vec && vec != STUB_RTS) {
                    mfp_isr[1] &= ~0x40;
                    call_kbd_vector(vec, IKBD_PKT + (pk_type == 0xfd ? 0 : 0), sr, pc);
                    return;
                }
            }
        }
    }
    mfp_isr[1] &= ~0x40; update_irq();
    hle_return(sr, pc, m68k_get_reg(NULL, M68K_REG_D0));
}

static long bp_addr = -1;

static FILE *pclog_f; static long pclog_f0 = -1, pclog_f1 = -1;
static uint32_t *pc_counts;

void instr_hook(unsigned int pc)
{
    if (pc_counts && frame >= pclog_f0 && frame <= pclog_f1 && pc < RAMSIZE) pc_counts[pc >> 1]++;
    if (pc == bp_addr) {
        fprintf(stderr, "[bp] %06X frame %ld line %ld D:", pc, frame, line_in_frame);
        for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", m68k_get_reg(NULL, M68K_REG_D0 + i));
        fprintf(stderr, " A:"); for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", m68k_get_reg(NULL, M68K_REG_A0 + i));
        fprintf(stderr, "\n");
    }
    if (pc < ROMBASE || pc >= ROMBASE + 0x100) return;
    uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
    if (pc == STUB_KBDRET) {
        /* returned from kbd vector: restore regs, return from interrupt */
        for (int i = 0; i < 15; i++) m68k_set_reg(M68K_REG_D0 + i, saved_regs[i]);
        uint32_t sr = rd16(sp), rpc = rd32(sp + 2);
        m68k_set_reg(M68K_REG_A7, sp + 6);
        in_kbd_call = 0; update_irq();
        set_sr(sr); m68k_set_reg(M68K_REG_PC, rpc);
        return;
    }
    if (pc == STUB_EXIT) { fprintf(stderr, "[emu] child returned\n"); halted = 1; return; }
    if (pc == STUB_RTE || pc == STUB_RTS) return;
    uint32_t sr = rd16(sp), rpc = rd32(sp + 2);
    m68k_set_reg(M68K_REG_A7, sp + 6);
    uint32_t args = (sr & 0x2000) ? sp + 6 : m68k_get_reg(NULL, M68K_REG_USP);
    switch (pc) {
    case STUB_GEMDOS: do_gemdos(sr, rpc, args); break;
    case STUB_BIOS: do_bios(sr, rpc, args); break;
    case STUB_XBIOS: do_xbios(sr, rpc, args); break;
    case STUB_LINEA: {
        uint16_t op = rd16(rpc);
        if (op == 0xa000) { m68k_set_reg(M68K_REG_A0, LINEA_VARS); m68k_set_reg(M68K_REG_D0, LINEA_VARS);
            m68k_set_reg(M68K_REG_A1, LINEA_VARS + 0x400); m68k_set_reg(M68K_REG_A2, LINEA_VARS + 0x500); }
        set_sr(sr); m68k_set_reg(M68K_REG_PC, rpc + 2);
        break; }
    case STUB_VBL:
        wr32(0x462, rd32(0x462) + 1); wr32(0x466, rd32(0x466) + 1);
        set_sr(sr); m68k_set_reg(M68K_REG_PC, rpc);
        break;
    case STUB_TIMERC:
        wr32(0x4ba, rd32(0x4ba) + 1); mfp_isr[1] &= ~0x20; update_irq();
        set_sr(sr); m68k_set_reg(M68K_REG_PC, rpc);
        break;
    case STUB_ACIA: do_acia(sr, rpc); break;
    default: {
        uint32_t ppc = m68k_get_reg(NULL, M68K_REG_PPC);
        fprintf(stderr, "[emu] UNHANDLED exception stub %06X, frame sr=%04X pc=%06X ppc=%06X frame %ld\n", pc, sr, rpc, ppc, frame);
        for (int i = 0; i < 16; i++) fprintf(stderr, "%08X%c", m68k_get_reg(NULL, M68K_REG_D0 + i), i == 7 ? '\n' : ' ');
        fprintf(stderr, "\n");
        halted = 1; m68k_end_timeslice();
    }
    }
}

/* ---------- memory bus ---------- */
static uint32_t io_read8(uint32_t a)
{
    if (a >= 0xff8240 && a < 0xff8260) { int i = (a - 0xff8240) >> 1; return (a & 1) ? palette[i] & 0xff : palette[i] >> 8; }
    switch (a) {
    case 0xff8001: return 0x05;
    case 0xff8201: return vbase_hi;
    case 0xff8203: return vbase_mid;
    case 0xff820d: return vbase_lo;
    case 0xff8205: case 0xff8207: case 0xff8209: {
        /* video address counter */
        uint32_t base = vid_latched;
        long l = line_in_frame - 63; uint32_t pos = base;
        if (l >= 0 && l < 200) pos = base + l * 160 + (m68k_cycles_run() * 160 / 512);
        else if (l >= 200) pos = base + 32000;
        if (a == 0xff8205) return (pos >> 16) & 0xff; if (a == 0xff8207) return (pos >> 8) & 0xff; return pos & 0xff; }
    case 0xff820a: return sync_mode;
    case 0xff8260: return shifter_res;
    case 0xff8800: return ym_reg[ym_sel & 15];
    case 0xfffc00: return (acia_rx_full ? 1 : 0) | 2 | ((acia_rx_full && (acia_ctrl & 0x80)) ? 0x80 : 0);
    case 0xfffc02: { uint8_t v = acia_rx; acia_rx_full = 0; acia_update_irq(); return v; }
    case 0xfffc04: return 2;
    case 0xfffc06: return 0;
    }
    if (a >= 0xfffa00 && a < 0xfffa40) return mfp_read(a);
    if (a >= 0xff8600 && a < 0xff8610) { io_log("rd", a, 0); return 0; }
    io_log("rd", a, 0);
    return 0xff;
}
static void io_write8(uint32_t a, uint8_t v)
{
    if (a >= 0xff8240 && a < 0xff8260) { int i = (a - 0xff8240) >> 1;
        if (a & 1) palette[i] = (palette[i] & 0xff00) | v; else palette[i] = (palette[i] & 0xff) | (v << 8); return; }
    switch (a) {
    case 0xff8001: return;
    case 0xff8201: vbase_hi = v; return;
    case 0xff8203: vbase_mid = v; return;
    case 0xff820d: vbase_lo = v; return;
    case 0xff820a: sync_mode = v; return;
    case 0xff8260: shifter_res = v & 3; return;
    case 0xff8800: ym_sel = v; return;
    case 0xff8802: ym_reg[ym_sel & 15] = v; return;
    case 0xfffc00: acia_ctrl = v; acia_update_irq(); return;
    case 0xfffc02: ikbd_recv(v); return;
    case 0xfffc04: case 0xfffc06: return;
    }
    if (a >= 0xfffa00 && a < 0xfffa40) { mfp_write(a, v); return; }
    io_log("wr", a, v);
}
static uint32_t ww_lo = 1, ww_hi = 0, wr_lo = 1, wr_hi = 0; static int ww_n;
static void watch_hit(const char *k, uint32_t a, uint32_t v)
{
    if (ww_n++ < 400000) fprintf(stderr, "[%s] %06X=%02X pc=%06X f%ld\n", k, a, v & 0xff, m68k_get_reg(NULL, M68K_REG_PPC), frame);
}
static uint32_t rd8(uint32_t a)
{
    a &= 0xffffff;
    if (a >= wr_lo && a <= wr_hi && a < RAMSIZE) watch_hit("rd", a, ram[a]);
    if (a < RAMSIZE) return ram[a];
    if (a >= ROMBASE && a < 0xff0000) return rom[a - ROMBASE];
    if (a >= 0xff8000) return io_read8(a);
    return 0xff;
}
static uint32_t rd16(uint32_t a) { a &= 0xffffff; if (a + 1 >= wr_lo && a <= wr_hi) return rd8(a) << 8 | rd8(a + 1); if (a < RAMSIZE - 1) return ram[a] << 8 | ram[a + 1]; return rd8(a) << 8 | rd8(a + 1); }
static uint32_t rd32(uint32_t a) { return rd16(a) << 16 | rd16(a + 2); }
static void wr8(uint32_t a, uint32_t v)
{
    a &= 0xffffff;
    if (a >= ww_lo && a <= ww_hi) watch_hit("ww", a, v);
    if (a < RAMSIZE) { ram[a] = v; return; }
    if (a >= 0xff8000) io_write8(a, v);
}
static void wr16(uint32_t a, uint32_t v) { a &= 0xffffff; if (a + 1 >= ww_lo && a <= ww_hi) { wr8(a, v >> 8); wr8(a + 1, v); return; } if (a < RAMSIZE - 1) { ram[a] = v >> 8; ram[a + 1] = v; return; } wr8(a, v >> 8); wr8(a + 1, v); }
static void wr32(uint32_t a, uint32_t v) { wr16(a, v >> 16); wr16(a + 2, v); }

unsigned int m68k_read_memory_8(unsigned int a) { return rd8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return rd16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return rd32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { wr8(a, v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { wr16(a, v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { wr32(a, v); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return rd8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return rd16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return rd32(a); }

/* ---------- program loading ---------- */
static uint32_t load_prg(const char *path, uint32_t bp, uint32_t parent)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 0; }
    fseek(f, 0, 2); long sz = ftell(f); fseek(f, 0, 0);
    uint8_t *d = malloc(sz); fread(d, 1, sz, f); fclose(f);
#define B32(p) ((uint32_t)d[p] << 24 | d[p + 1] << 16 | d[p + 2] << 8 | d[p + 3])
    uint32_t tl = B32(2), dl = B32(6), bl = B32(10), sl = B32(14);
    uint32_t tb = bp + 0x100;
    memcpy(ram + tb, d + 0x1c, tl + dl);
    memset(ram + tb + tl + dl, 0, bl);
    uint32_t r = 0x1c + tl + dl + sl;
    if (!B32(0x16 + 4) || 1) {
        uint32_t off = B32(r); r += 4;
        if (off) {
            uint32_t a = tb + off; wr32(a, rd32(a) + tb);
            for (;;) { uint8_t c = d[r++]; if (!c) break; if (c == 1) { a += 254; continue; } a += c; wr32(a, rd32(a) + tb); }
        }
    }
    for (int i = 0; i < 256; i++) ram[bp + i] = 0;
    wr32(bp + 0, bp); wr32(bp + 4, memtop); wr32(bp + 8, tb); wr32(bp + 12, tl);
    wr32(bp + 16, tb + tl); wr32(bp + 20, dl); wr32(bp + 24, tb + tl + dl); wr32(bp + 28, bl);
    wr32(bp + 32, bp + 0x80); wr32(bp + 36, parent); wr32(bp + 44, 0);
    malloc_ptr = tb + tl + dl + bl;
    fprintf(stderr, "[emu] loaded %s at bp=%X text=%X data=%X bss=%X\n", path, bp, tl, dl, bl);
    free(d);
    return tb;
}

static void write_ppm(const char *fn)
{
    uint32_t base = vid_latched;
    FILE *f = fopen(fn, "wb");
    fprintf(f, "P6\n320 200\n255\n");
    for (int y = 0; y < 200; y++) for (int x = 0; x < 320; x++) {
        uint32_t w = base + y * 160 + (x >> 4) * 8; int bit = 15 - (x & 15); int c = 0;
        for (int p = 0; p < 4; p++) c |= ((rd16(w + p * 2) >> bit) & 1) << p;
        uint16_t col = palette[c];
        int r = (col >> 8) & 7, g = (col >> 4) & 7, b = col & 7;
        fputc(r * 255 / 7, f); fputc(g * 255 / 7, f); fputc(b * 255 / 7, f);
    }
    fclose(f);
}

struct ev { long frame; int type; int a, b; char file[256]; };
static struct ev evs[4096]; static int nev;

static void init_machine(void)
{
    memtop = RAMSIZE - 0x8000;
    /* rom stubs */
    for (int i = 0; i < 0x100; i += 2) { rom[i] = 0x4e; rom[i + 1] = 0x73; } /* rte everywhere */
    rom[0xa0] = 0x4e; rom[0xa1] = 0x75; /* rts */
    for (int v = 2; v < 64; v++) wr32(v * 4, STUB_UNHANDLED);
    for (int v = 64; v < 256; v++) wr32(v * 4, STUB_RTE);
    wr32(0x24 * 4 / 4 * 0 + 0x84, STUB_GEMDOS);
    wr32(0xb4, STUB_BIOS); wr32(0xb8, STUB_XBIOS);
    wr32(0x28, STUB_LINEA);
    wr32(0x68, STUB_RTE); wr32(0x70, STUB_VBL); wr32(0x64, STUB_RTE); wr32(0x6c, STUB_RTE); wr32(0x74, STUB_RTE); wr32(0x78, STUB_RTE); wr32(0x7c, STUB_RTE);
    wr32(0x80, STUB_RTE); for (int t = 0x88; t < 0xc0; t += 4) if (t != 0xb4 && t != 0xb8) wr32(t, STUB_RTE);
    wr32(0x114, STUB_TIMERC); wr32(0x118, STUB_ACIA);
    for (int i = 0; i < 9; i++) wr32(KBDVECS + i * 4, STUB_RTS);
    /* sysvars */
    wr32(0x42e, RAMSIZE); wr32(0x436, memtop); wr32(0x432, 0x800); wr16(0x454, 8); wr32(0x456, 0x1d00);
    wr32(0x44e, RAMSIZE - 0x8000); wr16(0x44c, 0); wr8(0x484, 7); wr32(0x4f2, ROMBASE + 0x100);
    /* fake rom header */
    rom[0x100] = 0x60; rom[0x101] = 0x2e; rom[0x102] = 0x01; rom[0x103] = 0x04; /* TOS 1.04 */
    vbase_hi = (RAMSIZE - 0x8000) >> 16; vbase_mid = ((RAMSIZE - 0x8000) >> 8) & 0xff;
    palette[0] = 0x777; palette[15] = 0;
    /* mfp like TOS */
    mfp_vr = 0x48; mfp_ier[1] = 0x60; mfp_imr[1] = 0x60; mfp_tcr[2] = 0x51; mfp_tdr[2] = 192; mfp_tcnt[2] = 192;
    mfp_tdr[3] = 2; mfp_tcnt[3] = 2;
    acia_ctrl = 0x96;
}

int main(int argc, char **argv)
{
    const char *prg = NULL; long nframes = 100; int every = 0; char every_prefix[256] = "";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir")) rootdir = argv[++i];
        else if (!strcmp(argv[i], "--frames")) nframes = atol(argv[++i]);
        else if (!strcmp(argv[i], "--trace-traps")) trace_traps = 1;
        else if (!strcmp(argv[i], "--ww")) sscanf(argv[++i], "%x:%x", &ww_lo, &ww_hi);
        else if (!strcmp(argv[i], "--wr")) sscanf(argv[++i], "%x:%x", &wr_lo, &wr_hi);
        else if (!strcmp(argv[i], "--disk")) { FILE *f = fopen(argv[++i], "rb"); fseek(f, 0, 2); disksize = ftell(f); fseek(f, 0, 0); diskimg = malloc(disksize); fread(diskimg, 1, disksize, f); fclose(f); }
        else if (!strcmp(argv[i], "--bp")) bp_addr = strtol(argv[++i], 0, 16);
        else if (!strcmp(argv[i], "--shots-every")) { char *s = argv[++i]; every = atoi(s); snprintf(every_prefix, sizeof every_prefix, "%s", strchr(s, ':') + 1); }
        else if (!strcmp(argv[i], "--pclog")) { char fn[256]; sscanf(argv[++i], "%ld:%ld:%255s", &pclog_f0, &pclog_f1, fn); pclog_f = fopen(fn, "w"); pc_counts = calloc(RAMSIZE / 2, 4); }
        else if (!strcmp(argv[i], "--shot") || !strcmp(argv[i], "--dump")) {
            struct ev *e = &evs[nev++]; e->type = argv[i][2] == 's' ? 0 : 1; sscanf(argv[++i], "%ld:%255s", &e->frame, e->file); }
        else if (!strcmp(argv[i], "--key")) { struct ev *e = &evs[nev++]; char ud[8]; e->type = 2; sscanf(argv[++i], "%ld:%x:%7s", &e->frame, &e->a, ud); e->b = ud[0] == 'd'; }
        else if (!strcmp(argv[i], "--joy")) { struct ev *e = &evs[nev++]; e->type = 3; sscanf(argv[++i], "%ld:%x", &e->frame, &e->a); }
        else prg = argv[i];
    }
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    init_machine();
    uint32_t bp = 0x10000;
    uint32_t entry = load_prg(prg, bp, 0);
    if (!entry) return 1;
    m68k_pulse_reset();
    /* user mode, usp at top of tpa */
    uint32_t usp = memtop - 4; wr32(usp, bp); usp -= 4; wr32(usp, STUB_EXIT);
    m68k_set_reg(M68K_REG_SR, 0x2300);
    m68k_set_reg(M68K_REG_A7, 0x7000);
    m68k_set_reg(M68K_REG_USP, usp);
    set_sr(0x0300);
    m68k_set_reg(M68K_REG_PC, entry);

    for (frame = 0; frame < nframes && !halted; frame++) {
        for (int i = 0; i < nev; i++) if (evs[i].frame == frame) {
            if (evs[i].type == 2) { kq_push(evs[i].b ? evs[i].a : (evs[i].a | 0x80)); }
            if (evs[i].type == 3) { joy_state[1] = evs[i].a; if (ikbd_joy_event) ikbd_send_joy(); }
        }
        for (line_in_frame = 0; line_in_frame < 313 && !halted; line_in_frame++) {
            if (line_in_frame == 0) { vid_latched = (vbase_hi << 16) | (vbase_mid << 8); irq_vbl = 1; update_irq(); }
            m68k_execute(512);
            mfp_run(512);
            if (line_in_frame >= 63 && line_in_frame < 263) mfp_event_tb();
            ikbd_tick();
            update_irq();
        }
        for (int i = 0; i < nev; i++) if (evs[i].frame == frame) {
            if (evs[i].type == 0) write_ppm(evs[i].file);
            if (evs[i].type == 1) { FILE *f = fopen(evs[i].file, "wb"); fwrite(ram, 1, RAMSIZE, f); fclose(f); }
        }
        if (every && frame % every == 0) { char fn[300]; snprintf(fn, sizeof fn, "%s%05ld.ppm", every_prefix, frame); write_ppm(fn); }
    }
    fprintf(stderr, "[emu] stopped at frame %ld pc=%06X sr=%04X\n", frame, m68k_get_reg(NULL, M68K_REG_PC), m68k_get_reg(NULL, M68K_REG_SR));
    if (pclog_f) {
        for (uint32_t i = 0; i < RAMSIZE / 2; i++) if (pc_counts[i]) fprintf(pclog_f, "%06X %u\n", i * 2, pc_counts[i]);
        fclose(pclog_f);
    }
    return 0;
}
