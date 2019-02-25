//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "relay_server.h"

#include "relay_private.h"

#include <media_router/media_router.h>
#include <base/media_route/media_route_application_interface.h>
#include <base/media_route/media_buffer.h>

RelayServer::RelayServer(MediaRouteApplicationInterface *media_route_application, const info::Application &application_info)
	: _media_route_application(media_route_application),
	  _application_info(application_info)
{
	// Listen to localhost:<relay_port>
	int port = application_info.GetRelayPort();

	_server_port = PhysicalPortManager::Instance()->CreatePort(ov::SocketType::Srt, ov::SocketAddress(static_cast<uint16_t>(port)));

	if(_server_port != nullptr)
	{
		logti("Trying to start relay server on %d", port);
		_server_port->AddObserver(this);
	}
	else
	{
		logtw("Could not create relay port. Origin features will not work.");
	}
}

RelayServer::~RelayServer()
{
	if(_server_port != nullptr)
	{
		_server_port->RemoveObserver(this);
	}
}

void RelayServer::SendStream(ov::Socket *remote, const std::shared_ptr<StreamInfo> &stream_info)
{
	// serialize media track
	ov::String serialize = ov::String::FormatString("%s\n", stream_info->GetName().CStr());

	// about 45 bytes per track
	for(auto &track_iter : stream_info->GetTracks())
	{
		MediaTrack *track = track_iter.second.get();

		serialize.AppendFormat(
			// Serialize VideoTrack
			// framerate|width|height
			"%.6f|%d|%d|"
			// Serialize AudioTrack
			// samplerate|format|layout
			"%d|%d|%d|"
			// Serialize MediaTrack
			// track_id|codec_id|media_type|timebase.num|timebase.den|bitrate|start_frame_time|last_frame_time
			"%d|%d|%d|%d|%d|%d|%lld|%lld\n",
			track->GetFrameRate(), track->GetWidth(), track->GetHeight(),
			track->GetSampleRate(), track->GetSample().GetFormat(), track->GetChannel().GetLayout(),
			track->GetId(), track->GetCodecId(), track->GetMediaType(), track->GetTimeBase().GetNum(), track->GetTimeBase().GetDen(), track->GetBitrate(), track->GetStartFrameTime(), track->GetLastFrameTime()
		);
	}

	logtd("Trying to send a stream information for %s/%s (%u/%u)\n%s...",
	      _application_info.GetName().CStr(), stream_info->GetName().CStr(),
	      _application_info.GetId(), stream_info->GetId(),
	      serialize.CStr()
	);

	RelayPacket response(RelayPacketType::CreateStream);

	if(remote != nullptr)
	{
		// send to specific relay client
		Send(remote, stream_info->GetId(), response, serialize.ToData().get());
	}
	else
	{
		// broadcast
		Send(stream_info->GetId(), response, serialize.ToData().get());
	}
}

bool RelayServer::OnCreateStream(std::shared_ptr<StreamInfo> info)
{
	// Notify to relay client
	logtd("Stream is created: %u, %s", info->GetId(), info->GetName().CStr());

	SendStream(nullptr, info);

	return true;
}

bool RelayServer::OnDeleteStream(std::shared_ptr<StreamInfo> info)
{
	// Notify to relay client
	logtd("Stream is deleted: %u, %s", info->GetId(), info->GetName().CStr());

	Send(info->GetId(), RelayPacket(RelayPacketType::DeleteStream), nullptr);

	return true;
}

bool RelayServer::OnSendVideoFrame(std::shared_ptr<StreamInfo> stream, std::shared_ptr<MediaTrack> track, std::unique_ptr<EncodedFrame> encoded_frame, std::unique_ptr<CodecSpecificInfo> codec_info, std::unique_ptr<FragmentationHeader> fragmentation)
{
	return true;
}

bool RelayServer::OnSendAudioFrame(std::shared_ptr<StreamInfo> stream, std::shared_ptr<MediaTrack> track, std::unique_ptr<EncodedFrame> encoded_frame, std::unique_ptr<CodecSpecificInfo> codec_info, std::unique_ptr<FragmentationHeader> fragmentation)
{
	return true;
}

void RelayServer::OnConnected(ov::Socket *remote)
{
	logtd("New RelayClient is connected: %s", remote->ToString().CStr());
}

void RelayServer::OnDataReceived(ov::Socket *remote, const ov::SocketAddress &address, const std::shared_ptr<const ov::Data> &data)
{
	logtd("Data received from %s: %zu bytes", remote->ToString().CStr(), data->GetLength());

	RelayPacket packet(data.get());

	switch(packet.GetType())
	{
		case RelayPacketType::Register:
			HandleRegister(remote, packet);
			break;

		default:
			OV_ASSERT2(false);
			logte("Invalid packet received from client: %d", packet.GetType());
			break;
	}
}

void RelayServer::OnDisconnected(ov::Socket *remote, PhysicalPortDisconnectReason reason, const std::shared_ptr<const ov::Error> &error)
{
	logtd("RelayClient is disconnected: %s (reason: %d)", remote->ToString().CStr(), reason);

	// remove from _client_list
	auto info_iter = _client_list.find(remote);

	if(info_iter != _client_list.end())
	{
		_client_list.erase(info_iter);
	}
}

