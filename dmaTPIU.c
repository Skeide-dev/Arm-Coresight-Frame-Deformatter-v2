#include "xaxidma.h"
#include "xscugic.h"
#include "ff.h"
#include "xil_cache.h"
#include "xparameters.h"
#include <stdio.h>

#define DMA_DEVICE_ID    0
#define INTC_DEVICE_ID   XPAR_SCUGIC_SINGLE_DEVICE_ID
#define DMA_S2MM_IRQ_ID  (XPAR_FABRIC_XAXIDMA_0_INTR + 32)
#define BUFFER_BYTES     (5500 * 8)
#define OUTPUT_FILE      "0:/capture.bin"

static u8 buf_a[BUFFER_BYTES] __attribute__((aligned(64)));
static u8 buf_b[BUFFER_BYTES] __attribute__((aligned(64)));
static u8 *dma_buf   = buf_a;
static u8 *write_buf = buf_b;

static volatile int dma_done  = 0;
static volatile int dma_error = 0;
static volatile int dma_running = 0;

static XAxiDma  dma;
static XScuGic  gic;
static FATFS    fs;
static FIL      fil;

void dma_s2mm_handler(void *cb)
{
    (void)cb;
    u32 status = XAxiDma_IntrGetIrq(&dma, XAXIDMA_DEVICE_TO_DMA);
    XAxiDma_IntrAckIrq(&dma, status, XAXIDMA_DEVICE_TO_DMA);
    if (status & XAXIDMA_IRQ_ERROR_MASK) { dma_error = 1; return; }
    if (status & XAXIDMA_IRQ_IOC_MASK)   { dma_done  = 1; }
}

static int start_dma(u8 *dst)
{
    Xil_DCacheInvalidateRange((UINTPTR)dst, BUFFER_BYTES);
    return XAxiDma_SimpleTransfer(&dma,
                (UINTPTR)dst,
                BUFFER_BYTES,
                XAXIDMA_DEVICE_TO_DMA);
}

/* ── Init: call once at startup ─────────────────────────── */
int dmaTPIU_init(void)
{
    XAxiDma_Config *cfg;
    XScuGic_Config *gic_cfg;
    FRESULT rc;

    cfg = XAxiDma_LookupConfig(DMA_DEVICE_ID);
    if (!cfg) { xil_printf("DMA config not found\r\n"); return -1; }
    XAxiDma_CfgInitialize(&dma, cfg);
    XAxiDma_IntrEnable(&dma,
        (XAXIDMA_IRQ_IOC_MASK | XAXIDMA_IRQ_ERROR_MASK),
        XAXIDMA_DEVICE_TO_DMA);

    gic_cfg = XScuGic_LookupConfig(INTC_DEVICE_ID);
    XScuGic_CfgInitialize(&gic, gic_cfg, gic_cfg->CpuBaseAddress);
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
        (Xil_ExceptionHandler)XScuGic_InterruptHandler, &gic);
    XScuGic_Connect(&gic, DMA_S2MM_IRQ_ID, dma_s2mm_handler, NULL);
    XScuGic_Enable(&gic, DMA_S2MM_IRQ_ID);
    Xil_ExceptionEnable();

    rc = f_mount(&fs, "", 1);
    if (rc) { xil_printf("SD mount failed %d\r\n", rc); return -1; }

    return 0;
}

/* ── Start: open file and begin capturing ───────────────── */
int dmaTPIU_start(void)
{
    FRESULT rc = f_open(&fil, OUTPUT_FILE, FA_CREATE_ALWAYS | FA_WRITE);
    if (rc) { xil_printf("File open failed %d\r\n", rc); return -1; }

    dma_done    = 0;
    dma_error   = 0;
    dma_running = 1;
    dma_buf     = buf_a;
    write_buf   = buf_b;

    xil_printf("DMA capture started\r\n");
    start_dma(dma_buf);
    return 0;
}

/* ── Stop: wait for current transfer, flush to SD ──────── */
int dmaTPIU_stop(void)
{
    UINT bw;
    FRESULT rc;

    if (!dma_running) return 0;
    dma_running = 0;

    /* Wait for the current in-flight transfer to complete */
    u32 timeout = 10000000;
    while (!dma_done && !dma_error && timeout > 0) {
        timeout--;
        u32 status = XAxiDma_IntrGetIrq(&dma, XAXIDMA_DEVICE_TO_DMA);
        if (status & XAXIDMA_IRQ_IOC_MASK) {
            XAxiDma_IntrAckIrq(&dma, status, XAXIDMA_DEVICE_TO_DMA);
            dma_done = 1;
        }
        if (status & XAXIDMA_IRQ_ERROR_MASK) {
            dma_error = 1;
        }
    }

    if (timeout == 0) xil_printf("DMA stop timeout!\r\n");
    if (dma_error)    xil_printf("DMA error on stop\r\n");

    /* Write whatever was captured to SD */
    rc = f_write(&fil, dma_buf, BUFFER_BYTES, &bw);
    if (rc || bw != (UINT)BUFFER_BYTES)
        xil_printf("Write err %d wrote %u\r\n", rc, bw);

    f_close(&fil);
    f_unmount("");
    xil_printf("DMA capture stopped. Bytes written: %u\r\n", bw);
    return 0;
}
