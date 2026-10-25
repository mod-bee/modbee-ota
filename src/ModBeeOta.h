#pragma once

// ModBee OTA - firmware updates for ModBee nodes, over whatever link reaches
// them. docs/ARCHITECTURE.md explains the pieces; examples/ shows them used.
//
// This header brings in the core and every target driver that compiles on the
// current platform. Links that depend on another library are included
// separately, after that library:
//
//   #include <ModBeeMqtt.h>
//   #include <ModBeeOta.h>
//   #include <links/OtaMqttLink.h>       // or links/OtaSparkplugLink.h

#include "OtaConfig.h"
#include "OtaTypes.h"
#include "OtaPlatform.h"
#include "OtaHash.h"
#include "OtaProtocol.h"
#include "OtaManifest.h"
#include "OtaTarget.h"
#include "OtaLink.h"
#include "OtaVerifier.h"
#include "OtaStore.h"
#include "OtaUpdater.h"
#include "OtaServer.h"
#include "OtaBackground.h"
#include "OtaAsyncTarget.h"
#include "OtaAsyncVerifier.h"

#include "staged/OtaStagedTarget.h"

// Each compiles to nothing off its platform.
#include "esp32/OtaEsp32Target.h"
#include "esp32/OtaEsp32Store.h"
#include "esp32/OtaEd25519Verifier.h"
#include "esp32/OtaEsp32Worker.h"

#define MODBEE_OTA_LIBRARY_VERSION "0.1.0"
