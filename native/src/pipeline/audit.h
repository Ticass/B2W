#pragma once
// The build's zone audit (builddriver._audit_zone): unlink a built zone, check
// every material argument against T6's unbounded table scans
// and, for the map zone, the authored bone
// hitboxes.

#include "core/text.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    namespace fs = std::filesystem;

    struct MaterialAudit
    {
        std::vector<std::string> problems;
        long long checked = 0;
        std::set<std::string> missingTechsets;
    };

    uint32_t NameHash(const std::string& name);
    MaterialAudit AuditMaterials(const fs::path& zone, const std::vector<fs::path>& extra);
    // audit-material-args: prints its report, returns its exit code
    int PrintMaterialAudit(const fs::path& zone, const std::vector<fs::path>& extra);

    bool HasAuthoredHitboxes(const fs::path& source);
    util::Json AuditHitboxes(const fs::path& source, const fs::path& built);

    // throws StepError on a failed step
    void AuditZone(const fs::path& ff, const fs::path& stage, const std::string& project, const fs::path& bo2, const fs::path& stock, const fs::path& unlinker);
} // namespace w2b::pipeline
