#include "pch.h"
#include "NetConnection.h"
#include "NetStream.h"
#include "RtmpHelper.h"
#include "Command/NetConnectionConnectCommand.h"

using namespace Concurrency;
using namespace Windows::Foundation;
using namespace Windows::Storage::Streams;
using namespace mntone::rtmp;
using namespace Mntone::Rtmp;

const auto DEFAULT_WINDOW_SIZE = std::numeric_limits<uint32>::max();
const auto DEFAULT_LIMIT_TYPE = limit_type::hard;
const auto DEFAULT_CHUNK_SIZE = 128;
const auto DEFAULT_BUFFER_MILLSECONDS = 5000;

NetConnection::NetConnection()
	: connection_( ref new Connection() )
	, latestTransactionId_( 2 )
	, receiveOperation_( nullptr )
	, rxHeaderBuffer_( 11 )
	, rxWindowSize_( DEFAULT_WINDOW_SIZE ), txWindowSize_( DEFAULT_WINDOW_SIZE )
	, rxLimitType_( DEFAULT_LIMIT_TYPE ), txLimitType_( DEFAULT_LIMIT_TYPE )
	, rxChunkSize_( DEFAULT_CHUNK_SIZE ), txChunkSize_( DEFAULT_CHUNK_SIZE )
{
	connection_->ReadOperationChanged += ref new TypedEventHandler<Connection^, IAsyncOperationWithProgress<Windows::Storage::Streams::IBuffer^, uint32>^>( this, &NetConnection::OnReadOperationChanged );
}

void NetConnection::CloseImpl()
{
	if( receiveOperation_ != nullptr )
	{
		receiveOperation_->Cancel();
		receiveOperation_ = nullptr;
	}
	connection_ = nullptr;
	Closed( this, ref new NetConnectionClosedEventArgs() );
}
	
IAsyncAction^ NetConnection::ConnectAsync( Windows::Foundation::Uri^ uri )
{
	return ConnectAsync( ref new RtmpUri( uri ) );
}

IAsyncAction^ NetConnection::ConnectAsync( Windows::Foundation::Uri^ uri, Command::NetConnectionConnectCommand^ command )
{
	if( command->Type != "connect" )
	{
		throw ref new Platform::InvalidArgumentException();
	}

	return ConnectAsync( ref new RtmpUri( uri ), command );
}

IAsyncAction^ NetConnection::ConnectAsync( RtmpUri^ uri )
{
	auto connect = ref new Command::NetConnectionConnectCommand( uri->App );
	connect->TcUrl = uri->ToString();
	return ConnectAsync( uri, connect );
}

IAsyncAction^ NetConnection::ConnectAsync( RtmpUri^ uri, Command::NetConnectionConnectCommand^ command )
{
	startTime_ = utility::get_windows_time();
	Uri_ = uri;

	return create_async( [this, command]
	{
		auto task = connection_->ConnectAsync( Uri_->Host, Uri_->Port.ToString() );
		return task.then( [this, command]
		{
			Handshake( ref new HandshakeCallbackHandler( [this, command]
			{
				SendActionAsync( command->Commandify() );
				Receive();
			} ) );
		}, task_continuation_context::use_arbitrary() );
	} );
}

IAsyncAction^ NetConnection::CallAsync( Command::NetConnectionCallCommand^ command )
{
	return create_async( [this, command]
	{
		const auto tid = latestTransactionId_++;
		command->TransactionId = tid;
		return SendActionAsync( 0, command->Commandify() );
	} );
}

task<void> NetConnection::AttachNetStreamAsync( NetStream^ stream )
{
	using namespace Mntone::Data::Amf;

	stream->parent_ = this;

	const auto tid = latestTransactionId_++;
	netStreamTemporary_.emplace( tid, stream );

	auto cmd = ref new AmfArray();
	cmd->Append( AmfValue::CreateStringValue( "createStream" ) );				// Command name
	cmd->Append( AmfValue::CreateNumberValue( static_cast<float64>( tid ) ) );	// Transaction id
	cmd->Append( ref new AmfValue() );											// Command object: set to null type when do not exist
	return SendActionAsync( cmd );
}

