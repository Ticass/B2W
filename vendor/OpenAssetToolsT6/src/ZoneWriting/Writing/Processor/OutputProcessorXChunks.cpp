#include "OutputProcessorXChunks.h"

#include "Writing/WritingException.h"
#include "Zone/XChunk/XChunkException.h"
#include "Zone/ZoneTypes.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace
{
    // Chunks compressed per parallel batch: 2 x 0x8000 bytes of buffers each.
    constexpr size_t QUEUE_CHUNKS = 256;
    // The linker is a 32-bit process; more workers than this buys nothing.
    constexpr unsigned MAX_WORKERS = 16;
} // namespace

void OutputProcessorXChunks::Init()
{
    if (m_vanilla_buffer_size > 0)
        m_vanilla_buffer_offset = static_cast<size_t>(m_base_stream->Pos()) % m_vanilla_buffer_size;

    m_initialized = true;
}

OutputProcessorXChunks::Chunk OutputProcessorXChunks::NewChunk()
{
    if (!m_spare.empty())
    {
        auto chunk = std::move(m_spare.back());
        m_spare.pop_back();
        chunk.m_size = 0;
        return chunk;
    }

    Chunk chunk;
    chunk.m_input = std::make_unique<uint8_t[]>(m_chunk_size);
    chunk.m_output = std::make_unique<uint8_t[]>(m_chunk_size);
    return chunk;
}

void OutputProcessorXChunks::QueueChunk()
{
    m_filling.m_stream = static_cast<unsigned>(m_current_stream);
    m_current_stream = (m_current_stream + 1) % m_stream_count;

    if (m_queued == m_queue.size())
        m_queue.emplace_back();
    m_queue[m_queued++] = std::move(m_filling);
    m_filling = NewChunk();

    if (m_queued >= QUEUE_CHUNKS)
        ProcessQueue();
}

void OutputProcessorXChunks::ProcessQueue()
{
    if (m_queued == 0)
        return;

    size_t parallelProcessors = 0;
    while (parallelProcessors < m_chunk_processors.size() && m_chunk_processors[parallelProcessors]->IsStateless())
        parallelProcessors++;

    // Each chunk runs the stateless processors on its own buffers. The result
    // ends in m_input with its size in m_size, as the serial loop leaves it.
    const auto runStateless = [this, parallelProcessors](Chunk& chunk)
    {
        for (size_t i = 0; i < parallelProcessors; i++)
        {
            chunk.m_size = m_chunk_processors[i]->Process(chunk.m_stream, chunk.m_input.get(), chunk.m_size, chunk.m_output.get(), m_chunk_size);
            std::swap(chunk.m_input, chunk.m_output);
        }
    };

    try
    {
        if (parallelProcessors > 0)
        {
            const auto workers = static_cast<unsigned>(std::min<size_t>(m_worker_count, m_queued));
            if (workers <= 1)
            {
                for (size_t i = 0; i < m_queued; i++)
                    runStateless(m_queue[i]);
            }
            else
            {
                std::atomic<size_t> next{0};
                std::exception_ptr failure;
                std::mutex failureLock;
                const auto work = [&]
                {
                    try
                    {
                        for (auto i = next++; i < m_queued; i = next++)
                            runStateless(m_queue[i]);
                    }
                    catch (...)
                    {
                        const std::lock_guard lock(failureLock);
                        if (!failure)
                            failure = std::current_exception();
                        next = m_queued;
                    }
                };

                std::vector<std::thread> threads;
                threads.reserve(workers - 1);
                for (auto i = 1u; i < workers; i++)
                    threads.emplace_back(work);
                work();
                for (auto& thread : threads)
                    thread.join();

                if (failure)
                    std::rethrow_exception(failure);
            }
        }

        for (size_t i = 0; i < m_queued; i++)
        {
            auto& chunk = m_queue[i];
            for (auto p = parallelProcessors; p < m_chunk_processors.size(); p++)
            {
                chunk.m_size = m_chunk_processors[p]->Process(chunk.m_stream, chunk.m_input.get(), chunk.m_size, chunk.m_output.get(), m_chunk_size);
                std::swap(chunk.m_input, chunk.m_output);
            }
            WriteProcessed(chunk);
        }
    }
    catch (XChunkException& e)
    {
        throw WritingException(e.Message());
    }

    for (size_t i = 0; i < m_queued; i++)
        m_spare.emplace_back(std::move(m_queue[i]));
    m_queued = 0;
}

