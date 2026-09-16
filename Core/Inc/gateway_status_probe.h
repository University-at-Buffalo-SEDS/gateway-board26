#ifndef GATEWAY_STATUS_PROBE_H
#define GATEWAY_STATUS_PROBE_H
#include <stdint.h>
#include <stddef.h>
/* Passive classification only. Never changes routing or validates delivery.
 * Compact frames without a retained template are explicitly counted unknown. */
typedef struct { uint32_t id, ty, source; } gateway_probe_template;
#define GATEWAY_STATUS_TEMPLATE_CAPACITY 8U
typedef struct {
  uint32_t frames, unknown, status, valve_status, actuator_status, last_tick;
} gateway_status_probe;
extern volatile gateway_status_probe g_gateway_status_path[4];
extern gateway_probe_template g_gateway_status_templates[2][GATEWAY_STATUS_TEMPLATE_CAPACITY];
extern uint32_t g_gateway_status_next[2];
enum { GW_STATUS_CAN_RX, GW_STATUS_UART_QUEUED, GW_STATUS_UART_SENT, GW_STATUS_UART_FAILED };
static inline int gateway_probe_uleb(const uint8_t *p, size_t n, size_t *i, uint64_t *v) {
  *v=0;
  for (unsigned s=0;s<64 && *i<n;s+=7) {
    uint8_t b=p[(*i)++];
    if (s==63 && (b & 0xfe)) return 0;
    *v|=(uint64_t)(b&127)<<s;
    if (!(b&128)) return 1;
  }
  return 0;
}
static inline void gateway_status_observe(unsigned stage, const uint8_t *p,
                                         size_t n, uint32_t status_type, uint32_t tick) {
  if(stage>=4) return;
  volatile gateway_status_probe *d=&g_gateway_status_path[stage];
  unsigned channel=stage==0 ? 0 : 1;
  gateway_probe_template *templates=g_gateway_status_templates[channel];
  uint64_t id=0, ty=0, source=0, ignored;
  size_t i=0;
  int full=0;
  d->frames++;
  if(!p || n<3) goto unknown;
  if(p[0]==83 && p[1]==68 && p[2]==84) {
    if(n<5) goto unknown;
    unsigned kind=p[3]; i=4;
    /* Compact transport has a flags byte before its template ID. */
    if(kind==2 || kind==4 || kind==5) i++;
    if(!gateway_probe_uleb(p,n,&i,&id) || id>UINT32_MAX) goto unknown;
    if(kind==2 || kind==4 || kind==5) {
      for(unsigned k=0;k<GATEWAY_STATUS_TEMPLATE_CAPACITY;k++) {
        if(templates[k].id==id && templates[k].ty) {
          ty=templates[k].ty; source=templates[k].source;
          goto classified;
        }
      }
      goto unknown;
    }
    if(kind!=1) goto unknown;
    full=1;
  }
  if(n-i<3) goto unknown;
  uint8_t flags=p[i]; i+=2;
  if(!gateway_probe_uleb(p,n,&i,&ty) ||
     !gateway_probe_uleb(p,n,&i,&ignored) ||
     !gateway_probe_uleb(p,n,&i,&ignored)) goto unknown;
  if((flags&8) && !gateway_probe_uleb(p,n,&i,&ignored)) goto unknown;
  if(!gateway_probe_uleb(p,n,&i,&source) || source>UINT32_MAX || ty>UINT32_MAX) goto unknown;
  if(full) {
    unsigned k;
    for(k=0;k<GATEWAY_STATUS_TEMPLATE_CAPACITY;k++) if(templates[k].id==id && templates[k].ty) break;
    if(k==GATEWAY_STATUS_TEMPLATE_CAPACITY) {k=g_gateway_status_next[channel]; g_gateway_status_next[channel]=(k+1)%GATEWAY_STATUS_TEMPLATE_CAPACITY;}
    templates[k].id=(uint32_t)id;
    templates[k].ty=(uint32_t)ty;
    templates[k].source=(uint32_t)source;
  }
classified:
  if(ty==status_type) {
    d->status++; d->last_tick=tick;
    if(source==3852079518U) d->valve_status++;
    if(source==483403151U) d->actuator_status++;
  }
  return;
unknown:
  d->unknown++;
}
#endif
