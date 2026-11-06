#include "cefmedia_api.h"

#include "cefmedia_ipc_client.h"

CefMediaApi& CefMediaApi::Instance()
{
    static CefMediaApi instance;
    return instance;
}

bool CefMediaApi::Load()
{
    return true; // libipc is linked in; there is no external DLL to load
}

void CefMediaApi::SetVideoFrameCallback(VideoFrameCallback callback, void* obj, int channel)
{
    CefMediaIpcClient::Instance().SetVideoFrameCallback(callback, obj, channel);
}

void CefMediaApi::SetAudioFrameCallback(AudioBufferCallback callback, void* obj, int channel)
{
    CefMediaIpcClient::Instance().SetAudioFrameCallback(callback, obj, channel);
}

void CefMediaApi::SetStatusCallback(ChannelStatusCallback callback, void* obj)
{
    CefMediaIpcClient::Instance().SetStatusCallback(callback, obj);
}

void CefMediaApi::RemoveVideoFrameCallback(VideoFrameCallback callback, void* obj, int channel)
{
    CefMediaIpcClient::Instance().RemoveVideoFrameCallback(callback, obj, channel);
}

void CefMediaApi::RemoveAudioFrameCallback(AudioBufferCallback callback, void* obj, int channel)
{
    CefMediaIpcClient::Instance().RemoveAudioFrameCallback(callback, obj, channel);
}

void CefMediaApi::RemoveStatusCallback(ChannelStatusCallback callback, void* obj)
{
    CefMediaIpcClient::Instance().RemoveStatusCallback(callback, obj);
}

void CefMediaApi::ReaderStart(int channel)
{
    CefMediaIpcClient::Instance().ReaderStart(channel);
}

void CefMediaApi::ReaderStop(int channel)
{
    CefMediaIpcClient::Instance().ReaderStop(channel);
}

void CefMediaApi::StatusStart()
{
    CefMediaIpcClient::Instance().StatusStart();
}

void CefMediaApi::StatusStop()
{
    CefMediaIpcClient::Instance().StatusStop();
}

void CefMediaApi::RateSet(int rate_permille, int channel)
{
    CefMediaIpcClient::Instance().RateSet(rate_permille, channel);
}
