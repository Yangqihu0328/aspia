//
// Aspia Project
// Copyright (C) 2020 Dmitry Chapyshev <dmitry@aspia.ru>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#include "base/audio/audio_player.h"

#include "base/audio/audio_output.h"
#include "proto/desktop.pb.h"

namespace base {

AudioPlayer::AudioPlayer()
{
    output_ = AudioOutput::create(std::bind(
        &AudioPlayer::onMoreDataRequired, this, std::placeholders::_1, std::placeholders::_2));
    if (output_)
        output_->start();
}

AudioPlayer::~AudioPlayer() = default;

void AudioPlayer::addPacket(std::unique_ptr<proto::AudioPacket> packet)
{
    std::scoped_lock lock(incoming_queue_lock_);
    incoming_queue_.emplace(std::move(packet));
}

size_t AudioPlayer::onMoreDataRequired(void* data, size_t size)
{
    if (work_queue_.empty())
    {
        {
            std::scoped_lock lock(incoming_queue_lock_);
            work_queue_.swap(incoming_queue_);
        }

        if (work_queue_.empty())
            return 0;
    }

    size_t target_pos = 0;

    while (!work_queue_.empty())
    {
        const std::string& packet_data = work_queue_.front()->data(0);

        size_t source_size = packet_data.size() - source_pos_;
        size_t target_size = size - target_pos;
        size_t num_bytes = std::min(target_size, source_size);

        memcpy(reinterpret_cast<uint8_t*>(data) + target_pos,
               packet_data.data() + source_pos_,
               num_bytes);
        target_pos += num_bytes;

        if (target_size < source_size)
        {
            source_pos_ += num_bytes;
            break;
        }
        else
        {
            work_queue_.pop();
            source_pos_ = 0;

            if (target_size == source_size)
                break;
        }
    }

    return target_pos;
}

} // namespace base
