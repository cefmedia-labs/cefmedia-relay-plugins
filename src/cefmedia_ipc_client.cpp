#include "cefmedia_ipc_client.h"

#include <obs-module.h>

#include <cstring>
#include <string>

// libipc (mutouyun/cpp-ipc), linked statically into this plugin. CefMedia Relay
// process (cefmedia.dll inside CefMediaRelay.exe) hosts the same channels.
#include "libipc/ipc.h"

namespace {

// Receive wait timeout (ms): bounded so the threads can check the stop flag.
constexpr uint64_t kRecvTimeoutMs = 100;
// Rate send timeout (ms): the CefMedia Relay not running must not stall the OBS UI
// heartbeat; a failed send simply retries on the next heartbeat.
constexpr uint64_t kRateSendTimeoutMs = 50;

// libipc route names per channel: "cefmedia.video.<ch>" / "cefmedia.audio.<ch>"
// with the 0-based decimal suffix — the same concatenation the relay side uses
// when sending. Both ends must spell
// the names identically or they never meet.
std::string VideoRouteName(int channel)
{
    return std::string(CEFMEDIA_IPC_VIDEO_CHANNEL) + "." + std::to_string(channel);
}

std::string AudioRouteName(int channel)
{
    return std::string(CEFMEDIA_IPC_AUDIO_CHANNEL) + "." + std::to_string(channel);
}

// Sanity clamp (same as the old control-shm writer): 0.1x .. 8x.
int ClampRate(int rate_permille)
{
    if (rate_permille < 100) return 100;
    if (rate_permille > 8000) return 8000;
    return rate_permille;
}

// Clamp a caller-supplied channel into 0..4 (defensive: a wrong channel number
// must degrade to channel 0, never index out of bounds).
int ClampChannel(int channel)
{
    if (channel < 0 || channel >= CEFMEDIA_CHANNEL_COUNT) return 0;
    return channel;
}

} // namespace

CefMediaIpcClient& CefMediaIpcClient::Instance()
{
    static CefMediaIpcClient instance;
    return instance;
}

CefMediaIpcClient::~CefMediaIpcClient()
{
    for (int ch = 0; ch < kChannelCount; ++ch)
    {
        channels_[ch].refcount.store(0);
    }
    // Stop whatever threads are still running (static lifetime teardown).
    for (int ch = 0; ch < kChannelCount; ++ch)
    {
        ChannelState& st = channels_[ch];
        if (st.video_thread || st.audio_thread)
        {
            ReaderStop(ch);
        }
    }
    if (status_thread_)
    {
        StatusStop();
    }
}

void CefMediaIpcClient::SetVideoFrameCallback(VideoFrameCallback callback, void* obj, int channel)
{
    ChannelState& st = channels_[ClampChannel(channel)];
    std::lock_guard<std::mutex> lk(callbacks_mutex_);
    for (const auto& e : st.video_callbacks) {
        if (e.fn == callback && e.obj == obj) {
            return; // idempotent
        }
    }
    st.video_callbacks.push_back({callback, obj});
}

void CefMediaIpcClient::SetAudioFrameCallback(AudioBufferCallback callback, void* obj, int channel)
{
    ChannelState& st = channels_[ClampChannel(channel)];
    std::lock_guard<std::mutex> lk(callbacks_mutex_);
    for (const auto& e : st.audio_callbacks) {
        if (e.fn == callback && e.obj == obj) {
            return;
        }
    }
    st.audio_callbacks.push_back({callback, obj});
}

void CefMediaIpcClient::SetStatusCallback(ChannelStatusCallback callback, void* obj)
{
    std::lock_guard<std::mutex> lk(callbacks_mutex_);
    for (size_t i = 0; i < status_callbacks_.size(); ++i) {
        if (status_callbacks_[i] == callback && status_objs_[i] == obj) {
            return; // idempotent
        }
    }
    status_callbacks_.push_back(callback);
    status_objs_.push_back(obj);
}

