#include "ui/pads.h"
#include "fsl_iomuxc.h"

void pad_set(uint32_t pad, uint32_t mux_mode, uint32_t pad_cfg)
{
    uint32_t mux = 0x401F8014u + 4u * pad, cfg = 0x401F8204u + 4u * pad;
    IOMUXC_SetPinMux(mux, mux_mode, 0u, 0u, cfg, 0u);
    IOMUXC_SetPinConfig(mux, mux_mode, 0u, 0u, cfg, pad_cfg);
}
