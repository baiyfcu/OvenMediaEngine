//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Jaejong Bong
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================

#include "segment_stream.h"
#include "segment_stream_application.h"

#define OV_LOG_TAG "SegmentStream"

using namespace common;

//====================================================================================================
// Create
//====================================================================================================
std::shared_ptr<SegmentStream>
SegmentStream::Create(const std::shared_ptr<Application> application, const StreamInfo &info, uint32_t worker_count)
{
    auto stream = std::make_shared<SegmentStream>(application, info);

    //TODO(Bong): SegmentStream should use stream_worker. Change 0 to worker_count.
    if (!stream->Start(0))
    {
        return nullptr;
    }
    return stream;
}

//====================================================================================================
// SegmentStream
// - DASH/HLS : H264/AAC only
// TODO : 다중 트랜스코딩/다중 트랙 구분 및 처리 필요
//====================================================================================================
SegmentStream::SegmentStream(const std::shared_ptr<Application> application, const StreamInfo &info) : Stream(
        application, info)
{
    std::string prefix = this->GetName().CStr();
    std::shared_ptr<MediaTrack> video_track = nullptr;
    std::shared_ptr<MediaTrack> audio_track = nullptr;

    for (auto &track_item : _tracks)
    {
        auto &track = track_item.second;

        if(track->GetMediaType() == MediaType::Video &&  track->GetCodecId() == MediaCodecId::H264)
        {
            video_track = track;
        }
        else if(track->GetMediaType() == MediaType::Audio &&  track->GetCodecId() == MediaCodecId::Aac)
        {
            audio_track = track;
        }
    }

    PacketyzerMediaInfo media_info;

    if (video_track != nullptr && video_track->GetCodecId() == MediaCodecId::H264)
    {
        media_info.video_codec_type = SegmentCodecType::H264Codec;
        media_info.video_framerate = video_track->GetFrameRate();
        media_info.video_width = video_track->GetWidth();
        media_info.video_height = video_track->GetHeight();
        media_info.video_timescale = video_track->GetTimeBase().GetDen();
        media_info.video_bitrate = video_track->GetBitrate();

        _media_tracks[video_track->GetId()] = video_track;
    }

    if (audio_track != nullptr && audio_track->GetCodecId() == MediaCodecId::Aac)
    {
        media_info.audio_codec_type = SegmentCodecType::AacCodec;
        media_info.audio_samplerate = audio_track->GetSampleRate();
        media_info.audio_channels = audio_track->GetChannel().GetCounts();
        media_info.audio_timescale = audio_track->GetTimeBase().GetDen();
        media_info.audio_bitrate = audio_track->GetBitrate();

        _media_tracks[audio_track->GetId()] = audio_track;
    }

    if (video_track != nullptr || audio_track != nullptr)
    {
        PacketyzerStreamType stream_type = PacketyzerStreamType::Common;

        if (video_track == nullptr) stream_type = PacketyzerStreamType::AudioOnly;
        if (audio_track == nullptr) stream_type = PacketyzerStreamType::VideoOnly;

        SegmentConfigInfo dash_segment_config_info(true, DEFAULT_SEGMENT_COUNT, DEFAULT_SEGMENT_DURATION);
        SegmentConfigInfo hls_segment_config_info(true, DEFAULT_SEGMENT_COUNT, DEFAULT_SEGMENT_DURATION);

        const auto &publishers = application->GetPublishers();

        for (auto &publisher_info : publishers)
        {
            if (cfg::PublisherType::Dash == publisher_info->GetType())
            {
                dash_segment_config_info._count = dynamic_cast<const cfg::DashPublisher *>(publisher_info)->GetSegmentCount();
                dash_segment_config_info._duration = dynamic_cast<const cfg::DashPublisher *>(publisher_info)->GetSegmentDuration();

                if (dash_segment_config_info._count <= 0)
                    dash_segment_config_info._count = DEFAULT_SEGMENT_COUNT;

                if (dash_segment_config_info._duration <= 0)
                    dash_segment_config_info._duration = DEFAULT_SEGMENT_DURATION;

            }
            else if (cfg::PublisherType::Hls == publisher_info->GetType())
            {
                hls_segment_config_info._count = dynamic_cast<const cfg::HlsPublisher *>(publisher_info)->GetSegmentCount();
                hls_segment_config_info._duration = dynamic_cast<const cfg::HlsPublisher *>(publisher_info)->GetSegmentDuration();

                if (hls_segment_config_info._count <= 0)
                    hls_segment_config_info._count = DEFAULT_SEGMENT_COUNT;

                if (hls_segment_config_info._duration <= 0)
                    hls_segment_config_info._duration = DEFAULT_SEGMENT_DURATION;
            }
        }

        _stream_packetyzer = std::make_unique<StreamPacketyzer>(dash_segment_config_info,
                                                                hls_segment_config_info,
                                                                prefix,
                                                                stream_type,
                                                                media_info);
    }

    _stream_check_time = time(nullptr);
    _previous_key_frame_timestamp = 0;
}

