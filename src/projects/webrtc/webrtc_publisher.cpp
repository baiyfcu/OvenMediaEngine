#include <utility>

#include "rtc_private.h"
#include "webrtc_publisher.h"
#include "rtc_stream.h"
#include "rtc_session.h"

#include "config/config_manager.h"

std::shared_ptr<WebRtcPublisher> WebRtcPublisher::Create(const info::Application &application_info, std::shared_ptr<MediaRouteInterface> router, std::shared_ptr<MediaRouteApplicationInterface> application)
{
	auto webrtc = std::make_shared<WebRtcPublisher>(application_info, router, application);

	// CONFIG을 불러온다.
	webrtc->Start();

	return webrtc;
}

WebRtcPublisher::WebRtcPublisher(const info::Application &application_info, std::shared_ptr<MediaRouteInterface> router, std::shared_ptr<MediaRouteApplicationInterface> application)
	: Publisher(application_info, std::move(router))
{
	_application = application;
}

WebRtcPublisher::~WebRtcPublisher()
{
	logtd("WebRtcPublisher has been terminated finally");
}

/*
 * Publisher Implementation
 */

bool WebRtcPublisher::Start()
{
	// Find WebRTC publisher configuration
	_publisher_info = FindPublisherInfo<cfg::WebrtcPublisher>();

	if(_publisher_info == nullptr)
	{
		logte("Cannot initialize WebrtcPublisher using config information");
		return false;
	}

	// _ice_port 생성
	ov::SocketType socket_type;

	if(GetCandidateProto().UpperCaseString() == "UDP")
	{
		socket_type = ov::SocketType::Udp;
	}
	else if(GetCandidateProto().UpperCaseString() == "TCP")
	{
		socket_type = ov::SocketType::Tcp;
	}
	else
	{
		return false;
	}

	_ice_port = IcePortManager::Instance()->CreatePort(socket_type, ov::SocketAddress(GetCandidatePort()), IcePortObserver::GetSharedPtr());
	_signalling = std::make_shared<RtcSignallingServer>(_application_info, _application);

	if(_ice_port == nullptr || _signalling == nullptr)
	{
		// Something wrokng...
		logte("Failed to start publisher");
		return false;
	}

	std::shared_ptr<Certificate> certificate = GetCertificate();
	std::shared_ptr<Certificate> chain_certificate = GetChainCertificate();

	// Signalling에 Observer 연결
	_signalling->AddObserver(RtcSignallingObserver::GetSharedPtr());
	_signalling->Start(ov::SocketAddress(GetSignallingPort()), certificate, chain_certificate);

	// Publisher::Start()에서 Application을 생성한다.
	return Publisher::Start();
}

bool WebRtcPublisher::Stop()
{
	_ice_port.reset();
	_signalling.reset();

	return Publisher::Stop();
}

// Publisher에서 Application 생성 요청이 온다.
std::shared_ptr<Application> WebRtcPublisher::OnCreateApplication(const info::Application &application_info)
{
	return RtcApplication::Create(application_info, _ice_port, _signalling);
}

/*
 * Signalling Implementation
 */


// 클라이언트가 Request Offer를 하면 다음 함수를 통해 SDP를 받아서 넘겨준다.
std::shared_ptr<SessionDescription> WebRtcPublisher::OnRequestOffer(const ov::String &application_name, const ov::String &stream_name, std::vector<RtcIceCandidate> *ice_candidates)
{
	// Application -> Stream에서 SDP를 얻어서 반환한다.
	auto stream = std::static_pointer_cast<RtcStream>(GetStream(application_name, stream_name));
	if(!stream)
	{
		return nullptr;
	}

	ice_candidates->push_back(RtcIceCandidate(GetCandidateProto(), ov::SocketAddress(GetCandidateIP(), GetCandidatePort()), 0, ""));

	auto session_description = std::make_shared<SessionDescription>(*stream->GetSessionDescription());

	session_description->SetIceUfrag(_ice_port->GenerateUfrag());
	session_description->Update();

	return session_description;
}

