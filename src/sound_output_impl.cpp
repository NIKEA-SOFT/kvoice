#include "bass/bass.h"
#include "voice_exception.hpp"
#include "sound_output_impl.hpp"
#include "stream_impl.hpp"
#include <array>
#include <chrono>

kvoice::sound_output_impl::sound_output_impl(std::string_view device_name, std::uint32_t sample_rate)
    : sampling_rate(sample_rate), requests() {
    using namespace std::string_literals;

    std::mutex              condvar_mtx{};
    std::condition_variable output_initialization{};
    output_alive = true;
    output_thread = std::thread([this, sample_rate, name = device_name, &output_initialization]() {
        auto ensureDeviceInited = [sample_rate](std::string_view name) -> int {
            int idx = -1;
            if (!name.empty()) {
                BASS_DEVICEINFO info{};
                for (int i = 1; BASS_GetDeviceInfo(i, &info); ++i) {
                    if ((info.flags & BASS_DEVICE_ENABLED) && name == info.name) {
                        idx = i;
                        break;
                    }
                }
                if (idx == -1) return -1;
            }

            // idx == -1 -> default device
            BASS_DEVICEINFO di{};
            if (idx != -1) BASS_GetDeviceInfo(idx, &di);
            if (idx == -1 || !(di.flags & BASS_DEVICE_INIT))
            {
                DWORD flags = 0;
                flags |= BASS_DEVICE_STEREO;
                flags |= BASS_DEVICE_FREQ;

                if (!BASS_Init(idx, sample_rate, flags, nullptr, nullptr)) {
                    return -1;
                }

                BASS_SetConfig(BASS_CONFIG_3DALGORITHM, BASS_3DALG_DEFAULT);
            }

            BASS_SetDevice(idx);
            return idx;
        };

        (void)ensureDeviceInited(name);

        // dummy https request to init OpenSSL(not thread safe in basslib)
        auto temp_handle = BASS_StreamCreateURL("https://www.google.com", 0, 0, NULL, 0);
        BASS_StreamFree(temp_handle);

        output_initialization.notify_one();

        while (output_alive.load())
        {
            BASS_SetConfig(BASS_CONFIG_GVOL_STREAM, static_cast<unsigned>(output_gain.load() * 10000));

            if (device_need_update.load()) {
                int target = ensureDeviceInited(this->device_name);
                if (target == -1) {
                    target = ensureDeviceInited("");
                }

                if (target != -1) {
                    {
                        std::lock_guard lk(channels_mtx_);
                        for (DWORD h : channels_)
                        {
                            if (!h) continue;
                            BASS_CHANNELINFO ci{};
                            if (BASS_ChannelGetInfo(h, &ci) && (ci.flags & BASS_STREAM_DECODE))
                                continue;

                            BASS_ChannelPause(h);
                            if (!BASS_ChannelSetDevice(h, target)) {

                            }
                            BASS_ChannelPlay(h, FALSE);
                        }
                    }

                    BASS_SetDevice(target);
                }

                device_need_update.store(false);
            }

            {
                std::lock_guard lock(spatial_mtx);
                auto            vec_convert = [](kvoice::vector vec) { return BASS_3DVECTOR{ vec.x, vec.y, vec.z }; };
                BASS_3DVECTOR   pos = vec_convert(listener_pos);
                BASS_3DVECTOR   vel = vec_convert(listener_vel);
                BASS_3DVECTOR   front = vec_convert(listener_front);
                BASS_3DVECTOR   up = vec_convert(listener_up);
                BASS_Set3DPosition(&pos, &vel, &front, &up);
                BASS_Apply3D();
            }

            std::array<request_stream_message, ringbuffer_max_size> msg_buffer;
            std::size_t available = requests.readBuff(msg_buffer.data(), msg_buffer.size());

            for (std::size_t i = 0; i < available; i++) {
                auto& msg = msg_buffer[i];
                if (msg.params.has_value()) {
                    auto& params = *msg.params;
                    msg.on_creation_callback(
                        std::make_unique<stream_impl>(this, params.url, params.file_offset, this->sampling_rate));
                } else {
                    msg.on_creation_callback(std::make_unique<stream_impl>(this, this->sampling_rate));
                }
            };

            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        BASS_Free();
    });

    std::unique_lock lock{ condvar_mtx };
    output_initialization.wait(lock);
}

kvoice::sound_output_impl::~sound_output_impl() {
    output_alive = false;
    output_thread.join();
}

void kvoice::sound_output_impl::register_channel(DWORD h)
{
    if (!h) return;
    std::lock_guard lk(channels_mtx_);
    channels_.push_back(h);
}

void kvoice::sound_output_impl::unregister_channel(DWORD h)
{
    std::lock_guard lk(channels_mtx_);
    auto it = std::find(channels_.begin(), channels_.end(), h);
    if (it != channels_.end()) channels_.erase(it);
}

void kvoice::sound_output_impl::set_my_position(vector pos) noexcept {
    std::lock_guard lock{ spatial_mtx };
    listener_pos = pos;
}

void kvoice::sound_output_impl::set_my_velocity(vector vel) noexcept {
    std::lock_guard lock{ spatial_mtx };
    listener_vel = vel;
}

void kvoice::sound_output_impl::set_my_orientation_up(vector up) noexcept {
    std::lock_guard lock{ spatial_mtx };
    listener_up = up;
}

void kvoice::sound_output_impl::set_my_orientation_front(vector front) noexcept {
    std::lock_guard lock{ spatial_mtx };
    listener_front = front;
}

void kvoice::sound_output_impl::set_gain(float gain) noexcept {
    output_gain.store(gain); }

float kvoice::sound_output_impl::get_gain() const { 
    return output_gain.load(std::memory_order_acquire);
}

void kvoice::sound_output_impl::change_device(std::string_view device_name) {
    if (device_need_update.load()) return;
    this->device_name = device_name;
    device_need_update.store(true);
}

void kvoice::sound_output_impl::create_stream(on_create_callback cb) {
    requests.insert(request_stream_message{ std::nullopt, cb });
}

void kvoice::sound_output_impl::create_stream(on_create_callback cb, 
    std::string_view url, std::uint32_t file_offset) {
    requests.insert(request_stream_message{std::make_optional(online_stream_parameters{ std::string{ url }, file_offset }), std::move(cb)});
}
