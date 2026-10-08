from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT=Path(__file__).resolve().parents[1]
class PacketArenaTests(unittest.TestCase):
    def test_arena_failure_retry_and_router_recreation(self):
        stub=r'''#pragma once
#include <stdint.h>
typedef int SedsResult;
#define SEDS_OK 0
#define SEDS_IO -14
static unsigned calls;
static int result;
static int seds_packet_store_configure(unsigned bytes,unsigned handles,unsigned limit)
{ calls++; if(bytes!=4096 || handles!=32 || limit!=512) return -1; return result; }
'''
        body=r'''#include "board_packet_store.h"
#include <assert.h>
int main(void) {
#ifdef SEDS_ENABLE_COMPACT_PACKET_STORE
 result=-14; assert(board_packet_store_init()==-14 && calls==1);
 result=0; assert(board_packet_store_init()==0 && calls==2);
 for(unsigned i=0;i<20;i++) assert(board_packet_store_init()==0);
 assert(calls==2 && g_board_packet_store_init_result==0);
#else
 assert(board_packet_store_init()==0 && calls==0);
 (void)seds_packet_store_configure; (void)result;
#endif
}
'''
        for enabled in (False,True):
            with tempfile.TemporaryDirectory() as directory:
                d=Path(directory)
                (d/'sedsnet_config.h').write_text(stub)
                (d/'board_packet_store.h').write_text((ROOT/'Core/Inc/board_packet_store.h').read_text())
                cmd=['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-DBOARD_PACKET_ARENA_BYTES=4096','-DBOARD_PACKET_ARENA_HANDLES=32','-I',str(d)]
                if enabled: cmd+=['-DSEDS_ENABLE_COMPACT_PACKET_STORE=1']
                subprocess.run(cmd+['-x','c','-','-o',str(d/'test')],input=body,text=True,check=True)
                subprocess.run([str(d/'test')],check=True,timeout=5)

    def test_arena_initialization_precedes_router(self):
        source=(ROOT/'Core/Src/telemetry.c').read_text()
        self.assertLess(source.index('(void)board_packet_store_init();'),source.index('r = seds_router_new'))
        self.assertIn('if (g_gateway_packet_store_init_result != SEDS_OK)',source)

    def test_gateway_arena_leaves_discovery_headroom(self):
        source=(ROOT/'Core/Src/telemetry.c').read_text()
        self.assertIn('#define GATEWAY_PACKET_ARENA_BYTES 2048U',source)
        self.assertIn('#define GATEWAY_PACKET_ARENA_HANDLES 16U',source)
