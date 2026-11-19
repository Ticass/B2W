#include "pipeline/bridge_report.h"

namespace w2b::pipeline
{
    py::Json BridgeReport::ToJson() const
    {
        auto fx = py::Json::object();
        for (const auto& [name, converted] : fxTable)
            fx[name] = converted;
        return {{"project", project},   {"materials", pipeline::ToJson(materials)},
                {"images", images},     {"techsets", techsets},
                {"collision", collision}, {"entities", entities},
                {"scripts", scripts},   {"content", content},
                {"fx_table", std::move(fx)}, {"errors", errors},
                {"warnings", warnings}};
    }
} // namespace w2b::pipeline
