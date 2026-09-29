/* The looper's borrowed memory: see loop_mem.h. */
#include "cold.h"
#include "loop_mem.h"

COLD void loop_mem_take(loop_mem_t *m)
{
    if (m->owned) return;
    if (m->tail) cab_detach_tail(m->cab);
    looper_attach(m->lp, m->line, DELAY_LEN * sizeof m->line[0], m->tail,
                  m->tail ? sizeof *m->tail : 0u);
    m->owned = 1;
}

COLD void loop_mem_give(loop_mem_t *m)
{
    if (!m->owned) return;
    looper_detach(m->lp);
    delay_clear(m->dly);
    if (m->tail) cab_attach_tail(m->cab, m->tail);
    m->owned = 0;
}
