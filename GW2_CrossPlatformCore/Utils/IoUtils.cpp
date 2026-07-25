#include "pch.h"
#include "IoCore.h"
#include "CoreGlobal.h"
#include "IoUtils.h"

void IoUtils::Init()
{

}

void IoUtils::Clear()
{
    if (GIoCore)
    {
        GIoCore->Stop();
        GIoCore = nullptr;
    }
}