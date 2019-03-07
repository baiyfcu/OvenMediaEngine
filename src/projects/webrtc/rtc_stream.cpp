#include "rtc_private.h"
#include "rtc_stream.h"
#include "rtc_application.h"
#include "rtc_session.h"

using namespace common;

std::shared_ptr<RtcStream> RtcStream::Create(const std::shared_ptr<Application> application,
                                             const StreamInfo &info)
{
	auto stream = std::make_shared<RtcStream>(application, info);
	if(!stream->Start())
	{
		return nullptr;
	}
	return stream;
}

RtcStream::RtcStream(const std::shared_ptr<Application> application,
                     const StreamInfo &info)
	: Stream(application, info)
{
	_certificate = application->GetSharedPtrAs<RtcApplication>()->GetCertificate();
}

RtcStream::~RtcStream()
{
	logtd("RtcStream(%d) has been terminated finally", GetId());
	Stop();
}

bool RtcStream::Start()
{
	// OFFER SDP 생성
	_offer_sdp = std::make_shared<SessionDescription>();
	_offer_sdp->SetOrigin("OvenMediaEngine", ov::Random::GenerateInteger(), 2, "IN", 4, "127.0.0.1");
	_offer_sdp->SetTiming(0, 0);
	_offer_sdp->SetIceOption("trickle");
	_offer_sdp->SetIceUfrag(ov::Random::GenerateString(8));
	_offer_sdp->SetIcePwd(ov::Random::GenerateString(32));
	_offer_sdp->SetMsidSemantic("WMS", "*");
	_offer_sdp->SetFingerprint("sha-256", _certificate->GetFingerprint("sha-256"));

	// TODO(soulk): 현재는 Content가 Video 1개, Audio 1개라고 가정하고 개발되어 있다.
	// 현재의 Track은 Media Type과 Codec 정보만을 나타내며, Content를 구분할 수 없기 때문에
	// Multi Audio Channel 과 같은 내용을 표현할 수 없다.
	// GetContentCount() -> Content -> GetTrackCount로 확장해야 한다.
	// 다음은 위와 같은 제약사항으로 인해 개발된 임시 코드이다. by Getroot

	std::shared_ptr<MediaDescription> video_media_desc = nullptr;
	std::shared_ptr<MediaDescription> audio_media_desc = nullptr;

	bool first_video_desc = true;
	bool first_audio_desc = true;

	for(auto &track_item : _tracks)
	{
		ov::String codec = "";
		auto &track = track_item.second;

		switch(track->GetMediaType())
		{
			case MediaType::Video:
			{
				auto payload = std::make_shared<PayloadAttr>();

				switch(track->GetCodecId())
				{
					case MediaCodecId::Vp8:
						codec = "VP8";
						break;
					case MediaCodecId::H264:
						codec = "H264";

						payload->SetFmtp(ov::String::FormatString(
							// NonInterleaved => packetization-mode=1
							// baseline & lvl 3.1 => profile-level-id=42e01f
							"packetization-mode=1;profile-level-id=%x",
							0x42e01f
						));

						break;
					default:
						logtw("Unsupported codec(%d) is being input from media track", track->GetCodecId());
						continue;
				}

				if(first_video_desc)
				{
					video_media_desc = std::make_shared<MediaDescription>(_offer_sdp);
					video_media_desc->SetConnection(4, "0.0.0.0");
					// TODO(dimiden): Prevent duplication
					video_media_desc->SetMid(ov::Random::GenerateString(6));
					video_media_desc->SetSetup(MediaDescription::SetupType::ActPass);
					video_media_desc->UseDtls(true);
					video_media_desc->UseRtcpMux(true);
					video_media_desc->SetDirection(MediaDescription::Direction::SendOnly);
					video_media_desc->SetMediaType(MediaDescription::MediaType::Video);
					video_media_desc->SetCname(ov::Random::GenerateInteger(), ov::Random::GenerateString(16));

					_offer_sdp->AddMedia(video_media_desc);
					first_video_desc = false;
				}

				//TODO(getroot): WEBRTC에서는 TIMEBASE를 무조건 90000을 쓰는 것으로 보임, 정확히 알아볼것
				payload->SetRtpmap(track->GetId(), codec, 90000);

				video_media_desc->AddPayload(payload);

				// WebRTC에서 사용하는 Track만 별도로 관리한다.
				AddRtcTrack(payload->GetId(), track);

				// RTP Packetizer를 추가한다.
				AddRtpPacketizer(false, payload->GetId(), video_media_desc->GetSsrc());

				break;
			}

			case MediaType::Audio:
			{
				auto payload = std::make_shared<PayloadAttr>();

				switch(track->GetCodecId())
				{
					case MediaCodecId::Opus:
						codec = "OPUS";

						// Enable inband-fec
						// a=fmtp:111 maxplaybackrate=16000; useinbandfec=1; maxaveragebitrate=20000
						 payload->SetFmtp("stereo=1;useinbandfec=1;");

						break;

					default:
						logtw("Unsupported codec(%d) is being input from media track", track->GetCodecId());
						continue;
				}

				if(first_audio_desc)
				{
					audio_media_desc = std::make_shared<MediaDescription>(_offer_sdp);
					audio_media_desc->SetConnection(4, "0.0.0.0");
					// TODO(dimiden): Need to prevent duplication
					audio_media_desc->SetMid(ov::Random::GenerateString(6));
					audio_media_desc->SetSetup(MediaDescription::SetupType::ActPass);
					audio_media_desc->UseDtls(true);
					audio_media_desc->UseRtcpMux(true);
					audio_media_desc->SetDirection(MediaDescription::Direction::SendOnly);
					audio_media_desc->SetMediaType(MediaDescription::MediaType::Audio);
					audio_media_desc->SetCname(ov::Random::GenerateInteger(), ov::Random::GenerateString(16));
					_offer_sdp->AddMedia(audio_media_desc);
					first_audio_desc = false;
				}

				// TODO(dimiden): Need to change to transcoding profile's bitrate and channel
				payload->SetRtpmap(track->GetId(), codec, 48000, "2");

				audio_media_desc->AddPayload(payload);

				// WebRTC에서 사용하는 Track만 별도로 관리한다.
				AddRtcTrack(payload->GetId(), track);

				// RTP Packetizer를 추가한다.
				AddRtpPacketizer(true, payload->GetId(), audio_media_desc->GetSsrc());

				break;
			}

			default:
				// not supported type
				logtw("Not supported media type: %d", (int)(track->GetMediaType()));
				break;
		}
	}

	ov::String offer_sdp_text;
	_offer_sdp->ToString(offer_sdp_text);

	logti("Stream is created : %s/%u", GetName().CStr(), GetId());

	return Stream::Start();
}