void OutputProcessorXChunks::WriteProcessed(Chunk& chunk)
{
    if (m_vanilla_buffer_size > 0)
    {
        if (m_vanilla_buffer_offset + sizeof(xchunk_size_t) > m_vanilla_buffer_size)
        {
            xchunk_size_t zeroMem = 0;
            m_base_stream->Write(&zeroMem, m_vanilla_buffer_size - m_vanilla_buffer_offset);
            m_vanilla_buffer_offset = 0;
        }
    }

    auto chunkSize = static_cast<xchunk_size_t>(chunk.m_size);
    m_base_stream->Write(&chunkSize, sizeof(chunkSize));
    m_base_stream->Write(chunk.m_input.get(), chunk.m_size);

    if (m_vanilla_buffer_size > 0)
    {
        m_vanilla_buffer_offset += sizeof(chunkSize) + chunk.m_size;
        m_vanilla_buffer_offset %= m_vanilla_buffer_size;
    }
}

OutputProcessorXChunks::OutputProcessorXChunks(const int numStreams, const size_t xChunkSize, const size_t xChunkWriteSize)
    : m_stream_count(numStreams),
      m_chunk_size(xChunkSize),
      m_chunk_write_size(xChunkWriteSize),
      m_vanilla_buffer_size(0),
      m_initialized(false),
      m_current_stream(0),
      m_vanilla_buffer_offset(0),
      m_queued(0),
      m_worker_count(std::clamp(std::thread::hardware_concurrency(), 1u, MAX_WORKERS))
{
    assert(numStreams > 0);
    assert(xChunkSize > 0);
    assert(m_chunk_size >= m_chunk_write_size);

    m_filling = NewChunk();
}

OutputProcessorXChunks::OutputProcessorXChunks(const int numStreams, const size_t xChunkSize, const size_t xChunkWriteSize, const size_t vanillaBufferSize)
    : OutputProcessorXChunks(numStreams, xChunkSize, xChunkWriteSize)
{
    m_vanilla_buffer_size = vanillaBufferSize;
}

void OutputProcessorXChunks::AddChunkProcessor(std::unique_ptr<IXChunkProcessor> chunkProcessor)
{
    assert(chunkProcessor != nullptr);

    m_chunk_processors.emplace_back(std::move(chunkProcessor));
}

void OutputProcessorXChunks::Write(const void* buffer, const size_t length)
{
    assert(buffer != nullptr);

    if (!m_initialized)
        Init();

    auto sizeRemaining = length;
    while (sizeRemaining > 0)
    {
        const auto toWrite = std::min(m_chunk_write_size - m_filling.m_size, sizeRemaining);

        memcpy(&m_filling.m_input[m_filling.m_size], &static_cast<const char*>(buffer)[length - sizeRemaining], toWrite);
        m_filling.m_size += toWrite;
        if (m_filling.m_size >= m_chunk_write_size)
            QueueChunk();

        sizeRemaining -= toWrite;
    }
}

void OutputProcessorXChunks::Flush()
{
    if (m_filling.m_size)
        QueueChunk();
    ProcessQueue();

    m_base_stream->Flush();
}

int64_t OutputProcessorXChunks::Pos()
{
    // The serial writer had written every full chunk by now; so must this one.
    ProcessQueue();
    return m_base_stream->Pos();
}
