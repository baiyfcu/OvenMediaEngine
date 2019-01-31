//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Jaejong Bong
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include "rtmp_chunk_stream.h"
#include <base/ovsocket/ovsocket.h>
#include <physical_port/physical_port_manager.h>
#include "rtmp_observer.h"

//====================================================================================================
// RtmpServer
//====================================================================================================
class RtmpServer : protected PhysicalPortObserver, public IRtmpChunkStream
{
public:
    RtmpServer() = default;

    virtual ~RtmpServer();

public:
    bool Start(const ov::SocketAddress &address);

    bool Stop();

    bool AddObserver(const std::shared_ptr<RtmpObserver> &observer);

    bool RemoveObserver(const std::shared_ptr<RtmpObserver> &observer);

    bool Disconnect(const ov::String &app_name, uint32_t stream_id);

    void OnGarbageCheck();

    void MainTask();

protected:
    //--------------------------------------------------------------------
    // Implementation of PhysicalPortObserver
    //--------------------------------------------------------------------
    void OnConnected(ov::Socket *remote) override;

    void OnDataReceived(ov::Socket *remote,
                        const ov::SocketAddress &address,
                        const std::shared_ptr<const ov::Data> &data) override;

    void OnDisconnected(ov::Socket *remote,
                        PhysicalPortDisconnectReason reason,
                        const std::shared_ptr<const ov::Error> &error) override;

    //--------------------------------------------------------------------
    // Implementation of IRtmpChunkStream
    //--------------------------------------------------------------------
    bool OnChunkStreamReadyComplete(ov::ClientSocket *remote,
                                    ov::String &app_name,
                                    ov::String &stream_name,
                                    std::shared_ptr<RtmpMediaInfo> &media_info,
                                    info::application_id_t &application_id,
                                    uint32_t &stream_id) override;

    bool OnChunkStreamVideoData(ov::ClientSocket *remote,
                                info::application_id_t application_id,
                                uint32_t stream_id,
                                uint32_t timestamp,
                                std::shared_ptr<std::vector<uint8_t>> &data,
                                RtmpFrameType frame_type) override;

    bool OnChunkStreamAudioData(ov::ClientSocket *remote,
                                info::application_id_t application_id,
                                uint32_t stream_id,
                                uint32_t timestamp,
                                std::shared_ptr<std::vector<uint8_t>> &data) override;

    bool OnDeleteStream(ov::ClientSocket *remote,
                        ov::String &app_name,
                        ov::String &stream_name,
                        info::application_id_t application_id,
                        uint32_t stream_id) override;

private :
    std::shared_ptr<PhysicalPort> _physical_port;
    std::map<ov::Socket *, std::shared_ptr<RtmpChunkStream>> _chunk_stream_list;
    std::vector<std::shared_ptr<RtmpObserver>> _observers;

    volatile bool _thread_kill_flag;
    std::thread _thread;
    std::recursive_mutex _chunk_stream_list_mutex; // 동일 Thread에서 접속 가능

};
