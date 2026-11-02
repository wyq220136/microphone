#ifndef SINGLEMIC_ACTIVE_TRANSPORT_H_
#define SINGLEMIC_ACTIVE_TRANSPORT_H_

#include "cfg.h"

#if SINGLEMIC_TRANSPORT_MODE == SINGLEMIC_TRANSPORT_CLASSIC_SPP
#include "spp_transport.h"
using ActiveTransport = SppTransport;
#else
#include "ble_transport.h"
using ActiveTransport = BleTransport;
#endif

#endif
