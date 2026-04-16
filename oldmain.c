#include <string.h>
#include "xil_printf.h"
#include "ff.h"
#include "csaccess.h"
#include "csregisters.h"
#include "dmaTPIU.h"
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>


#define ZYNQ_ROM_TABLE  0xF8800000
#define ZYNQ_PTM0       0xF889C000
#define ZYNQ_PTM1       0xF889D000
#define ZYNQ_ETB        0xF8801000
#define ZYNQ_FUNNEL     0xF8804000
#define ZYNQ_TPIU       0xF8803000
#define PTM0_TRACE_ID   0x10
#define PTM1_TRACE_ID   0x11

static FATFS fs;
static FIL etb_file;
static FIL   g_fil;
static UINT  g_bw;

static uint8_t gTraceBuffer[4096] __attribute__((aligned(64)));
static int     gTraceBufferLen;

int coresight_test1(int val);



static void sd_heap_snapshot(const char *filename)
{
    extern char _heap_start;
    extern char _heap_end;
    char *heap_bottom = &_heap_start;
    char *heap_ceil   = &_heap_end;
    char *heap_cur    = (char *)sbrk(0);
    uint32_t used  = (uint32_t)(heap_cur   - heap_bottom);
    uint32_t free  = (uint32_t)(heap_ceil  - heap_cur);
    uint32_t total = (uint32_t)(heap_ceil  - heap_bottom);
    char buf[192];
    int len = snprintf(buf, sizeof(buf),
                       "heap_start : 0x%08lX\n"
                       "heap_cur   : 0x%08lX\n"
                       "heap_end   : 0x%08lX\n"
                       "used bytes : %lu  (%lu KB)\n"
                       "free bytes : %lu  (%lu KB)\n"
                       "total bytes: %lu  (%lu KB)\n",
                       (unsigned long)heap_bottom,
                       (unsigned long)heap_cur,
                       (unsigned long)heap_ceil,
                       (unsigned long)used,  (unsigned long)(used  / 1024),
                       (unsigned long)free,  (unsigned long)(free  / 1024),
                       (unsigned long)total, (unsigned long)(total / 1024));
    FIL   fil;
    UINT  bw;
    FRESULT mret = f_mount(&fs, "0:/", 1);
    xil_printf("snapshot f_mount %s: %d\r\n", filename, mret);
    if (mret != FR_OK) return;
    FRESULT fret = f_open(&fil, filename, FA_CREATE_ALWAYS | FA_WRITE);
    xil_printf("snapshot f_open %s: %d\r\n", filename, fret);
    if (fret != FR_OK) {
        f_unmount("0:/");
        return;
    }
    f_write(&fil, buf, len, &bw);
    f_close(&fil);
    f_unmount("0:/");
}

static int CoreSightRegister(cs_device_t *pPTM0, cs_device_t *pPTM1,
                              cs_device_t *pTPIU, cs_device_t *pETB)
{
    cs_device_t rep, ptm0, ptm1, etb, funnel, tpiu;
    int ret;
extern struct global G;
    ret = cs_init();
    if (ret) return ret;

    cs_diag_set(0);


cs_register_romtable(ZYNQ_ROM_TABLE);

    ptm0   = cs_device_get(ZYNQ_PTM0);
    ptm1   = cs_device_get(ZYNQ_PTM1);

    etb    = cs_device_get(ZYNQ_ETB);

    funnel = cs_device_get(ZYNQ_FUNNEL);

    tpiu   = cs_device_get(ZYNQ_TPIU);

sd_heap_snapshot("0:/test.txt");
xil_printf("before rep fs addr:    0x%08X\r\n", (unsigned int)&fs);
xil_printf("before rep etb_file:   0x%08X\r\n", (unsigned int)&etb_file);
xil_printf("before rep gTraceBuf:  0x%08X\r\n", (unsigned int)gTraceBuffer);
xil_printf("before rep: sbrk=0x%08X\r\n", (unsigned int)sbrk(0));
xil_printf("G addr: 0x%08X\r\n", (unsigned int)&G);
rep = cs_atb_add_replicator(2);
xil_printf("G addr: 0x%08X\r\n", (unsigned int)&G);
xil_printf("after rep:  sbrk=0x%08X  rep=0x%08X\r\n", (unsigned int)sbrk(0), (unsigned int)rep);
xil_printf("fs addr:    0x%08X\r\n", (unsigned int)&fs);
xil_printf("etb_file:   0x%08X\r\n", (unsigned int)&etb_file);
xil_printf("gTraceBuf:  0x%08X\r\n", (unsigned int)gTraceBuffer);
    sd_heap_snapshot("0:/rep.txt");
    cs_device_set_affinity(ptm0, 0);
    cs_device_set_affinity(ptm1, 1);
    cs_atb_register(ptm0,   0, funnel, 0);
    cs_atb_register(ptm1,   0, funnel, 1);
    cs_atb_register(funnel, 0, rep,    0);
    cs_atb_register(rep,    0, etb,    0);
    cs_atb_register(rep,    1, tpiu,   0);
    cs_registration_complete();
    *pPTM0 = ptm0;
    *pPTM1 = ptm1;
    *pETB  = etb;
    *pTPIU = tpiu;
    return ret;
}

