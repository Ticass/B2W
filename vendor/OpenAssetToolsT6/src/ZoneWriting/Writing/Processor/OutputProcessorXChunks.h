#pragma once
#include "Writing/OutputStreamProcessor.h"
#include "Zone/XChunk/IXChunkProcessor.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Splits the zone stream into XChunks and runs the chunk processors on them.
// Full chunks are queued; the leading stateless processors (deflate) run on a
// batch of queued chunks in parallel, then the remaining processors (Salsa20,
// whose state depends on earlier chunks of the same stream) and the write run
// in the original chunk order. Chunk boundaries depend only on the input
// sizes, so the written bytes are the same as processing one chunk at a time.
class OutputProcessorXChunks final : public OutputStreamProcessor
{
    struct Chunk
    {
        std::unique_ptr<uint8_t[]> m_input;
        std::unique_ptr<uint8_t[]> m_output;
        size_t m_size = 0;
        unsigned m_stream = 0;
    };

    std::vector<std::unique_ptr<IXChunkProcessor>> m_chunk_processors;

    int m_stream_count;
    size_t m_chunk_size;
    size_t m_chunk_write_size;
    size_t m_vanilla_buffer_size;

    bool m_initialized;
    int m_current_stream;
    size_t m_vanilla_buffer_offset;

    std::vector<Chunk> m_queue;    // full chunks waiting for processing, in write order
    size_t m_queued;               // chunks of m_queue in use
    std::vector<Chunk> m_spare;    // recycled chunk buffers
    Chunk m_filling;               // chunk receiving input
    unsigned m_worker_count;

    void Init();
    Chunk NewChunk();
    void QueueChunk();
    void ProcessQueue();
    void WriteProcessed(Chunk& chunk);

public:
    OutputProcessorXChunks(int numStreams, size_t xChunkSize, size_t xChunkWriteSize);
    OutputProcessorXChunks(int numStreams, size_t xChunkSize, size_t xChunkWriteSize, size_t vanillaBufferSize);
    ~OutputProcessorXChunks() override = default;

    OutputProcessorXChunks(const OutputProcessorXChunks& other) = delete;
    OutputProcessorXChunks(OutputProcessorXChunks&& other) noexcept = default;
    OutputProcessorXChunks& operator=(const OutputProcessorXChunks& other) = delete;
    OutputProcessorXChunks& operator=(OutputProcessorXChunks&& other) noexcept = default;

    void AddChunkProcessor(std::unique_ptr<IXChunkProcessor> chunkProcessor);

    void Write(const void* buffer, size_t length) override;
    void Flush() override;
    int64_t Pos() override;
};
