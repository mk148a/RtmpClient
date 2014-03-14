#include "pch.h"
#include "Connection.h"

using namespace Concurrency;
using namespace Windows::Foundation;
using namespace Mntone::Rtmp;

Connection::Connection()
	: IsInitialized_( false )
{ }

task<void> Connection::ConnectAsync( Platform::String^ host, Platform::String^ port )
{
	using namespace Windows::Networking;
	using namespace Windows::Networking::Sockets;

	streamSocket_ = ref new StreamSocket();
	auto task = streamSocket_->ConnectAsync( ref new HostName( host ), port, SocketProtectionLevel::PlainSocket );
	return create_task( task ).then( [=]
	{
		dataWriter_ = ref new Windows::Storage::Streams::DataWriter( streamSocket_->OutputStream );

		IsInitialized_ = true;
	} );
}

void Connection::Read( const uint32 length, ConnectionCallbackHandler^ callbackFunction )
{
	using namespace Windows::Storage::Streams;

	auto buffer = ref new Buffer( length );
	auto read_operation = streamSocket_->InputStream->ReadAsync( buffer, length, InputStreamOptions::Partial );
	read_operation->Completed = ref new AsyncOperationWithProgressCompletedHandler<IBuffer^, uint32>(
	[this, length, callbackFunction]( IAsyncOperationWithProgress<IBuffer^, uint32>^ operation, AsyncStatus status )
	{
		if( status == AsyncStatus::Completed )
		{
			auto buffer = operation->GetResults();
			callbackFunction( buffer );
		}
	} );
	ReadOperationChanged( this, read_operation );
}

Concurrency::task<void> Connection::Write( const uint8* const data, const size_t length )
{
	dataWriter_->WriteBytes( Platform::ArrayReference<uint8>( const_cast<uint8_t*>( data ), static_cast<uint32>( length ) ) );
	return Concurrency::create_task( dataWriter_->StoreAsync() ).then( [] ( const uint32 ) { } );
}

void Connection::CloseImpl()
{
	if( dataWriter_ != nullptr )
	{
		delete dataWriter_;
		dataWriter_ = nullptr;
	}
	if( streamSocket_ != nullptr )
	{
		delete streamSocket_;
		streamSocket_ = nullptr;
	}
}