// Named assets from loaded WaW zones (see zone_dumper.h).
#include "oat/zone_dumper.h"

#include "oat/loaded_zones.h"
#include "oat/oat_state.h"

#include "Utils/Logging/Log.h"

namespace w2b::oat
{
    AssetDumpRun DumpAssets(const std::filesystem::path& fastfile, const std::vector<std::filesystem::path>& searchPaths,
                            const std::vector<std::pair<std::string, std::string>>& assets, const std::filesystem::path& output)
    {
        const std::lock_guard guard(StateLock());
        const CapturedOutput log;
        con::reset_counts();
        con::set_use_color(false);
        AssetDumpRun run;
        run.ok = Loaded(fastfile, searchPaths).Dump(assets, output);
        run.log = log.Text();
        return run;
    }
} // namespace w2b::oat