//====================================================================================================
// ~SegmentStream
//====================================================================================================
SegmentStream::~SegmentStream()
{
    Stop();
}

//====================================================================================================
// Start
//====================================================================================================
bool SegmentStream::Start(uint32_t worker_count)
{
    return Stream::Start(worker_count);
}

//====================================================================================================
// Stop
//====================================================================================================
bool SegmentStream::Stop()
{
    return Stream::Stop();
}

//====================================================================================================
// SendVideoFrame
// - Packetyzer에 Video데이터 추가
// - 첫 key 프레임 에서 SPS/PPS 추출  이후 생성
//
//====================================================================================================
void SegmentStream::SendVideoFrame(std::shared_ptr<MediaTrack> track,
                                   std::unique_ptr<EncodedFrame> encoded_frame,
                                   std::unique_ptr<CodecSpecificInfo> codec_info,
                                   std::unique_ptr<FragmentationHeader> fragmentation)
{
    //logtd("Video Timestamp : %d" , encoded_frame->time_stamp);

    if (_stream_packetyzer != nullptr && _media_tracks.find(track->GetId()) != _media_tracks.end())
    {
        _stream_packetyzer->AppendVideoData(encoded_frame->_time_stamp,
                                            track->GetTimeBase().GetDen(),
                                            encoded_frame->_frame_type == FrameType::VideoFrameKey,
                                            0,
                                            encoded_frame->_length,
                                            encoded_frame->_buffer->GetDataAs<uint8_t>());

        if (encoded_frame->_frame_type == FrameType::VideoFrameKey)
        {
            _key_frame_interval = encoded_frame->_time_stamp - _previous_key_frame_timestamp;
            _previous_key_frame_timestamp = encoded_frame->_time_stamp;
        }

        _last_video_timestamp = encoded_frame->_time_stamp/90;
        _video_frame_count++;

        time_t current_time = time(nullptr);
        uint32_t check_gap = current_time - _stream_check_time;

        if(check_gap >= 60)
        {
            logtd("Segment Stream Info - stram(%s) key(%ums) timestamp(v:%lldms/a:%lldms/g:%lldms) fps(v:%u/a:%u) gap(v:%ums/a:%ums)",
                  GetName().CStr(),
                  _key_frame_interval/90, // 90000 *1000
                  _last_video_timestamp,
                  _last_audio_timestamp,
                  _last_video_timestamp - _last_audio_timestamp,
                  _video_frame_count/check_gap,
                  _audio_frame_count/check_gap,
                  _last_video_timestamp - _previous_last_video_timestamp,
                  _last_audio_timestamp - _previous__last_audio_timestamp);

            _stream_check_time = current_time;
            _video_frame_count = 0;
            _audio_frame_count = 0;
            _previous_last_video_timestamp = _last_video_timestamp;
            _previous__last_audio_timestamp = _last_audio_timestamp;
        }
    }
}

//====================================================================================================
// SendAudioFrame
// - Packetyzer에 Audio데이터 추가
//====================================================================================================
void SegmentStream::SendAudioFrame(std::shared_ptr<MediaTrack> track,
                                   std::unique_ptr<EncodedFrame> encoded_frame,
                                   std::unique_ptr<CodecSpecificInfo> codec_info,
                                   std::unique_ptr<FragmentationHeader> fragmentation)
                                   {
    //logtd("Audio Timestamp : %d", encoded_frame->time_stamp);

    if (_stream_packetyzer != nullptr && _media_tracks.find(track->GetId()) != _media_tracks.end())
    {
        _stream_packetyzer->AppendAudioData(encoded_frame->_time_stamp,
                                            track->GetTimeBase().GetDen(),
                                            encoded_frame->_length,
                                            encoded_frame->_buffer->GetDataAs<uint8_t>());

        _last_audio_timestamp = encoded_frame->_time_stamp/(track->GetTimeBase().GetDen()/1000);
        _audio_frame_count++;
    }
}

//====================================================================================================
// GetPlayList
// - M3U8/MPD
//====================================================================================================
bool SegmentStream::GetPlayList(PlayListType play_list_type, ov::String &play_list)
{
    if (_stream_packetyzer != nullptr)
    {
        return _stream_packetyzer->GetPlayList(play_list_type, play_list);
    }

    return false;
}

//====================================================================================================
// GetSegment
// - TS/M4S(mp4)
//====================================================================================================
bool SegmentStream::GetSegment(SegmentType type, const ov::String &file_name, std::shared_ptr<ov::Data> &data)
{
    if (_stream_packetyzer != nullptr)
    {
        return _stream_packetyzer->GetSegment(type, file_name, data);
    }

    return false;
}