static int CoreSightInit(cs_device_t *pPTM0, cs_device_t *pPTM1,
                          cs_device_t *pTPIU, cs_device_t *pETB)
{
    cs_device_t ptm0, ptm1, tpiu, etb;
    cs_etm_config_t config;
    int ret;

    ret = CoreSightRegister(&ptm0, &ptm1, &tpiu, &etb);
    if (ret) return ret;

    cs_disable_tpiu();
    cs_sink_disable(etb);
    cs_clear_trace_buffer(etb, 0);

    cs_set_trace_source_id(ptm0, PTM0_TRACE_ID);
    cs_etm_clean(ptm0);
    cs_etm_config_init(&config);
    config.flags = CS_ETMC_CONFIG;
    cs_etm_config_get(ptm0, &config);
    config.cr.raw.c.cxid_size = 1;
    config.flags |= CS_ETMC_TRACE_ENABLE;
    config.trace_enable_event = CS_ETME_WHEN(CS_ETMER_ALWAYS);
    config.trace_enable_cr1   = CS_ETMTECR1_EXCLUDE;
    config.trace_enable_cr2   = 0x00000000;
    cs_etm_config_put(ptm0, &config);


    /* Configure PTM1 */
    cs_set_trace_source_id(ptm1, PTM1_TRACE_ID);
    cs_etm_clean(ptm1);
    cs_etm_config_init(&config);
    config.flags = CS_ETMC_CONFIG;
    cs_etm_config_get(ptm1, &config);
    config.cr.raw.c.cxid_size = 1;
    config.flags |= CS_ETMC_TRACE_ENABLE;
    config.trace_enable_event = CS_ETME_WHEN(CS_ETMER_ALWAYS);
    config.trace_enable_cr1   = CS_ETMTECR1_EXCLUDE;
    config.trace_enable_cr2   = 0x00000000;
    cs_etm_config_put(ptm1, &config);

    *pPTM0 = ptm0;
    *pPTM1 = ptm1;
    *pETB  = etb;
    *pTPIU = tpiu;
    return 0;
}

static int CoreSightBeginTrace(cs_device_t ptm0, cs_device_t ptm1,
                                cs_device_t tpiu, cs_device_t etb)
{
    cs_trace_enable(ptm0);
    cs_trace_enable(ptm1);
    cs_sink_enable(tpiu);
    cs_sink_enable(etb);
    return 0;
}

static int CoreSightStopTrace(cs_device_t ptm0, cs_device_t ptm1,
                               cs_device_t tpiu, cs_device_t etb)
{
    int ret;
    int unread_bytes;
    ret = cs_trace_disable(ptm0);
    ret = cs_trace_disable(ptm1);
    cs_sink_disable(tpiu);
    ret = cs_sink_disable(etb);

    memset(gTraceBuffer, 0, sizeof(gTraceBuffer));

    unread_bytes = cs_get_buffer_unread_bytes(etb);
    xil_printf("unread_bytes = %d\r\n", unread_bytes);

    if (unread_bytes > (int)sizeof(gTraceBuffer))
        unread_bytes = sizeof(gTraceBuffer);
cs_get_trace_data(etb, gTraceBuffer, unread_bytes);
    gTraceBufferLen = unread_bytes;

    return ret;
}

int main(void)
{
    cs_device_t ptm0, ptm1, tpiu, etb;
    int ret, val;
    const char test_data[] = "SD card OK\n";

   // sd_checkpoint("0:/test.bin");

    xil_printf("=== ZedBoard ETB & TPIU tracing session ===\r\n");
    ret = CoreSightInit(&ptm0, &ptm1, &tpiu, &etb);
    if (ret) { xil_printf("CoreSightInit failed: %d\r\n", ret); return ret; }
sd_heap_snapshot("0:/heap_after_init.txt");
   // sd_checkpoint("0:/init.bin");
    CoreSightBeginTrace(ptm0, ptm1, tpiu, etb);
sd_heap_snapshot("0:/heap_after_begin.txt");
    //sd_checkpoint("0:/begin.bin");
    val = coresight_test1(10);
    xil_printf("val: %d\r\n", val);

    CoreSightStopTrace(ptm0, ptm1, tpiu, etb);
    sd_heap_snapshot("0:/heap_after_stop.txt");
    //sd_checkpoint("0:/stop.bin");

    xil_printf("ETB captured %d bytes\r\n", gTraceBufferLen);
    if (gTraceBufferLen <= 0) {
        xil_printf("No trace data\r\n");
        cs_shutdown();
        return 1;
    }

// Mount SD only now, all CSAL malloc is done

//Xil_DCacheFlush();

volatile int delay;
for (delay = 0; delay < 100000000; delay++);

if (f_mount(&fs, "0:", 1) != FR_OK) {
    xil_printf("f_mount failed\r\n");
    return 1;
}
xil_printf("mounted\r\n");

// Check what's on the card
DIR dir;
FILINFO fno;
FRESULT fr;

fr = f_opendir(&dir, "0:");
xil_printf("f_opendir result: %d\r\n", fr);
if (fr == FR_OK) {
    while (1) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == 0) break;
        xil_printf("found: %s\r\n", fno.fname);
    }
    f_closedir(&dir);
}

fr = f_open(&etb_file, "0:/trace.bin", FA_CREATE_ALWAYS | FA_WRITE);
xil_printf("f_open result: %d\r\n", fr);
if (fr != FR_OK) {
    xil_printf("Failed to open etb_trace.bin\r\n");
    return 1;
}
f_write(&etb_file, gTraceBuffer, gTraceBufferLen, &g_bw);
f_close(&etb_file);
f_unmount("0:/");

xil_printf("ETB trace saved: %u bytes\r\n", g_bw);
    cs_shutdown();
    return 0;
}
