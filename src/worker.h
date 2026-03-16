#pragma once

#include "transport_policy.h"
#include "worker_base.h"

using H2cWorker = WorkerBase<H2cTransportPolicy>;
using TlsWorker = WorkerBase<TlsTransportPolicy>;
