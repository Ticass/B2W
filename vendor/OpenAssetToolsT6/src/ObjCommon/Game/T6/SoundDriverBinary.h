#pragma once

// Lossless T6 driver sidecar. All eight tables are pointer-free native records;
// no process pointers or game DLL data are embedded. Version/strides/counts are
// checked on import. Keeping original T6 tables is required before appending
// converted WaW curves: replacing the driver's stock indices breaks BO2 audio.
#include "Game/T6/T6.h"

#include <bit>
#include <cstring>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <type_traits>

namespace T6::sound_driver_binary
{
    inline constexpr char MAGIC[8] = {'W', '2', 'B', 'S', 'D', 'G', '1', '\0'};
    static_assert(std::endian::native == std::endian::little);
    static_assert(sizeof(SndVolumeGroup) == 80 && sizeof(SndCurve) == 100 && sizeof(SndPan) == 60);
    static_assert(sizeof(SndDuckGroup) == 36 && sizeof(SndContext) == 36 && sizeof(SndMaster) == 240);
    static_assert(sizeof(SndSidechainDuck) == 60 && sizeof(SndFutz) == 100);

    template<typename T> void Write(std::ostream& stream, unsigned int count, const T* values)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        const unsigned int stride = sizeof(T);
        if (count && !values)
            throw std::runtime_error("null sound driver table");
        stream.write(reinterpret_cast<const char*>(&count), sizeof(count));
        stream.write(reinterpret_cast<const char*>(&stride), sizeof(stride));
        if (count)
            stream.write(reinterpret_cast<const char*>(values), sizeof(T) * count);
        if (!stream)
            throw std::runtime_error("failed to write sound driver table");
    }

    inline void WriteAll(std::ostream& stream, const SndDriverGlobals& driver)
    {
        stream.write(MAGIC, sizeof(MAGIC));
        Write(stream, driver.groupCount, driver.groups);
        Write(stream, driver.curveCount, driver.curves);
        Write(stream, driver.panCount, driver.pans);
        Write(stream, driver.duckGroupCount, driver.duckGroups);
        Write(stream, driver.contextCount, driver.contexts);
        Write(stream, driver.masterCount, driver.masters);
        Write(stream, driver.voiceDuckCount, driver.voiceDucks);
        Write(stream, driver.futzCount, driver.futzes);
    }
}