// 클라이언트가 자신의 SDP를 보내면 다음 함수를 호출한다.
bool WebRtcPublisher::OnAddRemoteDescription(const ov::String &application_name,
                                             const ov::String &stream_name,
                                             const std::shared_ptr<SessionDescription> &offer_sdp,
                                             const std::shared_ptr<SessionDescription> &peer_sdp)
{
	auto application = GetApplicationByName(application_name);
	auto stream = GetStream(application_name, stream_name);

	if(!stream)
	{
		logte("Cannot find stream (%s/%s)", application_name.CStr(), stream_name.CStr());
		return false;
	}

	ov::String remote_sdp_text;
	peer_sdp->ToString(remote_sdp_text);
	logtd("OnAddRemoteDescription: %s", remote_sdp_text.CStr());

	// Stream에 Session을 생성한다.
	auto session = RtcSession::Create(application, stream, offer_sdp, peer_sdp, _ice_port);

	if(session != nullptr)
	{
		// Stream에 Session을 등록한다.
		stream->AddSession(session);

		// ice_port에 SessionInfo을 전달한다.
		// 향후 해당 session에서 Ice를 통해 패킷이 들어오면 SessionInfo와 함께 Callback을 준다.
		_ice_port->AddSession(session, offer_sdp, peer_sdp);
	}
	else
	{
		// peer_sdp가 잘못되거나, 다른 이유로 인해 session을 생성하지 못함
		logte("Cannot create session");
	}

	return true;
}

bool WebRtcPublisher::OnStopCommand(const ov::String &application_name, const ov::String &stream_name,
                                    const std::shared_ptr<SessionDescription> &offer_sdp,
                                    const std::shared_ptr<SessionDescription> &peer_sdp)
{
	// 플레이어에서 stop 이벤트가 수신 된 경우 처리
	logtd("Stop commnad received : %s/%s/%u", application_name.CStr(), stream_name.CStr(), peer_sdp->GetSessionId());
	// Find Stream
	auto stream = std::static_pointer_cast<RtcStream>(GetStream(application_name, stream_name));
	if(!stream)
	{
		logte("To stop session failed. Cannot find stream (%s/%s)", application_name.CStr(), stream_name.CStr());
		return false;
	}

	// Peer SDP의 Session ID로 세션을 찾는다.
	auto session = stream->GetSession(peer_sdp->GetSessionId());
	if(session == nullptr)
	{
		logte("To stop session failed. Cannot find session by peer sdp session id (%u)", peer_sdp->GetSessionId());
		return false;
	}

	// Session을 Stream에서 정리한다.
	stream->RemoveSession(session->GetId());

	// IcePort에서 Remove 한다.
	_ice_port->RemoveSession(session);

	return true;
}

// It does not be used because
bool WebRtcPublisher::OnIceCandidate(const ov::String &application_name,
                                     const ov::String &stream_name,
                                     const std::shared_ptr<RtcIceCandidate> &candidate,
                                     const ov::String &username_fragment)
{
	return true;
}

/*
 * IcePort Implementation
 */

void WebRtcPublisher::OnStateChanged(IcePort &port, const std::shared_ptr<SessionInfo> &session_info,
                                     IcePortConnectionState state)
{
	logtd("IcePort OnStateChanged : %d", state);

	auto session = std::static_pointer_cast<RtcSession>(session_info);
	auto application = session->GetApplication();
	auto stream = session->GetStream();

	// state를 보고 session을 처리한다.
	switch(state)
	{
		case IcePortConnectionState::New:
		case IcePortConnectionState::Checking:
		case IcePortConnectionState::Connected:
		case IcePortConnectionState::Completed:
			// 연결되었을때는 할일이 없다.
			break;
		case IcePortConnectionState::Failed:
		case IcePortConnectionState::Disconnected:
		case IcePortConnectionState::Closed:

			// Session을 Stream에서 정리한다.
			stream->RemoveSession(session->GetId());

			// Signalling에 종료 명령을 내린다.
			_signalling->Disconnect(application->GetName(), stream->GetName(), session->GetPeerSDP());
			break;
	}
}