void NetConnection::UnattachNetStream( NetStream^ stream )
{
	using namespace Mntone::Data::Amf;

	bindingNetStream_.erase( stream->streamId_ );
}

#pragma region Network operation (Server to Client)

void NetConnection::OnReadOperationChanged( Connection^ sender, IAsyncOperationWithProgress<Windows::Storage::Streams::IBuffer^, uint32>^ operation )
{
	receiveOperation_ = operation;
}

void NetConnection::Receive()
{
	connection_->Read( 1, ref new ConnectionCallbackHandler( this, &NetConnection::ReceiveHeader1Impl ) );
}

void NetConnection::ReceiveHeader1Impl( IBuffer^ result )
{
	auto reader = DataReader::FromBuffer( result );
	reader->ByteOrder = ByteOrder::BigEndian;

	const auto type_and_id = reader->ReadByte();
	const uint8 format_type = ( type_and_id >> 6 ) & 0x03;
	const uint16 chunk_stream_id = type_and_id & 0x3f;
	if( chunk_stream_id == 0 )
	{
		connection_->Read( 1, ref new ConnectionCallbackHandler( [this, format_type]( IBuffer^ result )
		{
			auto reader = DataReader::FromBuffer( result );
			reader->ByteOrder = ByteOrder::BigEndian;

			const uint16 chunk_stream_id = reader->ReadByte() + 64;
			ReceiveHeader2Impl( format_type, chunk_stream_id );
		} ) );
	}
	else if( chunk_stream_id == 1 )
	{
		connection_->Read( 2, ref new ConnectionCallbackHandler( [this, format_type]( IBuffer^ result )
		{
			auto reader = DataReader::FromBuffer( result );
			reader->ByteOrder = ByteOrder::BigEndian;

			const uint16 chunk_stream_id = reader->ReadUInt16() + 64;
			ReceiveHeader2Impl( format_type, chunk_stream_id );
		} ) );
	}
	else
	{
		ReceiveHeader2Impl( format_type, chunk_stream_id );
	}
}

