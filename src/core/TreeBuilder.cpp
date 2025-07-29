#include "gma/TreeBuilder.hpp"
#include "gma/Logger.hpp"
#include "gma/JsonValidator.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/MarketDispatcher.hpp"

// pull in all the node types
#include "gma/nodes/Listener.hpp"
#include "gma/nodes/AtomicAccessor.hpp"
#include "gma/nodes/Worker.hpp"
#include "gma/nodes/Aggregate.hpp"
#include "gma/nodes/SymbolSplit.hpp"
#include "gma/nodes/Interval.hpp"
#include "gma/FunctionMap.hpp"

using namespace gma;
using namespace gma::nodes;

std::shared_ptr<INode>
TreeBuilder::build(const rapidjson::Document& json,
                   ExecutionContext* ctx,
                   MarketDispatcher* dispatcher)
{
    // validate top‑level request has "id" and "tree"
    JsonValidator::validateRequest(json);

    // recurse into the "tree" object, with no downstream yet
    return buildInternal(json["tree"], ctx, dispatcher, nullptr);
}

std::shared_ptr<INode>
TreeBuilder::buildInternal(const rapidjson::Value& nodeJson,
                           ExecutionContext* ctx,
                           MarketDispatcher* dispatcher,
                           std::shared_ptr<INode> downstream)
{
    // validate this node
    JsonValidator::validateNode(nodeJson);
    const std::string type = nodeJson["type"].GetString();

    //
    // 1) Listener
    //
    if (type == "Listener") {
        JsonValidator::requireMember(nodeJson, "symbol", rapidjson::kStringType);
        JsonValidator::requireMember(nodeJson, "field",  rapidjson::kStringType);

        // first build any nested child
        std::shared_ptr<INode> next = downstream;
        if (nodeJson.HasMember("downstream")) {
            JsonValidator::requireMember(nodeJson, "downstream", rapidjson::kObjectType);
            next = buildInternal(nodeJson["downstream"], ctx, dispatcher, downstream);
        }

        auto symbol = nodeJson["symbol"].GetString();
        auto field  = nodeJson["field"].GetString();
        return std::make_shared<Listener>(
            symbol,
            field,
            next,
            ctx->pool(),
            dispatcher
        );
    }

    //
    // 2) AtomicAccessor
    //
    if (type == "AtomicAccessor") {
        JsonValidator::requireMember(nodeJson, "symbol", rapidjson::kStringType);
        JsonValidator::requireMember(nodeJson, "field",  rapidjson::kStringType);

        // First build any downstream subtree
        std::shared_ptr<INode> next = downstream;
        if (nodeJson.HasMember("downstream")) {
            JsonValidator::requireMember(nodeJson, "downstream", rapidjson::kObjectType);
            next = buildInternal(nodeJson["downstream"], ctx, dispatcher, downstream);
        }

        // Pull out config
        auto symbol = nodeJson["symbol"].GetString();
        auto field  = nodeJson["field"].GetString();

        // Instantiate the accessor node
        auto accessor = std::make_shared<AtomicAccessor>(
            symbol,
            field,
            ctx->store(),
            next
        );

        // **NEW**: subscribe it so it gets fired whenever we compute that atomic
        dispatcher->registerListener(symbol, field, accessor);

        return accessor;
    }

    //
    // 3) Worker
    //
    if (type == "Worker") {
        JsonValidator::requireMember(nodeJson, "function", rapidjson::kStringType);
        // downstream(s) of the worker
        std::vector<std::shared_ptr<INode>> children;
        if (nodeJson.HasMember("downstream")) {
            JsonValidator::requireMember(nodeJson, "downstream", rapidjson::kArrayType);
            for (auto& childJs : nodeJson["downstream"].GetArray()) {
                children.push_back(buildInternal(childJs, ctx, dispatcher, downstream));
            }
        }
        // look up the function
        // look up the function by name (use the correct API)
        // look up the raw atomic function: vector<double> -> double
        auto fnName = nodeJson["function"].GetString();
        auto rawFn = FunctionMap::instance().getFunction(fnName);

        // wrap it in a Worker::Function (Span<const ArgType> -> ArgType)
        Worker::Function fn = [rawFn](Span<const ArgType> args) -> ArgType {
            std::vector<double> nums;
            nums.reserve(args.size());
            for (auto const& arg : args) {
                if (auto pd = std::get_if<double>(&arg)) {
                    nums.push_back(*pd);
                } else {
                    // fallback for non‐double ArgType
                    nums.push_back(0.0);
                }
            }
            double result = rawFn(nums);
            return ArgType{ result };
        };

        return std::make_shared<Worker>(fn, std::move(children));
    }

    //
    // 4) Aggregate
    //
    if (type == "Aggregate") {
        JsonValidator::requireMember(nodeJson, "children", rapidjson::kArrayType);
        std::vector<std::shared_ptr<INode>> kids;
        for (auto& c : nodeJson["children"].GetArray()) {
            kids.push_back(buildInternal(c, ctx, dispatcher, downstream));
        }
        // this node’s own downstream is the passed‑in downstream
        return std::make_shared<Aggregate>(std::move(kids), downstream);
    }

    //
    // 5) SymbolSplit
    //
    if (type == "SymbolSplit") {
        JsonValidator::requireMember(nodeJson, "template", rapidjson::kObjectType);
        const auto& tmpl = nodeJson["template"];
        // factory: clones the template subtree each time (capture tmpl by reference)
        auto factory = [=, &tmpl](const std::string& sym) {
            return buildInternal(tmpl, ctx, dispatcher, downstream);
        };
    }

    //
    // 6) Interval
    //
    if (type == "Interval") {
        JsonValidator::requireMember(nodeJson, "period_ms", rapidjson::kNumberType);
        int ms = nodeJson["period_ms"].GetInt();
        // its downstream is optional single object
        std::shared_ptr<INode> next = downstream;
        if (nodeJson.HasMember("downstream")) {
            JsonValidator::requireMember(nodeJson, "downstream", rapidjson::kObjectType);
            next = buildInternal(nodeJson["downstream"], ctx, dispatcher, downstream);
        }
        return std::make_shared<Interval>(
            std::chrono::milliseconds(ms),
            next,
            ctx->pool()
        );
    }

    //
    // Unknown type
    //
    throw std::runtime_error("Unknown node type: " + type);
}