bool RtcStream::Stop()
{
	_rtc_track.clear();

	return Stream::Stop();
}

std::shared_ptr<SessionDescription> RtcStream::GetSessionDescription()
{
	return _offer_sdp;
}

std::shared_ptr<RtcSession> RtcStream::FindRtcSessionByPeerSDPSessionID(uint32_t session_id)
{
	// 모든 Session에 Frame을 전달한다.
	for(auto const &x : GetSessionMap())
	{
		auto session = std::static_pointer_cast<RtcSession>(x.second);

		if(session->GetPeerSDP() != nullptr && session->GetPeerSDP()->GetSessionId() == session_id)
		{
			return session;
		}
	}

	return nullptr;
}

bool RtcStream::SendRtpToNetwork(std::unique_ptr<RtpPacket> packet)
{
	logte("RTP Packetizing completed : length(%d), payload_type(%d)", packet->GetData()->GetLength(), packet->PayloadType());

	// 모든 Session에 만들어진 Packet을 전달한다.
	for(auto const &x : GetSessionMap())
	{
		auto session = x.second;

		// Session에 전달되면 SRTP로 인해 모두 변경되기 때문에 복제하여 보내야 한다.
		auto session_packet = std::make_unique<RtpPacket>(*packet);
		logte("Copy packet : packet length (%d)", packet->GetData()->GetLength());
		std::static_pointer_cast<RtcSession>(session)->SendOutgoingData(std::move(session_packet));
	}

	return true;
}

bool RtcStream::SendRtcpToNetwork(std::unique_ptr<RtcpPacket> packet)
{
	return true;
}

