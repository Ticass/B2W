#pragma once
// t6bridge.stage_bridge, the client scripts: the environment script (native
// flame pools for carried gas weapons, the tesla client), the BO2 client
// main's hooks and the map's own client main (clientport.finish_map_client).

#include "core/text.h"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    struct ClientFinishRequest
    {
        std::filesystem::path projectRoot;
        std::string project;
        std::vector<std::filesystem::path> roots;
        std::map<std::string, std::string> fxTable;
        std::set<std::string> soundNames;
        bool characters = false;
        util::Json mapClient; // the scripts report's client section, null without map client scripts
    };

    struct ClientFinish
    {
        std::vector<std::string> warnings;
        util::Json mapClient; // the client section as the map client hook left it
    };

    // The client mains: environment script, Tesla, characters and the map's own client main.
    // Throws StepError("ValueError") when the BO2 client main has no waitforclient( 0 ).
    ClientFinish FinishClientScripts(const ClientFinishRequest& request);
} // namespace w2b::pipeline
