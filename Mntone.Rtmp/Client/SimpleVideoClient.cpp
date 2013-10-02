#include "pch.h"
#include "SimpleVideoClient.h"

using namespace Mntone::Rtmp;
using namespace Mntone::Rtmp::Client;
namespace WF = Windows::Foundation;

SimpleVideoClient::SimpleVideoClient( void ) :
	_dispatcher( Windows::UI::Core::CoreWindow::GetForCurrentThread()->Dispatcher )
{ }

SimpleVideoClient::~SimpleVideoClient( void )
{
	__Close();
}

void SimpleVideoClient::__Close( void )
{
	if( _connection != nullptr )
	{
		delete _connection;
		_connection = nullptr;

		if( _stream != nullptr )
		{
			delete _stream;
			_stream = nullptr;
		}

		Stopped( this, ref new SimpleVideoClientStoppedEventArgs() );
	}
}

void SimpleVideoClient::Connect( WF::Uri^ uri )
{
	Connect( ref new RtmpUri( uri ) );
}

void SimpleVideoClient::Connect( RtmpUri^ uri )
{
	_connection = ref new NetConnection();
	_connection->StatusUpdated += ref new WF::EventHandler<NetStatusUpdatedEventArgs^>( this, &SimpleVideoClient::OnNetConnectionStatusUpdated );
	_connection->Connect( uri );
}

void SimpleVideoClient::OnNetConnectionStatusUpdated( Platform::Object^ sender, NetStatusUpdatedEventArgs^ args )
{
	auto ns = args->NetStatus;
	if( ns == NetStatusType::NetConnection_Connect_Success )
	{
		_stream = ref new NetStream();
		_stream->Attached += ref new WF::EventHandler<NetStreamAttachedEventArgs^>( this, &SimpleVideoClient::OnAttached );
		_stream->StatusUpdated += ref new WF::EventHandler<NetStatusUpdatedEventArgs^>( this, &SimpleVideoClient::OnNetStreamStatusUpdated );
		_stream->AudioReceived += ref new WF::EventHandler<NetStreamAudioReceivedEventArgs^>( this, &SimpleVideoClient::OnAudioReceived );
		_stream->VideoReceived += ref new WF::EventHandler<NetStreamVideoReceivedEventArgs^>( this, &SimpleVideoClient::OnVideoReceived );
		_stream->Attach( _connection );
	}
	else if( ( ns & NetStatusType::NetConnection_Connect ) == NetStatusType::NetConnection_Connect )
		__Close();
}

void SimpleVideoClient::OnAttached( Platform::Object^ sender, NetStreamAttachedEventArgs^ args )
{
	_stream->Play( _connection->Uri->Instance );
}

void SimpleVideoClient::OnNetStreamStatusUpdated( Platform::Object^ sender, NetStatusUpdatedEventArgs^ args )
{
	auto ns = args->NetStatus;
	if( ns == NetStatusType::NetStream_Play_Start )
	{
		using namespace Windows::UI::Core;
		_dispatcher->RunAsync( CoreDispatcherPriority::Normal, ref new DispatchedHandler( [this]
		{
			using namespace Windows::Media::Core;
			using namespace Windows::Foundation;
			using namespace Windows::Media::MediaProperties;

			auto audio = AudioEncodingProperties::CreateMp3( 44100, 2, 112 );
			//auto audio = AudioEncodingProperties::CreateAac( 44100, 2, 112 );
			audio->BitsPerSample = 16;
			const auto ades = ref new AudioStreamDescriptor( audio );

			auto video = VideoEncodingProperties::CreateH264();
			video->ProfileId = H264ProfileIds::High;
			video->Bitrate = 1024;
			video->FrameRate->Numerator = 24000;
			video->FrameRate->Denominator = 1000;
			video->Height = 720;
			video->Width = 1280;
			auto vdes = ref new VideoStreamDescriptor( video );

			_mediaStreamSource = ref new MediaStreamSource( vdes, ades );

			TimeSpan d;
			d.Duration = INT64_MAX;
			_mediaStreamSource->Duration = d;

			_mediaStreamSource->Starting += ref new TypedEventHandler<MediaStreamSource^, MediaStreamSourceStartingEventArgs^>( this, &SimpleVideoClient::OnStarting );
			_mediaStreamSource->SampleRequested += ref new TypedEventHandler<MediaStreamSource^, MediaStreamSourceSampleRequestedEventArgs^>( this, &SimpleVideoClient::OnSampleRequested );
			Started( this, ref new SimpleVideoClientStartedEventArgs( _mediaStreamSource ) );
		} ) );
	}
}

void SimpleVideoClient::OnAudioReceived( Platform::Object^ sender, NetStreamAudioReceivedEventArgs^ args )
{
	std::lock_guard<std::mutex> lock( _audioMutex );
	_audioBuffer.emplace( args );
	_audioConditionVariable.notify_one();
}

void SimpleVideoClient::OnVideoReceived( Platform::Object^ sender, NetStreamVideoReceivedEventArgs^ args )
{
	std::lock_guard<std::mutex> lock( _videoMutex );
	_videoBuffer.emplace( args );
	_videoConditionVariable.notify_one();
}

void SimpleVideoClient::OnStarting( Windows::Media::Core::MediaStreamSource^ sender, Windows::Media::Core::MediaStreamSourceStartingEventArgs^ args )
{
	auto request = args->Request;
	Windows::Foundation::TimeSpan ts;
	ts.Duration = 0;
	request->SetActualStartPosition( ts );
}

void SimpleVideoClient::OnSampleRequested( Windows::Media::Core::MediaStreamSource^ sender, Windows::Media::Core::MediaStreamSourceSampleRequestedEventArgs^ args )
{
	auto request = args->Request;
	auto deferral = request->GetDeferral();

	auto className = request->StreamDescriptor->ToString();
	if( className == "Windows.Media.Core.AudioStreamDescriptor" )
	{
		NetStreamAudioReceivedEventArgs^ data;
		{
			std::unique_lock<std::mutex> lock( _audioMutex );
			while( _audioBuffer.empty() )
				_audioConditionVariable.wait( lock );
			data = _audioBuffer.front();
			_audioBuffer.pop();
		}

		auto sample = Windows::Media::Core::MediaStreamSample::CreateFromBuffer( data->Data, data->Timestamp );
		sample->KeyFrame = true;
		request->Sample = sample;

		//WF::TimeSpan d;
		//d.Duration = data->Duration * 10000L;
		//sample->Duration = d;

		//Platform::String^ str( "Audio - TS: " + data->Timestamp + "\n" );
		//OutputDebugString( str->Data() );
	}
	else
	{
		NetStreamVideoReceivedEventArgs^ data;
		{
			std::unique_lock<std::mutex> lock( _videoMutex );
			while( _videoBuffer.empty() )
				_videoConditionVariable.wait( lock );
			data = _videoBuffer.front();
			_videoBuffer.pop();
		}

		auto sample = Windows::Media::Core::MediaStreamSample::CreateFromBuffer( data->Data, data->PresentationTimestamp );
		sample->DecodeTimestamp = data->DecodeTimestamp;
		sample->KeyFrame = data->IsKeyframe;
		request->Sample = sample;

		//WF::TimeSpan d;
		//d.Duration = 10 * 10000L;
		//sample->Duration = d;

		//Platform::String^ str( "Video - DTS: " + data->DecodeTimestamp + ", PTS: " + data->PresentationTimestamp + "\n" );
		//OutputDebugString( str->Data() );
	}
	deferral->Complete();
}