void RtcStream::SendVideoFrame(std::shared_ptr<MediaTrack> track,
                               std::unique_ptr<EncodedFrame> encoded_frame,
                               std::unique_ptr<CodecSpecificInfo> codec_info,
                               std::unique_ptr<FragmentationHeader> fragmentation)
{
	// VideoFrame 데이터를 Protocol에 맞게 변환한다.

	// RTP Video Header를 생성한다. 사용한 CodecSpecificInfo는 버린다. (자동 삭제 됨)
	RTPVideoHeader rtp_video_header;
	memset(&rtp_video_header, 0, sizeof(RTPVideoHeader));

	if(codec_info)
	{
		MakeRtpVideoHeader(codec_info.get(), &rtp_video_header);
	}

	// RTP Packetizing
	// Track의 GetId와 PayloadType은 같다. Track의 ID로 Payload Type을 만들기 때문이다.
	auto rtp_sender = GetRtpSender(track->GetId());

	// RTP_SENDER에 등록된 RtpRtcpSession에 의해서 Packetizing이 완료되면 SendRtpToNetwork 함수가 호출된다.
	rtp_sender->SendOutgoingData(encoded_frame->frame_type,
		encoded_frame->time_stamp,
		encoded_frame->buffer,
		encoded_frame->length,
		fragmentation.get(),
		&rtp_video_header);
}

void RtcStream::SendAudioFrame(std::shared_ptr<MediaTrack> track,
                               std::unique_ptr<EncodedFrame> encoded_frame,
                               std::unique_ptr<CodecSpecificInfo> codec_info,
                               std::unique_ptr<FragmentationHeader> fragmentation)
{
	// AudioFrame 데이터를 Protocol에 맞게 변환한다.
	OV_ASSERT2(encoded_frame != nullptr);
	OV_ASSERT2(encoded_frame->buffer != nullptr);

	// RTP Packetizing
	// Track의 GetId와 PayloadType은 같다. Track의 ID로 Payload Type을 만들기 때문이다.
	auto rtp_sender = GetRtpSender(track->GetId());
	rtp_sender->SendOutgoingData(encoded_frame->frame_type,
	                             encoded_frame->time_stamp,
	                             encoded_frame->buffer,
	                             encoded_frame->length,
	                             fragmentation.get(),
	                             nullptr);
}

void RtcStream::MakeRtpVideoHeader(const CodecSpecificInfo *info, RTPVideoHeader *rtp_video_header)
{
	switch(info->codec_type)
	{
		case CodecType::Vp8:
			rtp_video_header->codec = RtpVideoCodecType::Vp8;
			rtp_video_header->codec_header.vp8.InitRTPVideoHeaderVP8();
			rtp_video_header->codec_header.vp8.picture_id = info->codec_specific.vp8.picture_id;
			rtp_video_header->codec_header.vp8.non_reference = info->codec_specific.vp8.non_reference;
			rtp_video_header->codec_header.vp8.temporal_idx = info->codec_specific.vp8.temporal_idx;
			rtp_video_header->codec_header.vp8.layer_sync = info->codec_specific.vp8.layer_sync;
			rtp_video_header->codec_header.vp8.tl0_pic_idx = info->codec_specific.vp8.tl0_pic_idx;
			rtp_video_header->codec_header.vp8.key_idx = info->codec_specific.vp8.key_idx;
			rtp_video_header->simulcast_idx = info->codec_specific.vp8.simulcast_idx;
			return;
		case CodecType::H264:
			rtp_video_header->codec = RtpVideoCodecType::H264;
			rtp_video_header->codec_header.h264.packetization_mode = info->codec_specific.h264.packetization_mode;
			rtp_video_header->simulcast_idx = info->codec_specific.h264.simulcast_idx;
			return;
	}
}

void RtcStream::AddRtpPacketizer(bool audio, uint32_t payload_type, uint32_t ssrc)
{
	auto rtp_sender = std::make_shared<RTPSender>(audio, RtpRtcpSession::GetSharedPtr());
	rtp_sender->SetPayloadType(payload_type);
	rtp_sender->SetSSRC(ssrc);

	_rtp_sender[payload_type] = rtp_sender;
}

void RtcStream::AddRtcTrack(uint32_t payload_type, std::shared_ptr<MediaTrack> track)
{
	_rtc_track[payload_type] = track;
}

std::shared_ptr<MediaTrack> RtcStream::GetRtcTrack(uint32_t payload_type)
{
	if(!_rtc_track.count(payload_type))
	{
		return nullptr;
	}

	return _rtc_track[payload_type];
}

std::shared_ptr<RTPSender> RtcStream::GetRtpSender(uint32_t payload_type)
{
	if(!_rtp_sender.count(payload_type))
	{
		return nullptr;
	}

	return _rtp_sender[payload_type];
}