void NetConnection::ReceiveHeader2Impl( const uint8 format_type, const uint16 chunk_stream_id )
{
	// ---[ Get object ]----------
	std::shared_ptr<rtmp_packet> packet;
	const auto& itr = rxBakPackets_.lower_bound( chunk_stream_id );
	if( itr != rxBakPackets_.end() && itr->first == chunk_stream_id )
	{
		packet = itr->second;
	}
	else
	{
		packet = std::make_shared<rtmp_packet>( chunk_stream_id );
		rxBakPackets_.emplace_hint( itr, chunk_stream_id, packet );
	}

	// ---[ Read header body ]----------
	switch( format_type )
	{
	case 0:
		connection_->Read( 11, ref new ConnectionCallbackHandler( [this, packet]( IBuffer^ result )
		{
			auto reader = DataReader::FromBuffer( result );
			reader->ByteOrder = ByteOrder::BigEndian;
			packet->timestamp_ = RtmpHelper::ReadUint24( reader );
			packet->length_ = RtmpHelper::ReadUint24( reader );
			packet->type_id_ = static_cast<type_id_type>( reader->ReadByte() );
			
			reader->ByteOrder = ByteOrder::LittleEndian;
			packet->stream_id_ = reader->ReadUInt32();

			if( packet->timestamp_ == 0xffffff )
			{
				connection_->Read( 4, ref new ConnectionCallbackHandler( [this, packet]( IBuffer^ result )
				{
					auto reader = DataReader::FromBuffer( result );
					reader->ByteOrder = ByteOrder::BigEndian;
					packet->timestamp_ = reader->ReadUInt32();
					ReceiveBodyImpl( packet );
				} ) );
			}
			else
			{
				ReceiveBodyImpl( packet );
			}
		} ) );
		break;
	
	case 1:
		connection_->Read( 7, ref new ConnectionCallbackHandler( [this, packet]( IBuffer^ result )
		{
			auto reader = DataReader::FromBuffer( result );
			reader->ByteOrder = ByteOrder::BigEndian;
			packet->timestamp_delta_ = RtmpHelper::ReadUint24( reader );
			packet->length_ = RtmpHelper::ReadUint24( reader );
			packet->type_id_ = static_cast<type_id_type>( reader->ReadByte() );

			if( packet->timestamp_delta_ == 0xffffff )
			{
				connection_->Read( 4, ref new ConnectionCallbackHandler( [this, packet]( IBuffer^ result )
				{
					auto reader = DataReader::FromBuffer( result );
					reader->ByteOrder = ByteOrder::BigEndian;
					packet->timestamp_delta_ = reader->ReadUInt32();
					if( packet->temporary_length_ == 0 )
					{
						packet->timestamp_ += packet->timestamp_delta_;
					}
					ReceiveBodyImpl( packet );
				} ) );
			}
			else
			{
				if( packet->temporary_length_ == 0 )
				{
					packet->timestamp_ += packet->timestamp_delta_;
				}
				ReceiveBodyImpl( packet );
			}
		} ) );
		break;
	
	case 2:
		connection_->Read( 3, ref new ConnectionCallbackHandler( [this, packet]( IBuffer^ result )
		{
			auto reader = DataReader::FromBuffer( result );
			reader->ByteOrder = ByteOrder::BigEndian;
			packet->timestamp_delta_ = RtmpHelper::ReadUint24( reader );

			if( packet->timestamp_delta_ == 0xffffff )
			{
				connection_->Read( 4, ref new ConnectionCallbackHandler( [this, packet]( IBuffer^ result )
				{
					auto reader = DataReader::FromBuffer( result );
					reader->ByteOrder = ByteOrder::BigEndian;
					packet->timestamp_delta_ = reader->ReadUInt32();
					if( packet->temporary_length_ == 0 )
					{
						packet->timestamp_ += packet->timestamp_delta_;
					}
					ReceiveBodyImpl( packet );
				} ) );
			}
			else
			{
				if( packet->temporary_length_ == 0 )
				{
					packet->timestamp_ += packet->timestamp_delta_;
				}
				ReceiveBodyImpl( packet );
			}
		} ) );
		break;
	
	case 3:
		if( packet->timestamp_delta_ > 0xffffff )
		{
			connection_->Read( 4, ref new ConnectionCallbackHandler( [this, packet]( IBuffer^ result )
			{
				auto reader = DataReader::FromBuffer( result );
				reader->ByteOrder = ByteOrder::BigEndian;
				packet->timestamp_delta_ = reader->ReadUInt32();
				if( packet->temporary_length_ == 0 )
				{
					packet->timestamp_ += packet->timestamp_delta_;
				}
				ReceiveBodyImpl( packet );
			} ) );
		}
		else
		{
			if( packet->temporary_length_ == 0 )
			{
				packet->timestamp_ += packet->timestamp_delta_;
			}
			ReceiveBodyImpl( packet );
		}
		break;
	}
}

void NetConnection::ReceiveBodyImpl( const std::shared_ptr<rtmp_packet> packet )
{
	const auto& body_length = std::min<uint32_t>( rxChunkSize_, packet->length_ - packet->temporary_length_ );
	if( body_length != 0 )
	{
		if( packet->temporary_length_ == 0 )
		{
			packet->body_ = std::make_shared<std::vector<uint8>>( packet->length_ );
		}

		connection_->Read( body_length, ref new ConnectionCallbackHandler( [this, packet]( IBuffer^ result )
		{
			auto reader = DataReader::FromBuffer( result );
			const auto& data_size = reader->UnconsumedBufferLength;
			reader->ReadBytes( Platform::ArrayReference<uint8>( packet->body_->data() + packet->temporary_length_, data_size ) );
			packet->temporary_length_ += data_size;

			if( packet->temporary_length_ == packet->length_ )
			{
				packet->temporary_length_ = 0;
				const auto data = *packet->body_.get();
				packet->body_.reset();
				ReceiveCallbackImpl( packet, std::move( data ) );
			}
			else
			{
				Receive();
			}
		} ) );
	}
	else
	{
		Receive();
	}
}

