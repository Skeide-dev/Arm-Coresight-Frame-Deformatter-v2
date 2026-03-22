#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "csaccess.h"
#include "csregisters.h"
 
#define ZYNQ_ROM_TABLE  0xF8800000
#define ZYNQ_PTM0       0xF889C000
#define ZYNQ_PTM1       0xF889D000
#define ZYNQ_ETB        0xF8801000
#define ZYNQ_FUNNEL     0xF8804000
#define ZYNQ_TPIU       0xF8803000
 
#define PTM0_TRACE_ID   0x10
#define PTM1_TRACE_ID   0x11
 
int coresight_test1(int val);
 
static uint8_t gTraceBuffer[4096];
static int gTraceBufferLen;
 
static int CoreSightRegister(cs_device_t *pPTM0, cs_device_t *pPTM1, cs_device_t *pTPIU, cs_device_t *pETB)
{
    cs_device_t rep, ptm0, ptm1, etb, funnel, tpiu;
    int ret;
 
    ret = cs_init();
    if (ret)
        return ret;
    cs_diag_set(1);
 
    cs_register_romtable(ZYNQ_ROM_TABLE);
 
    ptm0   = cs_device_get(ZYNQ_PTM0);
    ptm1   = cs_device_get(ZYNQ_PTM1);
    etb    = cs_device_get(ZYNQ_ETB);
    funnel = cs_device_get(ZYNQ_FUNNEL);
    tpiu   = cs_device_get(ZYNQ_TPIU);
    rep = cs_atb_add_replicator(2);
 
    cs_device_set_affinity(ptm0, 0);
    cs_device_set_affinity(ptm1, 1);
 
    cs_atb_register(ptm0,   0, funnel, 0);
    cs_atb_register(ptm1,   0, funnel, 1);
    cs_atb_register(funnel, 0, rep,    0);
    cs_atb_register(rep, 0, etb,    0);
    cs_atb_register(rep, 1, tpiu,    0);
 
    cs_registration_complete();
 
    *pPTM0 = ptm0;
    *pPTM1 = ptm1;
    *pETB  = etb;
    *pTPIU = tpiu;
 
    return ret;
}
 
static int CoreSightInit(cs_device_t *pPTM0, cs_device_t *pPTM1, cs_device_t *pTPIU, cs_device_t *pETB)
{
    cs_device_t ptm0, ptm1, tpiu, etb;
    cs_etm_config_t config;
    int ret;
 
    ret = CoreSightRegister(&ptm0, &ptm1, &tpiu, &etb);
    if (ret)
        return ret;
 
    cs_disable_tpiu();
    /* disable under config*/
 
    cs_sink_disable(etb);
    cs_clear_trace_buffer(etb, 0);
    /* Empty the CoreSight trace buffer so we don't pick
       up residual trace from before this session. */
 
    /* Configure PTM0 */
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
 
static int CoreSightBeginTrace(cs_device_t ptm0, cs_device_t ptm1, cs_device_t tpiu, cs_device_t etb)
{
    cs_trace_enable(ptm0);
    cs_trace_enable(ptm1);
    cs_sink_enable(tpiu);
    cs_sink_enable(etb);
    return 0;
}
 
static int CoreSightStopTrace(cs_device_t ptm0, cs_device_t ptm1, cs_device_t tpiu, cs_device_t etb)
{
    int ret;
    int unread_bytes;
 
    
    ret = cs_trace_disable(ptm0);
    ret = cs_trace_disable(ptm1);
    cs_sink_disable(tpiu);
    ret = cs_sink_disable(etb);
 
    memset(gTraceBuffer, 0, sizeof(gTraceBuffer));
    unread_bytes = cs_get_buffer_unread_bytes(etb);
    ret = cs_get_trace_data(etb, gTraceBuffer, unread_bytes);
    gTraceBufferLen = unread_bytes;
 
    return ret;
}
 
int main(void)
{
    cs_device_t ptm0, ptm1, tpiu, etb;
    int ret;
    int val;
    FILE *f;
 
    printf("=== ZedBoard ETB & TPIU tracing session ===\n");
 
    ret = CoreSightInit(&ptm0, &ptm1, &tpiu, &etb);
    if (ret) {
        fprintf(stderr, "CoreSightInit failed: %d\n", ret);
        return ret;
    }
 
    CoreSightBeginTrace(ptm0, ptm1, tpiu, etb);
 
    /* Workload to trace */
    val = coresight_test1(10);
    printf("val: %d\n", val);
 
    CoreSightStopTrace(ptm0, ptm1, tpiu, etb);
 
    printf("ETB captured %d bytes of trace\n", gTraceBufferLen);
 
    if (gTraceBufferLen <= 0) {
        fprintf(stderr, "No trace data captured\n");
        cs_shutdown();
        return 1;
    }
 
    f = fopen("trace.bin", "wb");
    if (!f) {
        fprintf(stderr, "Failed to open trace.bin\n");
        cs_shutdown();
        return 1;
    }
 
    fwrite(gTraceBuffer, 1, gTraceBufferLen, f);
    fclose(f);
 
    printf("Trace saved to trace.bin\n");
 
    cs_shutdown();
    return 0;
}
