#pragma once

#include "core/text.h"

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace w2b::extract
{
    class StockWaw;
}

namespace w2b::pipeline
{
    std::vector<std::string> NativeTreeAnimations(std::string_view tree);

    // A native T6 actor tree copied under a name the map owns, with the
    // donor states and the source traversal animations it needs.
    struct OwnedActorTreeRequest
    {
        std::filesystem::path project, bo2Root;
        std::string base, ownedTree;
        std::vector<std::pair<std::string, std::vector<std::string>>> donorStates; // donor .asd -> states
        std::map<std::string, std::string> sourceStates;                          // traversal state -> WaW xanim
        std::vector<std::filesystem::path> roots;
        extract::StockWaw* stock = nullptr;
    };

    struct OwnedActorTree
    {
        std::string name, tree;
        std::vector<std::string> errors;
    };

    // Throws StepError (StageError, TraversalError) when the BO2 tree lacks what it needs.
    OwnedActorTree StageOwnedActorTree(const OwnedActorTreeRequest& request);
}
