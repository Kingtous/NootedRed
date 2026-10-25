"""Exercise production idle policy with power gating and scheduler-delay stubs."""
from pathlib import Path
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    work = ROOT / 'build/drain-guard-tests'
    stub = work / 'stubs/IOKit'
    stub.mkdir(parents=True, exist_ok=True)
    (stub / 'IOTypes.h').write_text('#pragma once\n#include <cstdint>\nusing UInt32=uint32_t;using UInt64=uint64_t;\n')
    source = r'''
#include <VCNDrain.hpp>
#include <cassert>
#include <cstdio>
static UInt64 ns=0;static unsigned depth=0,downs=0,ups=0,queries=0,waits=0;
static UInt32 oversleep=1,readyAt=0,queryMs=0,setupMs=0;
static bool busy=false;
static void down(void*){assert(depth==0);depth++;downs++;ns+=UInt64(setupMs)*1000000;}
static void up(void*){assert(depth==1);depth--;ups++;}
static bool idle(void*){assert(depth==1);queries++;ns+=UInt64(queryMs)*1000000;return !busy&&ns>=UInt64(readyAt)*1000000;}
static UInt64 clockNs(){return ns;}
static void sleepMs(UInt32 delay){assert(depth==1);ns+=UInt64(delay)*oversleep*1000000;}
static void native(void*,UInt32){assert(depth==0);waits++;}
static void reset(){assert(depth==0);ns=0;downs=ups=queries=waits=0;oversleep=1;readyAt=queryMs=setupMs=0;busy=false;}
static auto run(VCNDrain::Query q=idle,VCNDrain::GfxControl d=down,VCNDrain::GfxControl u=up){
 auto r=VCNDrain::wait(reinterpret_cast<void*>(1),reinterpret_cast<void*>(2),q,d,u,native,clockNs,sleepMs,250,5);
 assert(depth==0&&downs==ups);return r;
}
int main(){
 using O=VCNDrain::Outcome;
 reset();auto r=run();assert(r.outcome==O::Idle&&downs==1&&ups==1&&r.elapsedMs==0);
 reset();readyAt=15;r=run();assert(r.outcome==O::Idle&&r.elapsedMs==15&&r.requestedSleepMs==15);
 reset();busy=true;r=run();assert(r.outcome==O::TimedOut&&r.elapsedMs==250&&r.requestedSleepMs==250);
 reset();busy=true;oversleep=10;r=run();assert(r.outcome==O::TimedOut&&r.elapsedMs==250&&r.requestedSleepMs==25);
 reset();setupMs=300;r=run();assert(r.outcome==O::TimedOut&&queries==0&&r.elapsedMs==300);
 reset();busy=true;queryMs=300;r=run();assert(r.outcome==O::TimedOut&&queries==1&&r.requestedSleepMs==0);
 reset();r=run(nullptr);assert(r.outcome==O::NativeUnverified&&downs==0&&queries==0&&waits==1);
 reset();r=run(idle,nullptr);assert(r.outcome==O::NativeUnverified&&downs==0&&queries==0);
 reset();r=run(idle,down,nullptr);assert(r.outcome==O::NativeUnverified&&downs==0&&queries==0);
 reset();r=VCNDrain::wait(reinterpret_cast<void*>(1),nullptr,idle,down,up,nullptr,clockNs,sleepMs,250,5);
 assert(r.outcome==O::Unavailable&&downs==0&&queries==0);
 reset();r=VCNDrain::wait(nullptr,nullptr,idle,down,up,native,clockNs,sleepMs,250,5);
 assert(r.outcome==O::Unavailable&&downs==0&&queries==0);
 puts("PASS production scoped GFXOFF pairing, monotonic deadline, scheduler oversleep and unverified fallback");
}
'''
    src = work / 'test.cpp'
    src.write_text(source, encoding='utf-8')
    exe = work / ('test.exe' if os.name == 'nt' else 'test')
    compiler = os.environ.get('CXX') or shutil.which('clang++')
    assert compiler, 'clang++ required'
    subprocess.run([compiler, '-std=c++20', '-O2', '-I'+str(work/'stubs'), '-I'+str(ROOT/'NootedRed'),
                    str(src), '-o', str(exe)], check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == '__main__':
    main()
