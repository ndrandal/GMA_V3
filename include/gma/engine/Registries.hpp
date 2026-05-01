#pragma once

// Umbrella header for connector authors. Including this brings in IConnector,
// EngineRegistries, and every engine extension-point registry so a connector
// implementation can write a single include instead of seven.
//
// Engine code should still include the specific headers it needs.

#include "gma/engine/IConnector.hpp"
#include "gma/engine/EngineRegistries.hpp"
#include "gma/engine/EventTypeRegistry.hpp"
#include "gma/engine/EventComputerRegistry.hpp"
#include "gma/engine/NodeTypeRegistry.hpp"
#include "gma/engine/IngressRegistry.hpp"
#include "gma/engine/ConfigNamespaceRegistry.hpp"