void NetConnection::ReceiveCallbackImpl( const std::shared_ptr<const rtmp_packet> packet, const std::vector<uint8> result )
{
	const auto& sid = packet->stream_id_;
	if( sid == 0 )
	{
		OnMessage( *packet.get(), std::move( result ) );
	}
	else
	{
		const auto& itr = bindingNetStream_.lower_bound( sid );
		if( itr != bindingNetStream_.cend() && itr->first == sid )
		{
			itr->second->OnMessage( *packet.get(), std::move( result ) );
		}
	}
	Receive();
}

void NetConnection::OnMessage( const rtmp_packet packet, std::vector<uint8> data )
{
	if( packet.chunk_stream_id_ == 2 )
	{
		OnNetworkMessage( std::move( packet ), std::move( data ) );
		return;
	}

	switch( packet.type_id_ )
	{
	case type_id_type::command_message_amf3:
	case type_id_type::command_message_amf0:
		OnCommandMessage( std::move( packet ), std::move( data ) );
		break;
	}
}

void NetConnection::OnNetworkMessage( const rtmp_packet packet, std::vector<uint8> data )
{
	switch( packet.type_id_ )
	{
	case type_id_type::set_chunk_size:
		utility::convert_big_endian( &data[0], 4, &rxChunkSize_ );
		break;

	case type_id_type::abort_message:
		break;

	case type_id_type::acknowledgement:
		break;

	case type_id_type::user_control_message:
		OnUserControlMessage( std::move( packet ), std::move( data ) );
		break;

	case type_id_type::window_acknowledgement_size:
		utility::convert_big_endian( &data[0], 4, &rxWindowSize_ );
		break;

	case type_id_type::set_peer_bandwidth:
		OnSetPeerBandwidthMessage( std::move( packet ), std::move( data ) );
		break;
	}
}

void NetConnection::OnUserControlMessage( const rtmp_packet /*packet*/, std::vector<uint8> data )
{
	uint16 buf;
	utility::convert_big_endian( &data[0], 2, &buf );

	const auto& message_type = static_cast<UserControlMessageEventType>( buf );
	switch( message_type )
	{
	case UserControlMessageEventType::StreamBegin:
		{
			uint32 stream_id;
			utility::convert_big_endian( &data[2], 4, &stream_id );
			if( stream_id == 0 )
			{
				SetBufferLengthAsync( 0, DEFAULT_BUFFER_MILLSECONDS );
			}
			break;
		}

	case UserControlMessageEventType::StreamEof:
		{
			//uint32 stream_id;
			//utility::convert_big_endian( &data[2], 4, &stream_id );
			break;
		}

	case UserControlMessageEventType::StreamDry:
	case UserControlMessageEventType::StreamIsRecorded:
		{
			//uint32 stream_id;
			//utility::convert_big_endian( &data[2], 4, &stream_id );
			break;
		}

	case UserControlMessageEventType::PingRequest:
		{
			uint32 timestamp;
			utility::convert_big_endian( &data[2], 4, &timestamp );
			PingResponseAsync( timestamp );
			break;
		}
	}
}

void NetConnection::OnSetPeerBandwidthMessage( const mntone::rtmp::rtmp_packet /*packet*/, std::vector<uint8> data )
{
	uint32 buf;
	utility::convert_big_endian( &data[0], 4, &buf );

	const auto& limit = static_cast<limit_type>( data[4] );
	switch( limit )
	{
	case limit_type::hard:
		txWindowSize_ = buf;
		txLimitType_ = limit;
		break;

	case limit_type::soft:
		txWindowSize_ = std::min( txWindowSize_, buf );
		txLimitType_ = limit;
		break;

	case limit_type::dynamic:
		if( txLimitType_ == limit_type::hard )
		{
			txWindowSize_ = buf;
			txLimitType_ = limit;
		}
		break;

	default:
		return;
	}
	WindowAcknowledgementSizeAsync( buf );
}

