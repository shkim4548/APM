#include "pch.h"
#include "SendBuffer.h"

/*----------------
    SendBuffer
-----------------*/

SendBuffer::SendBuffer(SendBufferChunkRef owner, BYTE* buffer, uint32 allocSize)
    : _owner(owner), _buffer(buffer), _allocSize(allocSize)
{
}

SendBuffer::~SendBuffer()
{
}

void SendBuffer::Close(uint32 writeSize)
{
    ASSERT_CRASH(_allocSize >= writeSize);
    _writeSize = writeSize;
    _owner->Close(writeSize);
}

/*--------------------
    SendBufferChunk
--------------------*/

SendBufferChunk::SendBufferChunk()
{
}

SendBufferChunk::~SendBufferChunk()
{
}

void SendBufferChunk::Reset()
{
    _open = false;
    _usedSize = 0;
}

SendBufferRef SendBufferChunk::Open(uint32 allocSize)
{
    ASSERT_CRASH(allocSize <= SEND_BUFFER_CHUNK_SIZE);
    ASSERT_CRASH(_open == false);

    if (allocSize > FreeSize())
        return nullptr;

    _open = true;
    return make_shared<SendBuffer>(shared_from_this(), Buffer(), allocSize);
}

void SendBufferChunk::Close(uint32 writeSize)
{
    ASSERT_CRASH(_open == true);
    _open = false;
    _usedSize += writeSize;
}

/*---------------------
    SendBufferManager
----------------------*/

SendBufferRef SendBufferManager::Open(uint32 size)
{
    {
        WRITE_LOCK;
        if (_currentChunk == nullptr || _currentChunk->FreeSize() < size)
        {
            _currentChunk = make_shared<SendBufferChunk>();
            _currentChunk->Reset();
        }
    }

    ASSERT_CRASH(_currentChunk->IsOpen() == false);
    return _currentChunk->Open(size);
}