void RelayServer::HandleRegister(ov::Socket *remote, const RelayPacket &packet)
{
	// The relay client wants to be registered on this server for the application
	ov::String app_name(reinterpret_cast<const char *>(packet.GetData()), packet.GetDataSize());
	if(_application_info.GetName() != app_name)
	{
		// Cannot handle that application
		logte("Cannot handle %s", app_name.CStr());

		// TODO(dimiden): If multiple RelayServers use the same PhysicalPort, data from other servers can come in here
		// This situation is not assumed at this time, and packet probe function should be added afterward

		RelayPacket response(RelayPacketType::Error);
		remote->Send(&response, sizeof(response));

		return;
	}

	logtd("Registering a relay client %s for application: %s", remote->ToString().CStr(), _application_info.GetName().CStr());
	_client_list[remote] = ClientInfo();

	// Send streams to the relay client
	auto streams = _media_route_application->GetStreams();

	if(streams.empty() == false)
	{
		logtd("Trying to send streams (%zu streams found)...", streams.size());
	}

	for(auto &stream_iter : streams)
	{
		const auto &stream_info = stream_iter.second->GetStreamInfo();

		SendStream(remote, stream_info);
	}
}

void RelayServer::Send(info::stream_id_t stream_id, const RelayPacket &base_packet, const ov::Data *data)
{
	if(_client_list.empty())
	{
		// There is no client to send
		return;
	}

	uint32_t transaction_id = _transaction_id++;

	RelayPacket packet = base_packet;

	packet.SetApplicationId(_application_info.GetId());
	packet.SetStreamId(stream_id);
	packet.SetTransactionId(transaction_id);

	packet.SetStart(true);

	if(data != nullptr)
	{
		ov::ByteStream stream(data);

		while(stream.Remained() > 0)
		{
			size_t read_bytes = stream.Read(static_cast<uint8_t *>(packet.GetData()), RelayPacketDataSize);
			packet.SetDataSize(static_cast<uint16_t>(read_bytes));

			if(stream.Remained() == 0)
			{
				packet.SetEnd(true);
			}

			for(auto &client : _client_list)
			{
				packet.srctime = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
									std::chrono::high_resolution_clock::now().time_since_epoch()).count());

				client.first->Send(&packet, sizeof(packet));
			}

			packet.SetStart(false);
		}
	}
	else
	{
		packet.SetEnd(true);

		for(auto &client : _client_list)
		{
			packet.srctime = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::high_resolution_clock::now().time_since_epoch()).count());

			client.first->Send(&packet, sizeof(packet));
		}
	}
}

void RelayServer::Send(info::stream_id_t stream_id, const RelayPacket &base_packet, const void *data, uint16_t data_size)
{
	if(_client_list.empty())
	{
		return;
	}

	auto data_to_send = ov::Data(data, data_size, true);

	Send(stream_id, base_packet, &data_to_send);
}

void RelayServer::Send(ov::Socket *socket, info::stream_id_t stream_id, const RelayPacket &base_packet, const ov::Data *data)
{
	uint32_t transaction_id = _transaction_id++;

	RelayPacket packet = base_packet;

	packet.SetApplicationId(_application_info.GetId());
	packet.SetStreamId(stream_id);
	packet.SetTransactionId(transaction_id);

	packet.SetStart(true);

	if(data != nullptr)
	{
		// separate the data to multiple packets
		ov::ByteStream stream(data);

		while(stream.Remained() > 0)
		{
			size_t read_bytes = stream.Read(static_cast<uint8_t *>(packet.GetData()), RelayPacketDataSize);
			packet.SetDataSize(static_cast<uint16_t>(read_bytes));

			if(stream.Remained() == 0)
			{
				packet.SetEnd(true);
			}

			packet.srctime = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::high_resolution_clock::now().time_since_epoch()).count());

			socket->Send(&packet, sizeof(packet));

			packet.SetStart(false);
		}
	}
	else
	{
		packet.SetEnd(true);

		packet.srctime = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::high_resolution_clock::now().time_since_epoch()).count());

		socket->Send(&packet, sizeof(packet));
	}
}

void RelayServer::SendMediaPacket(const std::shared_ptr<MediaRouteStream> &media_stream, const MediaPacket *packet)
{
	if(_client_list.empty())
	{
		// Nothing to do
		return;
	}

	auto stream_info = media_stream->GetStreamInfo();
	auto media_track = stream_info->GetTrack(packet->GetTrackId());

	RelayPacket relay_packet(RelayPacketType::Packet);

	relay_packet.SetFragmentHeader(packet->_frag_hdr.get());

	relay_packet.SetMediaType(static_cast<int8_t>(packet->GetMediaType()));
	relay_packet.SetTrackId(static_cast<uint32_t>(packet->GetTrackId()));
	relay_packet.SetPts(static_cast<uint64_t>(packet->GetPts()));
	relay_packet.SetFlags(static_cast<uint8_t>(packet->GetFlags()));

	Send(stream_info->GetId(), relay_packet, packet->GetData().get());
}