void NetConnection::OnCommandMessage( const rtmp_packet /*packet*/, std::vector<uint8> data )
{
	const auto& amf = RtmpHelper::ParseAmf( std::move( data ) );
	if( amf == nullptr )
		return;

	const auto& name = amf->GetStringAt( 0 );
	const auto& tid = static_cast<uint32>( amf->GetNumberAt( 1 ) );
	
	// for connect result (tid = 1)
	if( tid == 1 )
	{
		//const auto& properties = amf->GetObjectAt( 2 );
		const auto& information = amf->GetObjectAt( 3 );

		//const auto fmsVer = properties->GetNamedString( "fmsVer" );
		//const auto capabilities = properties->GetNamedDouble( "capabilities" );
		//const auto level = information->GetNamedString( "level" );
		//const auto description = information->GetNamedString( "description" );
		//const auto objectEncoding = information->GetNamedDouble( "objectEncoding" );

		const auto& code = information->GetNamedString( "code" );
		const auto& nsc = RtmpHelper::ParseNetConnectionConnectCode( code->Data() );
		StatusUpdated( this, ref new NetStatusUpdatedEventArgs( nsc ) );
		return;
	}

	// for createStream result
	{
		const auto& itr = netStreamTemporary_.lower_bound( tid );
		if( itr != netStreamTemporary_.cend() && itr->first == tid )
		{
			if( name == "_result" )
			{
				//const auto commandBuf = amf->GetAt( 2 );
				//if( commandBuf->ValueType != Mntone::Data::Amf::AmfValueType::Object )
				//	const auto command = commandBuf->GetObject();
				const auto& sid = static_cast<uint32>( amf->GetNumberAt( 3 ) );

				auto stream = itr->second;
				stream->streamId_ = sid;
				bindingNetStream_.emplace( sid, stream );
				stream->AttachedImpl();
				SetBufferLengthAsync( sid, DEFAULT_BUFFER_MILLSECONDS );
			}
			netStreamTemporary_.erase( itr );
			return;
		}
	}

	// for call result (tid = 0 or choice)
	{
		//const auto commandBuf = amf->GetAt( 2 );
		//if( commandBuf->ValueType != Mntone::Data::Amf::AmfValueType::Object )
		//	const auto command = commandBuf->GetObject();
		if( amf->Size >= 4 )
		{
			const auto response = amf->GetAt( 3 );
			Callback( this, ref new NetConnectionCallbackEventArgs( name, response ) );
		}
		else
		{
			Callback( this, ref new NetConnectionCallbackEventArgs( name, nullptr ) );
		}
	}
}

#pragma endregion

#pragma region Network operation (Client to Server)

task<void> NetConnection::SetChunkSizeAsync( const int32 chunkSize )
{
	if( chunkSize < 0 )
	{
		throw ref new Platform::InvalidArgumentException( "Invalid chunkSize. Valid chunkSize is 1 to 2147483647." );
	}

	std::vector<uint8> buf( 4 );
	utility::convert_big_endian( &chunkSize, 4, &buf[0] );
	return SendNetworkAsync( type_id_type::set_chunk_size, std::move( buf ) ).then( [=]
	{
		txChunkSize_ = chunkSize;
	} );
}

task<void> NetConnection::AbortMessageAsync( const uint32 chunkStreamId )
{
	std::vector<uint8> buf( 4 );
	utility::convert_big_endian( &chunkStreamId, 4, &buf[0] );
	return SendNetworkAsync( type_id_type::abort_message, std::move( buf ) );
}

task<void> NetConnection::AcknowledgementAsync( const uint32 sequenceNumber )
{
	std::vector<uint8> buf( 4 );
	utility::convert_big_endian( &sequenceNumber, 4, &buf[0] );
	return SendNetworkAsync( type_id_type::acknowledgement, std::move( buf ) );
}

task<void> NetConnection::WindowAcknowledgementSizeAsync( const uint32 acknowledgementWindowSize )
{
	std::vector<uint8> buf( 4 );
	utility::convert_big_endian( &acknowledgementWindowSize, 4, &buf[0] );
	return SendNetworkAsync( type_id_type::window_acknowledgement_size, std::move( buf ) );
}

