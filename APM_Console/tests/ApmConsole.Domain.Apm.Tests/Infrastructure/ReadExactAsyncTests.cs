using ApmConsole.Domain.Apm.Infrastructure;

namespace ApmConsole.Domain.Apm.Tests.Infrastructure;

// 테스트 전용 - ReadAsync를 호출될 때마다 최대 maxBytesPerRead만큼만 채워서 반환해
// "한 번의 read가 요청한 만큼 다 안 왔을 때"(TCP에서 흔한 상황)를 재현하는 스트림.
file sealed class ChunkedStream : Stream
{
    private readonly byte[] _data;
    private readonly int _maxBytesPerRead;
    private int _position;

    public ChunkedStream(byte[] data, int maxBytesPerRead)
    {
        _data = data;
        _maxBytesPerRead = maxBytesPerRead;
    }

    public override async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
    {
        await Task.Yield(); // 실제 비동기 I/O처럼 동기 완료가 아님을 보장
        var remaining = _data.Length - _position;
        if (remaining <= 0)
            return 0;

        var toCopy = Math.Min(Math.Min(_maxBytesPerRead, buffer.Length), remaining);
        _data.AsSpan(_position, toCopy).CopyTo(buffer.Span);
        _position += toCopy;
        return toCopy;
    }

    public override bool CanRead => true;
    public override bool CanSeek => false;
    public override bool CanWrite => false;
    public override long Length => _data.Length;
    public override long Position { get => _position; set => throw new NotSupportedException(); }
    public override void Flush() { }
    public override int Read(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
}

// 연결이 중간에 끊긴 상황(요청한 길이만큼 못 채우고 스트림이 끝남)을 재현.
file sealed class TruncatedStream : Stream
{
    private readonly byte[] _data;
    private int _position;

    public TruncatedStream(byte[] data) => _data = data;

    public override async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
    {
        await Task.Yield();
        if (_position >= _data.Length)
            return 0; // 연결 종료(더 줄 데이터 없음)

        var toCopy = Math.Min(buffer.Length, _data.Length - _position);
        _data.AsSpan(_position, toCopy).CopyTo(buffer.Span);
        _position += toCopy;
        return toCopy;
    }

    public override bool CanRead => true;
    public override bool CanSeek => false;
    public override bool CanWrite => false;
    public override long Length => _data.Length;
    public override long Position { get => _position; set => throw new NotSupportedException(); }
    public override void Flush() { }
    public override int Read(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
}

public class ReadExactAsyncTests
{
    [Fact]
    public async Task 한번에_전부_오면_그대로_반환한다()
    {
        var data = new byte[] { 1, 2, 3, 4, 5 };
        var stream = new MemoryStream(data);

        var result = await MetricsReceiverService.ReadExactAsync(stream, 5, CancellationToken.None);

        Assert.Equal(data, result);
    }

    [Fact]
    public async Task 여러_조각으로_나뉘어_와도_전부_모아서_반환한다()
    {
        // TCP는 스트림 프로토콜이라 한 번의 read가 요청한 만큼 다 안 올 수 있음 -
        // ReadExactAsync가 이 경우를 정확히 처리하는지가 이 테스트의 핵심.
        var data = new byte[] { 10, 20, 30, 40, 50, 60, 70 };
        var stream = new ChunkedStream(data, maxBytesPerRead: 2); // 2바이트씩만 반환

        var result = await MetricsReceiverService.ReadExactAsync(stream, data.Length, CancellationToken.None);

        Assert.Equal(data, result);
    }

    [Fact]
    public async Task 요청한_길이보다_먼저_스트림이_끝나면_null을_반환한다()
    {
        var data = new byte[] { 1, 2, 3 }; // 3바이트뿐인데
        var stream = new TruncatedStream(data);

        var result = await MetricsReceiverService.ReadExactAsync(stream, 10, CancellationToken.None); // 10바이트 요청

        Assert.Null(result);
    }

    [Fact]
    public async Task 길이가_0이면_빈_배열을_반환한다()
    {
        var stream = new MemoryStream(Array.Empty<byte>());

        var result = await MetricsReceiverService.ReadExactAsync(stream, 0, CancellationToken.None);

        Assert.NotNull(result);
        Assert.Empty(result);
    }
}
