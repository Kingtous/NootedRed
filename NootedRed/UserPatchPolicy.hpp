// FairPlay 客户端识别：这些进程必须保持视频驱动原生代码。
#pragma once
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSBoolean.h>

namespace UserPatchPolicy
{
    inline bool isFairPlayClient(const task_t task)
    {
        if (task == nullptr) { return false; }
        auto* entitlement = IOUserClient::copyClientEntitlement(task, "com.apple.private.fpsd.client");
        const bool client = entitlement == kOSBooleanTrue;
        if (entitlement != nullptr) { entitlement->release(); }
        return client;
    }
}