void WebRtcPublisher::OnDataReceived(IcePort &port, const std::shared_ptr<SessionInfo> &session_info, std::shared_ptr<const ov::Data> data)
{
	// ice_port를 통해 STUN을 제외한 모든 Packet이 들어온다.
	auto session = std::static_pointer_cast<Session>(session_info);

	//받는 Data 형식을 협의해야 한다.
	auto application = session->GetApplication();
	application->PushIncomingPacket(session_info, data);
}

uint16_t WebRtcPublisher::GetSignallingPort()
{
	int port = _publisher_info->GetSignalling().GetPort();

	if(cfg::ConfigManager::edge_siginaling_port==0)
	{
		return static_cast<uint16_t>((port == 0) ? 3333 : port);
	}

	logtw("edge_siginaling_port=%d, port=%d", cfg::ConfigManager::edge_siginaling_port, port);
	return static_cast<uint16_t>((port == 0) ? cfg::ConfigManager::edge_siginaling_port : port);
}

ov::String WebRtcPublisher::GetCandidateIP()
{
	ov::String ip = _publisher_info->GetIp();

	return ip.IsEmpty() ? "127.0.0.1" : ip;
}

uint16_t WebRtcPublisher::GetCandidatePort()
{
	auto tokens = _publisher_info->GetPort().Split("/");

	int port = ov::Converter::ToUInt16(tokens[0]);

	if (cfg::ConfigManager::edge_candidate_port==0)
	{
		return static_cast<uint16_t>((port == 0) ? 10000 : port);
	}

	logtw("edge_candidate_port=%d, port=%d", cfg::ConfigManager::edge_candidate_port, port);
	return static_cast<uint16_t>((port == 0) ? cfg::ConfigManager::edge_candidate_port : port);
}

ov::String WebRtcPublisher::GetCandidateProto()
{
	auto tokens = _publisher_info->GetPort().Split("/");

	if(tokens.size() >= 2)
	{
		return tokens[1].UpperCaseString();
	}

	return "UDP";
}

std::shared_ptr<Certificate> WebRtcPublisher::GetCertificate()
{
	cfg::Tls tls_info = _publisher_info->GetSignalling().GetTls();

	if(
		(tls_info.GetCertPath().IsEmpty() == false) &&
		(tls_info.GetKeyPath().IsEmpty() == false)
		)
	{
		auto certificate = std::make_shared<Certificate>();

		logti("Trying to create a certificate using files\n\tCert path: %s\n\tPrivate key path: %s",
		      tls_info.GetCertPath().CStr(),
		      tls_info.GetKeyPath().CStr()
		);

		auto error = certificate->GenerateFromPem(tls_info.GetCertPath(), tls_info.GetKeyPath());

		if(error == nullptr)
		{
			return certificate;
		}

		logte("Could not create a certificate from files: %s", error->ToString().CStr());
	}
	else
	{
		// TLS is disabled
	}

	return nullptr;
}

std::shared_ptr<Certificate> WebRtcPublisher::GetChainCertificate()
{
	cfg::Tls tls_info = _publisher_info->GetSignalling().GetTls();

	if(
		(tls_info.GetChainCertPath().IsEmpty() == false)
		)
	{
		auto certificate = std::make_shared<Certificate>();

		logti("Trying to create a chain certificate using file: %s",
		      tls_info.GetChainCertPath().CStr()
		);

		auto error = certificate->GenerateFromPem(tls_info.GetChainCertPath(), true);

		if(error == nullptr)
		{
			return certificate;
		}

		logte("Could not create a chain certificate from file: %s", error->ToString().CStr());
	}
	else
	{
		// TLS is disabled
	}

	return nullptr;
}