task<void> NetConnection::SetPeerBandWidthAsync( const uint32 windowSize, const limit_type type )
{
	std::vector<uint8> buf( 5 );
	utility::convert_big_endian( &windowSize, 4, &buf[0] );
	buf[4] = static_cast<uint8>( type );
	return SendNetworkAsync( type_id_type::set_peer_bandwidth, std::move( buf ) );
}

task<void> NetConnection::SetBufferLengthAsync( const uint32 streamId, const uint32 bufferLength )
{
	std::vector<uint8> buf( 10 );
	utility::convert_big_endian( &streamId, 4, &buf[2] );
	utility::convert_big_endian( &bufferLength, 4, &buf[6] );
	return UserControlMessageEventAsync( UserControlMessageEventType::SetBufferLength, std::move( buf ) );
}

task<void> NetConnection::PingResponseAsync( const uint32 timestamp )
{
	std::vector<uint8> buf( 6 );
	utility::convert_big_endian( &timestamp, 4, &buf[2] );
	return UserControlMessageEventAsync( UserControlMessageEventType::PingResponse, std::move( buf ) );
}

task<void> NetConnection::UserControlMessageEventAsync( UserControlMessageEventType type, std::vector<uint8> data )
{
	const auto& c_type = static_cast<uint16>( type );
	utility::convert_big_endian( &c_type, 2, &data[0] );
	return SendNetworkAsync( type_id_type::user_control_message, std::move( data ) );
}

task<void> NetConnection::SendNetworkAsync( const type_id_type type, const std::vector<uint8> data )
{
	rtmp_packet packet( 2 ); // for Network (2)
	packet.timestamp_ = utility::hundred_nano_to_milli( utility::get_windows_time() - startTime_ );
	packet.length_ = static_cast<uint32>( data.size() );
	packet.type_id_ = type;
	packet.stream_id_ = 0;

	return SendAsync( std::move( packet ), std::move( data ), 0 );
}

task<void> NetConnection::SendActionAsync( Mntone::Data::Amf::AmfArray^ amf )
{
	return SendActionAsync( 0, amf );
}

task<void> NetConnection::SendActionAsync( const uint32 streamId, Mntone::Data::Amf::AmfArray^ amf )
{
	auto amf_data = amf->Sequencify( Mntone::Data::Amf::AmfEncodingType::Amf0 );
	const auto& length = amf_data->Length - 5;

	rtmp_packet packet( 3 ); // for Action (3)
	packet.timestamp_ = utility::hundred_nano_to_milli( utility::get_windows_time() - startTime_ );
	packet.length_ = length;
	packet.type_id_ = type_id_type::command_message_amf0;
	packet.stream_id_ = streamId;

	std::vector<uint8> buf( length );
	copy_n( amf_data->begin() + 5, length, buf.begin() );
	return SendAsync( std::move( packet ), std::move( buf ) );
}

task<void> NetConnection::SendAsync( rtmp_packet packet, const std::vector<uint8> data, const uint8_t forceFormatType )
{
	auto send_data = CreateHeader( std::move( packet ), forceFormatType );

	const auto& header_length = send_data.size();
	const auto& body_length = std::min<size_t>( txChunkSize_, packet.length_ - packet.temporary_length_ );
	send_data.resize( header_length + body_length );
	copy_n( data.cbegin() + packet.temporary_length_, body_length, send_data.begin() + header_length );
	packet.temporary_length_ += body_length;

	auto task = connection_->Write( send_data.data(), send_data.size() );
	if( packet.length_ != packet.temporary_length_ )
	{
		task = task.then( [this, packet, data]
		{
			return SendAsync( std::move( packet ), std::move( data ), 3 );
		} );
	}
	return task;
}

