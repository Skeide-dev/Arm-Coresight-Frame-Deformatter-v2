#include <string.h>
#include "xil_printf.h"
#include "ff.h"
#include "csaccess.h"
#include "csregisters.h"
#include "xil_mmu.h"
#include "xil_cache.h"
#include "sleep.h"
#include "xil_io.h" 
#include "xaxidma.h"

#define ZYNQ_ROM_TABLE  0xF8800000
#define ZYNQ_PTM0       0xF889C000
#define ZYNQ_ETB        0xF8801000
#define ZYNQ_FUNNEL     0xF8804000
#define ZYNQ_TPIU       0xF8803000
#define PTM0_TRACE_ID   0x10

#define GPIO_BASE       0x41200000
#define DMA_DEVICE_ID           XPAR_AXI_DMA_0_BASEADDR // use device_id for vitis classic
#define DMA_TRANSFER_SIZE   1024              // Perform DMA-write of 1024 64-bit words

static XAxiDma dma_ctl;                     // AXI DMA driver instance
static XAxiDma_Config *dma_cfg;             // AXI DMA configuration parameters
int coresight_test1(int val);
static uint8_t gTraceBuffer[4096]; // ETB buffer
static int gTraceBufferLen;

static FATFS gFatFs;

static int CoreSightRegister(cs_device_t *pPTM0, cs_device_t *pETB, cs_device_t *pTPIU)
{
    cs_device_t ptm0, etb, funnel, tpiu, rep;
    int ret;

    ret = cs_init();
    if (ret)
        return ret;

    cs_diag_set(0);
    cs_register_romtable(ZYNQ_ROM_TABLE);

    ptm0   = cs_device_get(ZYNQ_PTM0);
    etb    = cs_device_get(ZYNQ_ETB);
    funnel = cs_device_get(ZYNQ_FUNNEL);
    tpiu   = cs_device_get(ZYNQ_TPIU);
    rep    = cs_atb_add_replicator(2);

    cs_device_set_affinity(ptm0, 0);
    cs_atb_register(ptm0,   0, funnel, 0);
    cs_atb_register(funnel, 0, rep,    0);
    cs_atb_register(rep,    0, etb,    0);
    cs_atb_register(rep,    1, tpiu,   0);
    cs_registration_complete();

    *pPTM0 = ptm0;
    *pETB  = etb;
    *pTPIU = tpiu;

    return ret;
}

static int CoreSightInit(cs_device_t *pPTM0, cs_device_t *pETB, cs_device_t *pTPIU)
{
    cs_device_t ptm0, etb, tpiu;
    cs_etm_config_t config;
    int ret;

    ret = CoreSightRegister(&ptm0, &etb, &tpiu);
    if (ret)
        return ret;



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
    cs_checkpoint();

    *pPTM0 = ptm0;
    *pETB  = etb;
    *pTPIU = tpiu;

    return 0;
}

static int CoreSightBeginTrace(cs_device_t ptm0, cs_device_t etb, cs_device_t tpiu)
{

    cs_sink_enable(etb);
    cs_sink_enable(tpiu);
    cs_trace_enable(ptm0);
    cs_checkpoint();

    return 0;
}



static int CoreSightStopTrace(cs_device_t ptm0, cs_device_t etb, cs_device_t tpiu)
{
    int ret;
    int unread_bytes;

    ret = cs_trace_disable(ptm0);
    cs_sink_disable(tpiu);
    ret = cs_sink_disable(etb);

    memset(gTraceBuffer, 0, sizeof(gTraceBuffer));
    unread_bytes = cs_get_buffer_unread_bytes(etb);
    ret = cs_get_trace_data(etb, gTraceBuffer, unread_bytes);
    gTraceBufferLen = unread_bytes;
    return ret;
}

static int MountSD(void)
{
    FRESULT res = f_mount(&gFatFs, "0:", 1);
    if (res != FR_OK) {
        xil_printf("f_mount failed: %d\r\n", res);
        return -1;
    }
    return 0;
}

static void UnmountSD(void)
{
    f_mount(NULL, "0:", 0);
}

