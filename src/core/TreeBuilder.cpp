#include "gma/TreeBuilder.hpp"
#include "gma/Logger.hpp"
#include "gma/JsonValidator.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/nodes/Listener.hpp"    // for gma::nodes::Listener
#include <exception>
#include <stdexcept>

using namespace gma;
using gma::nodes::Listener;

std::shared_ptr<INode>
TreeBuilder::build(const rapidjson::Document& json,
                   ExecutionContext* ctx,
                   MarketDispatcher* dispatcher)
{
    try {
        // Validate the overall request
        JsonValidator::validateRequest(json);

        // Expect the subtree under "tree"
        if (!json.HasMember("tree") || !json["tree"].IsObject()) {
            throw std::runtime_error("Missing or invalid 'tree' object");
        }

        return buildInternal(json["tree"], ctx, dispatcher, nullptr);
    }
    catch (const std::exception& ex) {
        Logger::error("TreeBuilder::build failed: " + std::string(ex.what()));
        throw;
    }
}

std::shared_ptr<INode>
TreeBuilder::buildInternal(const rapidjson::Value&   nodeJson,
                           ExecutionContext*          ctx,
                           MarketDispatcher*         dispatcher,
                           std::shared_ptr<INode>    downstream)
{
    // Validate each node JSON
    JsonValidator::validateNode(nodeJson);

    auto typeIt = nodeJson.FindMember("type");
    if (typeIt == nodeJson.MemberEnd() || !typeIt->value.IsString()) {
        throw std::runtime_error("Node missing 'type' field");
    }
    std::string type = typeIt->value.GetString();

    // Dispatch on type
    if (type == "Listener") {
        // Required fields
        if (!nodeJson.HasMember("symbol") || !nodeJson["symbol"].IsString() ||
            !nodeJson.HasMember("field")  || !nodeJson["field"].IsString()) {
            throw std::runtime_error("Listener node missing 'symbol' or 'field'");
        }
        std::string symbol = nodeJson["symbol"].GetString();
        std::string field  = nodeJson["field"].GetString();

        // Create the listener, chaining downstream
        auto listener = std::make_shared<Listener>(
            symbol,
            field,
            downstream,
            ctx->pool(),        // ThreadPool*
            dispatcher          // MarketDispatcher*
        );
        return listener;
    }

    // TODO: handle other node types here...
    throw std::runtime_error("Unknown node type: " + type);
}
