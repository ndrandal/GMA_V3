#pragma once
namespace gma::ws {
static constexpr const char* kSubscribeSchema = R"JSON(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "type":"object",
  "properties":{
    "type":{"const":"subscribe"},
    "clientId":{"type":"string"},
    "requests":{
      "type":"array",
      "items":{
        "type":"object",
        "properties":{
          "id":{"type":"string"},
          "symbol":{"type":"string"},
          "field":{"type":"string"},
          "pollMs":{"type":"integer","minimum":10},
          "pipeline":{"type":"object"},
          "operations":{"type":"array"}
        },
        "required":["id","symbol"],
        "additionalProperties": true
      }
    }
  },
  "required":["type","clientId","requests"],
  "additionalProperties": false
}
)JSON";

static constexpr const char* kCancelSchema = R"JSON(
{
  "type":"object",
  "properties":{
    "type":{"const":"cancel"},
    "clientId":{"type":"string"},
    "ids":{"type":"array","items":{"type":"string"}}
  },
  "required":["type","clientId","ids"],
  "additionalProperties": false
}
)JSON";
} // namespace gma::ws