std::vector<uint8> NetConnection::CreateHeader( rtmp_packet packet, uint8_t forceFormatType )
{
	std::vector<uint8> data( 18 );
	auto itr = data.begin();

	// ---[ Get object ]----------
	std::shared_ptr<rtmp_packet> bak_packet;
	const auto& pk_itr = txBakPackets_.lower_bound( packet.chunk_stream_id_ );
	if( pk_itr != txBakPackets_.end() && pk_itr->first == packet.chunk_stream_id_ )
	{
		bak_packet = pk_itr->second;
	}
	else
	{
		forceFormatType = 0;
	}

	// ---[ Decide formatType ]----------
	uint8 formatType;
	if( forceFormatType <= 3 )
	{
		formatType = forceFormatType;
	}
	else if( packet.stream_id_ == bak_packet->stream_id_ )
	{
		if( packet.type_id_ == bak_packet->type_id_ && packet.length_ == bak_packet->length_ )
		{
			if( packet.timestamp_ == bak_packet->timestamp_ + 2 * bak_packet->timestamp_delta_ )
			{
				formatType = 3;
				packet.timestamp_delta_ = bak_packet->timestamp_delta_;
			}
			else if( packet.timestamp_ < bak_packet->timestamp_ )
			{
				formatType = 0;
			}
			else
			{
				formatType = 2;
			}
		}
		else
		{
			formatType = 1;
		}
	}
	else
	{
		formatType = 0;
	}

	// ---[ Chunk basic header ]----------
	*itr = formatType << 6;
	if( packet.chunk_stream_id_ < 64 )
	{
		*itr++ |= packet.chunk_stream_id_; //& 0x3f;
	}
	else if( packet.chunk_stream_id_ < 320 )
	{
		itr[1] = static_cast<uint8>( packet.chunk_stream_id_ - 64 );
		itr += 2;
	}
	else if( packet.chunk_stream_id_ < 65600 )
	{
		*itr++ |= 1;

		uint16 buf = packet.chunk_stream_id_ - 64;
		utility::convert_big_endian( &buf, 2, &itr[0] );
		itr += 2;
	}
	else
	{
		throw ref new Platform::InvalidArgumentException();
	}

	// ---[ Chunk message header ]----------
	switch( formatType )
	{
	case 0:
		utility::convert_big_endian( &packet.length_, 3, &itr[3] );
		itr[6] = static_cast<uint8>( packet.type_id_ );
		utility::convert_little_endian( &packet.stream_id_, 4, &itr[7] ); // LE
		if( packet.timestamp_ >= 0xffffff )
		{
			itr[0] = itr[1] = itr[2] = 0xff;
			itr += 11;

			utility::convert_big_endian( &packet.timestamp_, 4, &itr[0] );
			itr += 4;
		}
		else
		{
			utility::convert_big_endian( &packet.timestamp_, 3, &itr[0] );
			itr += 11;
		}
		break;
	case 1:
		{
			packet.timestamp_delta_ = packet.timestamp_ - bak_packet->timestamp_;
			utility::convert_big_endian( &packet.length_, 3, &itr[3] );
			itr[6] = static_cast<uint8>( packet.type_id_ );
			if( packet.timestamp_delta_ >= 0xffffff )
			{
				itr[0] = itr[1] = itr[2] = 0xff;
				itr += 7;

				utility::convert_big_endian( &packet.timestamp_delta_, 4, &itr[0] );
				itr += 4;
			}
			else
			{
				utility::convert_big_endian( &packet.timestamp_delta_, 3, &itr[0] );
				itr += 7;
			}
			break;
		}
	case 2:
		{
			packet.timestamp_delta_ = packet.timestamp_ - bak_packet->timestamp_;
			if( packet.timestamp_delta_ >= 0xffffff )
			{
				itr[0] = itr[1] = itr[2] = 0xff;
				itr += 3;

				utility::convert_big_endian( &packet.timestamp_delta_, 4, &itr[0] );
				itr += 4;
			}
			else
			{
				utility::convert_big_endian( &packet.timestamp_delta_, 3, &itr[0] );
				itr += 3;
			}
			break;
		}
	case 3:		
		if( packet.timestamp_delta_ >= 0xffffff )
		{
			utility::convert_big_endian( &packet.timestamp_delta_, 4, &itr[0] );
			itr += 4;
		}
		break;
	}

	data.resize( itr - data.cbegin() );
	txBakPackets_.emplace( packet.chunk_stream_id_, std::make_shared<rtmp_packet>( std::move( packet ) ) );
	return std::move( data );
}

#pragma endregion