static int SaveTraceToBin(const char *filename, const uint8_t *buf, int len)
{
    FIL     fil;
    FRESULT res;
    UINT    bw;

    Xil_DCacheFlush();

    res = f_open(&fil, filename, FA_CREATE_ALWAYS | FA_WRITE);
    if (res != FR_OK) {
        xil_printf("f_open failed: %d\r\n", res);
        return -1;
    }

    res = f_write(&fil, buf, (UINT)len, &bw);
    if (res != FR_OK || (int)bw != len) {
        xil_printf("f_write failed: res=%d written=%u\r\n", res, bw);
        f_close(&fil);
        return -1;
    }

    f_close(&fil);
    return 0;
}

int main(void)
{
    cs_device_t ptm0, etb, tpiu;
    int ret;
    int val;
    u64 buf_acq_to_dma[DMA_TRANSFER_SIZE];  // DMA buffer for acquired data
    s32 status;
    
    Xil_SetTlbAttributes(0xF8800000, 0x10C06);
    Xil_SetTlbAttributes(0x41200000, 0x10C06);
    Xil_DCacheInvalidateRange((UINTPTR)buf_acq_to_dma, DMA_TRANSFER_SIZE*8);
    xil_printf("=== ZedBoard ETB & TPIU tracing session ===\r\n");

    ret = CoreSightInit(&ptm0, &etb, &tpiu);
    if (ret) {
        xil_printf("CoreSightInit failed: %d\r\n", ret);
        return ret;
    }
    
    if (MountSD()) {
        cs_shutdown();
        return -1;
    }

    CoreSightBeginTrace(ptm0, etb, tpiu);

    val = coresight_test1(20);
    xil_printf("val: %d\r\n", val);

    CoreSightStopTrace(ptm0, etb, tpiu);

    xil_printf("ETB captured %d bytes of trace\r\n", gTraceBufferLen);

    // Initialize AXI DMA driver
    dma_cfg = XAxiDma_LookupConfig(DMA_DEVICE_ID);

    if (NULL == dma_cfg) {
        return XST_FAILURE;
    }
    status = XAxiDma_CfgInitialize(&dma_ctl, dma_cfg);
    if (status != XST_SUCCESS) {
        return XST_FAILURE;
    }
    // Disable DMA interrupt (use polling)
    XAxiDma_IntrDisable(&dma_ctl, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
        /* Data acquisition and DMA */

    // Enable AXI streaming data in Pl
        Xil_Out32(GPIO_BASE, 0x1);

    // Submit for DMA-write operation to move data to system memory
    status = XAxiDma_SimpleTransfer(&dma_ctl, (UINTPTR)buf_acq_to_dma, DMA_TRANSFER_SIZE*8, XAXIDMA_DEVICE_TO_DMA);
    if (status != XST_SUCCESS) {
        return XST_FAILURE;
    }

usleep(2);

while (XAxiDma_Busy(&dma_ctl, XAXIDMA_DEVICE_TO_DMA)); // This is where the DMA has issues
// worked fine a week ago..



    if (gTraceBufferLen <= 0) {
        xil_printf("No ETB trace data captured\r\n");
        UnmountSD();
        cs_shutdown();
        return 1;
    }

    ret = SaveTraceToBin("0:trace.bin", gTraceBuffer, gTraceBufferLen);
    if (ret) {
        xil_printf("Failed to save ETB trace to SD\r\n");
        UnmountSD();
        cs_shutdown();
        return 1;
    }

    xil_printf("ETB trace saved to trace.bin\r\n");
    xil_printf("First 5 frames of ETB trace:\r\n");
    for (int i = 0; i < 80; i++) {
        xil_printf("%02x ", gTraceBuffer[i]);
        if ((i + 1) % 16 == 0)
            xil_printf("\r\n");
    }

ret = SaveTraceToBin("0:tpiu1.bin", buf_acq_to_dma, DMA_TRANSFER_SIZE*8);
if (ret) {
    xil_printf("Failed to save DMA trace to SD\r\n");
    UnmountSD();
    cs_shutdown();
    return 1;
}
xil_printf("DMA trace saved to tpiu1.bin\r\n");
xil_printf("First 5 frames of DMA trace:\r\n");
for (int i = 0; i < 80; i++) {
    xil_printf("%02x ", buf_acq_to_dma[i]);
    if ((i + 1) % 16 == 0)
        xil_printf("\r\n");
}

    UnmountSD();
    cs_shutdown();
    return 0;
}