void CefMediaIpcClient::RemoveVideoFrameCallback(VideoFrameCallback callback, void* obj,
                                                 int channel)
{
    ChannelState& st = channels_[ClampChannel(channel)];
    std::lock_guard<std::mutex> lk(callbacks_mutex_);
    for (auto it = st.video_callbacks.begin(); it != st.video_callbacks.end(); ++it) {
        if (it->fn == callback && it->obj == obj) {
            st.video_callbacks.erase(it);
            return;
        }
    }
}

void CefMediaIpcClient::RemoveAudioFrameCallback(AudioBufferCallback callback, void* obj,
                                                 int channel)
{
    ChannelState& st = channels_[ClampChannel(channel)];
    std::lock_guard<std::mutex> lk(callbacks_mutex_);
    for (auto it = st.audio_callbacks.begin(); it != st.audio_callbacks.end(); ++it) {
        if (it->fn == callback && it->obj == obj) {
            st.audio_callbacks.erase(it);
            return;
        }
    }
}

void CefMediaIpcClient::RemoveStatusCallback(ChannelStatusCallback callback, void* obj)
{
    std::lock_guard<std::mutex> lk(callbacks_mutex_);
    for (size_t i = 0; i < status_callbacks_.size(); ++i) {
        if (status_callbacks_[i] == callback && status_objs_[i] == obj) {
            status_callbacks_.erase(status_callbacks_.begin() + i);
            status_objs_.erase(status_objs_.begin() + i);
            return;
        }
    }
}

void CefMediaIpcClient::ReaderStart(int channel)
{
    ChannelState& st = channels_[ClampChannel(channel)];
    if (st.refcount.fetch_add(1) > 0) {
        return; // threads already running
    }
    st.stop_requested = false;
    st.video_thread = new std::thread(&VideoRecvThreadProc, this, ClampChannel(channel));
    st.audio_thread = new std::thread(&AudioRecvThreadProc, this, ClampChannel(channel));
    blog(LOG_INFO, "[cefmedia] ipc client: ch%d receive threads started (%s / %s)",
         channel, VideoRouteName(channel).c_str(), AudioRouteName(channel).c_str());
}

void CefMediaIpcClient::ReaderStop(int channel)
{
    ChannelState& st = channels_[ClampChannel(channel)];
    const int prev = st.refcount.fetch_sub(1);
    if (prev > 1) {
        return; // another consumer still active
    }
    if (prev <= 0) {
        st.refcount.store(0);
        return;
    }
    st.stop_requested = true;
    if (st.video_thread) {
        st.video_thread->join();
        delete st.video_thread;
        st.video_thread = nullptr;
    }
    if (st.audio_thread) {
        st.audio_thread->join();
        delete st.audio_thread;
        st.audio_thread = nullptr;
    }
    blog(LOG_INFO, "[cefmedia] ipc client: ch%d receive threads stopped", channel);
}

void CefMediaIpcClient::StatusStart()
{
    if (status_refcount_.fetch_add(1) > 0) {
        return;
    }
    status_stop_requested_ = false;
    status_thread_ = new std::thread(&StatusRecvThreadProc, this);
    blog(LOG_INFO, "[cefmedia] ipc client: status thread started (%s)",
         CEFMEDIA_IPC_STATUS_CHANNEL);
}

void CefMediaIpcClient::StatusStop()
{
    const int prev = status_refcount_.fetch_sub(1);
    if (prev > 1) {
        return;
    }
    if (prev <= 0) {
        status_refcount_.store(0);
        return;
    }
    status_stop_requested_ = true;
    if (status_thread_) {
        status_thread_->join();
        delete status_thread_;
        status_thread_ = nullptr;
    }
    blog(LOG_INFO, "[cefmedia] ipc client: status thread stopped");
}

void CefMediaIpcClient::RateSet(int rate_permille, int channel)
{
    // Channel per call: cheap (handle + shm map lookup cached by libipc inside
    // one channel instance is preferable, but the heartbeat fires at most every
    // 3 s and the UI-thread budget stays tiny; a persistent channel would need
    // its own lifetime management across CefMedia Relay restarts).
    try {
        ipc::channel chan(CEFMEDIA_IPC_RATE_CHANNEL, ipc::sender);
        cefmedia_rate_msg msg;
        std::memset(&msg, 0, sizeof(msg));
        msg.rate_permille = ClampRate(rate_permille);
        msg.channel = ClampChannel(channel);
        chan.send(&msg, sizeof(msg), kRateSendTimeoutMs);
    } catch (...) {
        // CefMedia Relay not running / libipc storage unavailable: silent (the next
        // heartbeat retries). Never throw through the OBS UI thread.
    }
}

