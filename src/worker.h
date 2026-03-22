#pragma once

#include "transport_policy.h"
#include "worker_base.h"
#include "h2c_io_uring_worker.h" // alias 없이 사용

// alias
using H2cWorker = WorkerBase<H2cTransportPolicy>;
using TlsWorker = WorkerBase<TlsTransportPolicy>;
