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
using UInt32=uint32_t;
CONSTANTS
static unsigned slept=0,queries=0,readyAfter=0,fallbackCalls=0;
static bool wedged=false;
static void IOSleep(UInt32 n){slept+=n;}
static bool query(void*){queries++;return !wedged && slept>=readyAfter;}
static void fallback(void*,UInt32 n){assert(n==kDrainTimeoutMs);fallbackCalls++;}
static std::map<std::string,UInt32> props;
struct NRed {static NRed& singleton(){static NRed n;return n;}void setProp32(const char* key,UInt32 v){props[key]=v;}};
#define SYSLOG(...) ((void)0)
struct VCNBridge {
 bool enabled=true,noDrain=false;void* vcnEngine=reinterpret_cast<void*>(1);
 uintptr_t vcnIsIdle=reinterpret_cast<uintptr_t>(query),vcnWaitForIdle=reinterpret_cast<uintptr_t>(fallback);
 UInt32 drainCount=0,drainTimeouts=0,lastWaitMs=0,maxWaitMs=0;
 static VCNBridge& singleton(){static VCNBridge b;return b;}
 bool engineEnabled(){return enabled;}
 void drainVCN(const char*);
};
BODY
static void reset(){VCNBridge::singleton()=VCNBridge{};props.clear();slept=queries=readyAfter=fallbackCalls=0;wedged=false;}
int main(){
 auto& b=VCNBridge::singleton();
 reset();b.drainVCN("idle");assert(slept==0&&b.drainCount==1&&b.drainTimeouts==0);
 reset();readyAfter=15;b.drainVCN("delayed");assert(slept==15&&b.lastWaitMs==15&&b.drainTimeouts==0);
 reset();wedged=true;b.drainVCN("timeout");assert(slept<=kDrainTimeoutMs+kDrainStepMs&&b.drainTimeouts==1);
 slept=0;b.drainVCN("timeout again");assert(slept<=kDrainTimeoutMs+kDrainStepMs&&b.drainCount==2&&b.drainTimeouts==2);
 for(int mode=0;mode<3;mode++){
  reset();if(mode==0)b.enabled=false;if(mode==1)b.vcnEngine=nullptr;if(mode==2)b.noDrain=true;
  b.drainVCN("disabled");assert(queries==0&&slept==0&&b.drainCount==0);
 }
 reset();b.vcnIsIdle=0;b.drainVCN("fallback");assert(fallbackCalls==1&&b.lastWaitMs==kDrainTimeoutMs);
 reset();b.vcnIsIdle=b.vcnWaitForIdle=0;b.drainVCN("no query");assert(b.drainCount==0);
 puts("PASS production drain: idle/delayed/repeated timeout, guards and fallback; no GPU accessed");
}
'''.replace('CONSTANTS', constants).replace('BODY', body)
    work = ROOT / 'build/wake-tests'
    work.mkdir(parents=True, exist_ok=True)
    src = work / 'test-drain.cpp'
    src.write_text(program, encoding='utf-8')
    exe = work / ('test-drain.exe' if os.name == 'nt' else 'test-drain')
    compiler = os.environ.get('CXX') or shutil.which('clang++')
    assert compiler, 'clang++ required'
    subprocess.run([compiler, '-std=c++20', '-O2', str(src), '-o', str(exe)], check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == '__main__':
    main()
