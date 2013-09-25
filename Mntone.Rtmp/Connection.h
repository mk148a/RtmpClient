#pragma once

namespace Mntone { namespace Rtmp {

	ref class Connection sealed
	{
	internal:
		Connection( void );

	public:
		virtual ~Connection( void );

	internal:
		void Connect( Platform::String^ host, Platform::String^ port );

		uint32 TryRead( uint8 *const data, const size_t length );

		void Read( uint8 *const data, const size_t length );
		void Read( std::vector<uint8>& data );
		void Read( std::vector<uint8>& data, const size_t length );

		void Write( uint8 *const data, const size_t length );
		void Write( std::vector<uint8>& data );
		void Write( std::vector<uint8>& data, const size_t length );

	internal:
		property bool IsInitialized
		{
			bool get() { return _IsInitialized; }
		}

	private:
		bool _IsInitialized;
		Windows::Networking::Sockets::StreamSocket^ _streamSocket;
		Windows::Storage::Streams::DataReader^ _dataReader;
		Windows::Storage::Streams::DataWriter^ _dataWriter;
	};

} }