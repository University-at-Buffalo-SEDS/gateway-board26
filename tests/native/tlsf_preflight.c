#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include "tlsf.h"
static uint64_t arena[8192];
static uint64_t hash;
static size_t walked_free;
static void digest(void *p,size_t n,int used,void *arg) {
 (void)arg; if(!used)walked_free+=n; hash=hash*131U+(uintptr_t)p+n+(unsigned)used;
}
static uint64_t snapshot(tlsf_t t) { hash=0;walked_free=0;tlsf_walk_pool(tlsf_get_pool(t),digest,0);assert(tlsf_free_size(t)==walked_free);return hash; }
int main(void) {
 tlsf_t t=tlsf_create_with_pool(arena,sizeof(arena));assert(t);
 assert(!tlsf_can_memalign(t,0,8));assert(!tlsf_can_memalign(t,3,8));
 assert(!tlsf_can_memalign(t,8,SIZE_MAX));assert(!tlsf_can_memalign(t,SIZE_MAX,8));
 void *slots[64]={0};uint32_t rng=1;
 for(unsigned i=0;i<6000;i++) {
  rng=rng*1664525U+1013904223U;unsigned slot=(rng>>16)%64;
  if(slots[slot]){tlsf_free(t,slots[slot]);slots[slot]=0;}
  else slots[slot]=tlsf_memalign(t,8,1+rng%4096);
  size_t align=(size_t)1<<(3+(rng%6)),n=(rng>>8)%8192;
  uint64_t before=snapshot(t);
  int predicted=tlsf_can_memalign(t,align,n);
  assert(snapshot(t)==before && tlsf_check(t)==0);
  void *p=tlsf_memalign(t,align,n);
  assert(predicted==(p!=0));
  if(p) assert(((uintptr_t)p&(align-1))==0);
  tlsf_free(t,p);assert(tlsf_check(t)==0);
 }
 for(unsigned i=0;i<64;i++)tlsf_free(t,slots[i]);
 assert(tlsf_can_memalign(t,8,32768));
 snapshot(t);size_t baseline=tlsf_free_size(t);
 void *r=tlsf_malloc(t,1024);assert(r);snapshot(t);
 r=tlsf_realloc(t,r,2048);assert(r);snapshot(t);
 r=tlsf_realloc(t,r,32);assert(r);snapshot(t);
 tlsf_free(t,r);snapshot(t);assert(tlsf_free_size(t)==baseline);
 static uint64_t extra[1024];
 pool_t pool=tlsf_add_pool(t,extra,sizeof(extra));assert(pool);
 walked_free=0;tlsf_walk_pool(tlsf_get_pool(t),digest,0);
 tlsf_walk_pool(pool,digest,0);assert(tlsf_free_size(t)==walked_free);
 tlsf_remove_pool(t,pool);snapshot(t);assert(tlsf_free_size(t)==baseline);
}