void CefMediaIpcClient::VideoRecvThreadProc(CefMediaIpcClient* self, int channel)
{
    ChannelState& st = self->channels_[channel];
    // Broadcast route: CefMedia Relay is the single writer, this plugin a reader.
    ipc::route chan(VideoRouteName(channel).c_str(), ipc::receiver);
    while (!st.stop_requested.load()) {
        ipc::buff_t msg = chan.recv(kRecvTimeoutMs);
        if (msg.empty()) {
            continue; // timeout: check stop
        }
        if (msg.size() < CEFMEDIA_VIDEO_HEADER_SIZE) {
            continue; // truncated: drop
        }
        auto* frame = reinterpret_cast<cefmedia_video_frame*>(msg.data());
        const uint8_t* payload =
            reinterpret_cast<const uint8_t*>(msg.data()) + CEFMEDIA_VIDEO_HEADER_SIZE;
        // Snapshot the callback list, then invoke (fan-out contract:
        // callbacks copy, they do not keep the pointers). The
        // channel rides the callback — the route name carries it, the header
        // has no channel field.
        std::vector<VideoCallbackEntry> cbs;
        {
            std::lock_guard<std::mutex> lk(self->callbacks_mutex_);
            cbs = st.video_callbacks;
        }
        for (const auto& cb : cbs) {
            cb.fn(frame, payload, cb.obj, channel);
        }
    }
}

void CefMediaIpcClient::AudioRecvThreadProc(CefMediaIpcClient* self, int channel)
{
    ChannelState& st = self->channels_[channel];
    ipc::route chan(AudioRouteName(channel).c_str(), ipc::receiver);
    while (!st.stop_requested.load()) {
        ipc::buff_t msg = chan.recv(kRecvTimeoutMs);
        if (msg.empty()) {
            continue;
        }
        if (msg.size() < CEFMEDIA_AUDIO_HEADER_SIZE) {
            continue;
        }
        auto* buf = reinterpret_cast<cefmedia_audio_buffer*>(msg.data());
        const uint8_t* payload =
            reinterpret_cast<const uint8_t*>(msg.data()) + CEFMEDIA_AUDIO_HEADER_SIZE;
        std::vector<AudioCallbackEntry> cbs;
        {
            std::lock_guard<std::mutex> lk(self->callbacks_mutex_);
            cbs = st.audio_callbacks;
        }
        for (const auto& cb : cbs) {
            cb.fn(buf, payload, cb.obj, channel);
        }
    }
}

void CefMediaIpcClient::StatusRecvThreadProc(CefMediaIpcClient* self)
{
    // Multi-writer channel, this plugin a reader. CefMedia Relay (single writer in
    // practice) broadcasts the occupancy bitmap on every change; a late-started
    // OBS learns the current state from CefMedia Relay's periodic resend.
    ipc::channel chan(CEFMEDIA_IPC_STATUS_CHANNEL, ipc::receiver);
    while (!self->status_stop_requested_.load()) {
        ipc::buff_t msg = chan.recv(kRecvTimeoutMs);
        if (msg.empty()) {
            continue; // timeout: check stop (CefMedia Relay absent is the normal idle)
        }
        if (msg.size() < sizeof(cefmedia_status_msg)) {
            continue;
        }
        cefmedia_status_msg m;
        std::memcpy(&m, msg.data(), sizeof(m)); // copy out of the buff_t lifetime
        std::vector<ChannelStatusCallback> cbs;
        std::vector<void*> objs;
        {
            std::lock_guard<std::mutex> lk(self->callbacks_mutex_);
            cbs = self->status_callbacks_;
            objs = self->status_objs_;
        }
        for (size_t i = 0; i < cbs.size(); ++i) {
            cbs[i](m.channel_bitmap, objs[i]);
        }
    }
}
