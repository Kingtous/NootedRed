"""Run the actual VCN teardown drain against deterministic idle/timeout stubs."""
from pathlib import Path
import os
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    text = (ROOT / 'NootedRed/VCNBridge.cpp').read_text(encoding='utf-8')
    start = text.index('void VCNBridge::drainVCN(')
    end = text.index('\nvoid VCNBridge::signalWork(', start)
    body = text[start:end]
    constants = '\n'.join(re.findall(r'constexpr UInt32 kDrain\w+\s*=\s*\d+;', text))
    assert 'noopEnableGfxOff' not in text and 'holdGfxOff' not in text
    assert body.index('vcnIsIdle') < body.index('vcnWaitForIdle')
    program = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
using UInt32=uint32_t;using UInt64=uint64_t;
#include <VCNDrain.hpp>
CONSTANTS
static unsigned slept=0,queries=0,readyAfter=0,fallbackCalls=0;
static bool wedged=false;
static unsigned gfxDepth=0;
static void disable(void*){assert(gfxDepth==0);gfxDepth++;}
static void enable(void*){assert(gfxDepth==1);gfxDepth--;}
static void clock_get_uptime(UInt64* p){*p=UInt64(slept)*1000000ULL;}
static void absolutetime_to_nanoseconds(UInt64 a,UInt64* p){*p=a;}
static void IOSleep(UInt32 n){slept+=n;}
static bool query(void*){assert(gfxDepth==1);queries++;return !wedged && slept>=readyAfter;}
static void fallback(void*,UInt32 n){assert(n==kDrainTimeoutMs);assert(gfxDepth==0);slept+=n;fallbackCalls++;}
static std::map<std::string,UInt32> props;
struct NRed {static NRed& singleton(){static NRed n;return n;}void setProp32(const char* key,UInt32 v){assert(gfxDepth==0);props[key]=v;}};
#define SYSLOG(...) ((void)0)
struct VCNBridge {
 void* hardwareHandle=reinterpret_cast<void*>(2);
 uintptr_t drainDisableGfxOff=reinterpret_cast<uintptr_t>(disable),drainEnableGfxOff=reinterpret_cast<uintptr_t>(enable);
 bool enabled=true,noDrain=false;void* vcnEngine=reinterpret_cast<void*>(1);
 uintptr_t vcnIsIdle=reinterpret_cast<uintptr_t>(query),vcnWaitForIdle=reinterpret_cast<uintptr_t>(fallback);
 UInt32 drainCount=0,drainTimeouts=0,drainUnverified=0,drainUnavailable=0,lastWaitMs=0,maxWaitMs=0;
 static VCNBridge& singleton(){static VCNBridge b;return b;}
 bool engineEnabled(){return enabled;}
 void drainVCN(const char*);
};
BODY
static void reset(){assert(gfxDepth==0);VCNBridge::singleton()=VCNBridge{};props.clear();slept=queries=readyAfter=fallbackCalls=0;wedged=false;}
int main(){
 auto& b=VCNBridge::singleton();
 reset();b.drainVCN("idle");assert(slept==0&&b.drainCount==1&&b.drainTimeouts==0);
 reset();readyAfter=15;b.drainVCN("delayed");assert(slept==15&&b.lastWaitMs==15&&b.drainTimeouts==0);
 reset();wedged=true;b.drainVCN("timeout");assert(slept==kDrainTimeoutMs&&b.drainTimeouts==1);
 slept=0;b.drainVCN("timeout again");assert(slept==kDrainTimeoutMs&&b.drainCount==2&&b.drainTimeouts==2);
 for(int mode=0;mode<3;mode++){
  reset();if(mode==0)b.enabled=false;if(mode==1)b.vcnEngine=nullptr;if(mode==2)b.noDrain=true;
  b.drainVCN("disabled");assert(queries==0&&slept==0&&b.drainCount==0);
 }
 reset();b.vcnIsIdle=0;b.drainVCN("fallback");assert(fallbackCalls==1&&b.lastWaitMs==kDrainTimeoutMs&&b.drainUnverified==1&&b.drainTimeouts==0);
 reset();b.vcnIsIdle=b.vcnWaitForIdle=0;b.drainVCN("no query");assert(b.drainCount==1&&b.drainUnavailable==1);
 assert(gfxDepth==0);
 puts("PASS production drain: idle/delayed/repeated timeout, guards and fallback; no GPU accessed");
}
'''.replace('CONSTANTS', constants).replace('BODY', body)
    work = ROOT / 'build/wake-tests'
    work.mkdir(parents=True, exist_ok=True)
    stubs = work / 'stubs/IOKit'
    stubs.mkdir(parents=True, exist_ok=True)
    (stubs / 'IOTypes.h').write_text('#pragma once\n#include <cstdint>\nusing UInt32=uint32_t;using UInt64=uint64_t;\n')
    src = work / 'test-drain.cpp'
    src.write_text(program, encoding='utf-8')
    exe = work / ('test-drain.exe' if os.name == 'nt' else 'test-drain')
    compiler = os.environ.get('CXX') or shutil.which('clang++')
    assert compiler, 'clang++ required'
    subprocess.run([compiler, '-std=c++20', '-O2', '-I'+str(work/'stubs'), '-I'+str(ROOT/'NootedRed'), str(src), '-o', str(exe)], check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == '__main__':
    main()
