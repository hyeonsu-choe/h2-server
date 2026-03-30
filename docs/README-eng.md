# H2 Server

> 🇰🇷 [View in Korean](../README.md)

**h2server** is a high-performance HTTP/2 server implemented in **Modern C++**.   
It runs on Linux and uses `epoll`, [`nghttp2`](https://nghttp2.org), and [OpenSSL](https://www.openssl.org/).  
It supports optional TLS connections and provides low-latency file serving through an `mmap`-based cache.  
This project was originally inspired by my earlier Golang project, [webserver](https://github.com/hyeonsu-choe/websvr), and was developed as a personal project to study and benchmark system-level topics such as event-driven I/O, TLS handling, and the HTTP/2 protocol. :contentReference[oaicite:0]{index=0}

[[Detailed Documentation]](https://hyeonsu-choe.github.io/posts/h2_server/)

---

## 📌 Branch Overview

This repository has evolved beyond a single implementation and has been developed as a step-by-step exploration and validation of a high-performance HTTP/2 server architecture.

- **origin**
  - The initial high-performance design based on an `acceptor → load balancer → worker` architecture
  - A baseline branch used to validate the core structure and performance characteristics of HTTP/2, TLS, multithreaded processing, file serving, and `mmap`-based caching

- **feature/static-partitioning**
  - An experimental branch that applies **static partitioning** to more clearly separate worker responsibilities and processing paths
  - Introduces a **completion-based model on the H2C path** to address limitations of the readiness-oriented design
  - Contains the latest architectural improvements and performance experiments

> This document describes the architecture and benchmark results of the **origin branch**.  
> For the latest architecture and benchmark results, please refer to the `feature/static-partitioning` branch. :contentReference[oaicite:1]{index=1}

### Latest branch snapshot (`feature/static-partitioning`)

In the latest benchmark conducted on Ubuntu 24.04, the following results were observed:

- `h2server h2c` and `h2server h2c-io-uring` outperform `nghttpd` from the 4-thread range and above
- The best-performing configuration is `h2server_h2c_io_uring_n8`
- **Max QPS: 605,540**
- Static partitioning and the completion model showed meaningful throughput improvements under high-load conditions :contentReference[oaicite:2]{index=2}

> For the full benchmark tables and detailed explanations, please refer to the `feature/static-partitioning` branch README or the detailed blog post.

---

## 🚀 Key Features

- **HTTP/2 support** based on [nghttp2 v1.68.0](https://nghttp2.org)
- **TLS 1.2 / 1.3 support** based on [OpenSSL v3.0.13](https://www.openssl.org/)
- HTTP/2 Cleartext (h2c) support via the `--h2c` option
- Event-driven I/O model based on `epoll (edge-triggered mode)`
- `mmap`-based file caching
- Support for registering custom `GET` and `POST` handlers through `Router`
- Support for **multipart/form-data** uploads
- **Custom load balancer based on an indirection table** for efficient multithreaded worker dispatch
- **Bounded CircularQueue** for safe scheduling and predictable memory usage under overload :contentReference[oaicite:3]{index=3}

---

## 🛠 Tech Stack

- **OS**: Linux (Ubuntu 24.04)
- **Language**: C++17
- **Networking**: `epoll`, sockets, non-blocking I/O
- **Protocol**: HTTP/2 ([nghttp2](https://nghttp2.org)), TLS ([OpenSSL](https://www.openssl.org))
- **Performance**: Multithreaded session management, `mmap`-based file caching :contentReference[oaicite:4]{index=4}

---

## ⚙️ Build

```bash
git clone https://github.com/hyeonsu-choe/h2server.git
cd h2server
make -j$(nproc)
````

> **Note**
> The command above assumes that the **Makefile** is located in the repository root.  
> If the `Makefile` is under `src/`, run:
>
> ```bash
> cd h2server/src
> make -j$(nproc)
> ```



---

## 🐳 Docker Build

```bash
git clone https://github.com/hyeonsu-choe/h2server.git
cd h2server
docker build -t h2-server -f ./docker_build/Dockerfile .
```

> **Note**
> `docker build` must be executed from the project root directory, which includes both `docker_build/` and `src/`. 

---

## 🖥 Usage

### Command Format

```bash
./h2server [OPTIONS]
```

### 📋 Options

| Flag                  | Type / Default               | Description                                                       |
|----------------------|------------------------------|-------------------------------------------------------------------|
| `-p, --port <NUM>`   | integer / `443`              | TCP port to listen on.                                            |
| `-k, --key <PATH>`   | path / `./cert/server.key`   | Path to TLS private key file. <br>Not required when using `--h2c`.   |
| `-c, --cert <PATH>`  | path / `./cert/server.crt`   | Path to TLS certificate file.<br> Not required when using `--h2c`.   |
| `-n, --threads <NUM>`| integer / `1`                | Number of worker threads.                                         |
| `--h2c`              | flag                         | Enable HTTP/2 cleartext mode (no TLS).                            |
| `-h, --help`         | flag                         | Show help and exit.                                               |

### Example Runs

#### A) TLS-based H2 over HTTPS

```bash
./h2server --port 443 --key ./cert/server.key --cert ./cert/server.crt --threads 4
```

#### B) Cleartext H2C over HTTP

```bash
./h2server --port 8080 --h2c --threads 4
```



---

## 🐳 Run with Docker

### A) Direct Run

```bash
# TLS example: use port 443
docker run -d --name h2server \
  -e RUN_MODE=h2 \
  -e PORT=443 \
  -e THREADS=4 \
  -p 443:443 h2-server
```

```bash
# H2C example: use port 8080
docker run -d --name h2server-h2c \
  -e RUN_MODE=h2c \
  -e PORT=8080 \
  -e THREADS=4 \
  -p 8080:8080 h2-server
```

### B) Run with docker-compose

`docker_build/docker-compose.yml` (example)

```yaml
services:
  h2_server:
    image: h2-server
    environment:
      - RUN_MODE=h2
      - PORT=443
      - THREADS=1
    ports:
      - 0.0.0.0:443:443
      - :::443:443
```

Run:

```bash
cd docker_build
docker-compose up -d
```



---

## Routing & Handlers

### Handler Registration

`Router` maps incoming HTTP/2 requests (`method + path`) to user-defined handlers.  
This cleanly separates I/O and protocol handling (`nghttp2` / `OpenSSL` / `epoll`) from application logic.  

* Supported methods: `GET`, `POST`
* Registration timing: routes must be registered before server startup (`listen_and_serve`)

```cpp
// Example: path parameter {file_name}
server.add_handler(Method::GET,  "/files/{file_name}", downloader);
server.add_handler(Method::POST, "/files",            uploader);
```

* Path parameter: `{file_name}` in `/files/{file_name}`
* Matching priority:

  * `exact > parameter > wildcard` 

### Handler Signature

Handlers use the following form:

```cpp
int handler(Request& request)
```

Example: file download handler

```cpp
int downloader(Request& request)
{
    StreamData* stream_data = request.stream_data;
    const std::string& rel_path = request.rel_path;

    if (rel_path.empty()) {
        return request.reply_404();
    }

    auto file = load_file_from_filecache(rel_path);
    if (!file) {
        return request.reply_404();
    }

    stream_data->file_ctx.data = file->get_data();
    stream_data->file_ctx.size = file->get_data_len();

    return request.reply_ok_with_file();
}
```

Response helpers

* `reply_ok_with_file()` - returns `200 OK` with file body
* `reply_ok()` - returns `200 OK` without body
* `reply_404()` - returns `404 Not Found` 

---

## 📊 Benchmark Summary (origin baseline)

This document includes benchmark results for the **origin branch**.  
These initial benchmarks focused on validating the core design choices, including `mmap` caching, TLS handling, and multithreaded scalability.

The main trends observed in the original benchmark were:

* `nghttpd` achieved higher throughput in single-threaded scenarios
* `h2server` showed better scalability in multithreaded ranges
* Tail latency improved significantly with the `mmap`-based cache
* CPU usage was higher, but throughput and latency showed advantages under high-load multithreaded conditions 

---

## 📊 Benchmark (`h2load`)

Because the benchmarks were performed in a VMware-based virtualized environment, measurement variance (Max-Min) can be relatively large due to host OS (Windows) scheduling interference and resource contention. 

* **Statistical approach**

  * Since simple averages can be distorted by outliers, performance was evaluated based on repeated runs and percentile metrics such as **P50** and **P99**
* **Significance controls**

  * CPU affinity (`taskset`) and warm-up time were used to reduce interference from the virtualization layer 

> 🧪 **Benchmark methodology**
>
> * Each benchmark case was executed 5 times consecutively, and the resulting values were used.
> * Latency percentiles were calculated from TSV (Tab-Separated Values) logs produced by `h2load`.
> * CPU and memory usage were collected with `pidstat` during a 60-second load interval. 

> 📌 **Key findings**
>
> * The MMAP optimization reduced p99 latency by more than 60% in both low- and high-concurrency environments.
> * TLS introduced moderate overhead (roughly 20–30 ms in p99 latency), while overall performance remained stable.
> * Without `mmap`-based caching, many files were opened simultaneously during response processing, increasing the number of file descriptors and eventually causing resource exhaustion and higher failure rates. 

### 🌐 Benchmark Environment

All benchmarks were conducted in a VMware-based virtual environment:

* **Host OS**: Windows 11 Pro (64-bit, 24H2)
* **Virtualization**: VMware Workstation 17 Pro
* **Guest OS**: Ubuntu 22.04 LTS (64-bit)
* **Host CPU**: AMD Ryzen 5 7500F (6-Core)
* **vCPU allocation**: 4 cores for the server, 2 cores for the client
* **Allocated memory**: 8 GB
* **Disk**: NVMe SSD-backed virtual disk
* **Network**: Host-only network through VMware virtual interfaces
* **Kernel version**: 6.17
* **Compiler**: g++ 13.3.0 (C++17)
* **Benchmark tools**:

  * `h2load`: v1.68.0
  * `pidstat`: v12.6.1 

All benchmarks were performed using `h2load` from nghttp2:

```bash
./h2load -c<clients> -m<streams> --warm-up-time=5 -D 60 <web server addr>/index.html
```

⚠️ **Note**: the `/index.html` file size is 158 bytes. 

### ▶️ Client-side Test Command

```bash
./h2load -c1000 -m100 --warm-up-time=5 -D 60 https://test.com/index.html --log-file=result.tsv
```

P99, P90, and P50 latencies were calculated from the TSV output as follows:

```bash
# p99
total=$(cut -f3 result.tsv | wc -l); p99=$(echo "$total * 0.99" | bc | cut -d. -f1); cut -f3 result.tsv | sort -n | sed -n "${p99}p"

# p90
total=$(cut -f3 result.tsv | wc -l); p90=$(echo "$total * 0.90" | bc | cut -d. -f1); cut -f3 result.tsv | sort -n | sed -n "${p90}p"

# p50
total=$(cut -f3 result.tsv | wc -l); p50=$(echo "$total * 0.50" | bc | cut -d. -f1); cut -f3 result.tsv | sort -n | sed -n "${p50}p"
```

Server-side CPU and memory usage were measured with:

```bash
pidstat -r -u -p <PID> 1 60
```



### 🏆 Performance Comparison

| Server | Version / Commit | QPS | Success Rate (%) | p99 Latency (ms) | p90 Latency (ms) | p50 Latency (ms)  | CPU Usage (%) | Memory Usage (MB) |
|---------------|-------------|------------|-----------|--------|-------|----------|---------|----------|
| h2server | c34cd1d9ce | 205574.332| 100% | 547.7828 | 508.3812 | 486.7182 | 99.942 | 141.19  |
| libevent-server | nghttp2 1.60.0 | 204798.998 |100% | 541.5324 | 507.1514 | 479.899 | 99.912 | 95.45 |
| nghttpd | nghttp2 1.60.0 | 321848.43 | 100% | 235.788 | 166.4234 | 157.616 | 85.59 | 112.38 |

| ![table1_qps](./h2_c1000_m100_3servers_qps.png) | ![table1_p99](./h2_c1000_m100_3servers_p99.png) |
|:------------------------------------:|:------------------------------------:|
| **QPS (Throughput): Higher is better**                 | **p99 Latency: Lower is better**                      |

| ![table1_cpu](./h2_c1000_m100_3servers_cpu.png) | ![table1_mem](./h2_c1000_m100_3servers_mem.png) |
|:------------------------------------:|:------------------------------------:|
| **CPU Usage**                        | **Memory Usage**                     |

⚠️ **Note**: for **libevent-server**, running `h2load -c1000 -m100` under the default file descriptor limit (1024) caused resource exhaustion during response handling, dropping the success rate below 20%, which made meaningful benchmarking impossible.  
Therefore, for `libevent-server`, the file descriptor limit was raised from 1,024 to 65,535 with `ulimit -n` before benchmarking. 

### 📈 Thread Scaling

> **Test notes**
>
> * **libevent-server** supports only a single thread, so it was excluded from the scalability comparison.
> * For **nghttpd**, running with `-n 8` (8 worker threads) caused `h2load` to hang under the default file descriptor limit (1024). Only this test was completed after increasing the limit to **65,535** with `ulimit -n`.
> * All other tests used the default file descriptor limit. 

#### A. QPS
| Threads | h2server | nghttpd |
|:---------:|:----------:|:---------:|
| **1** |205574.332 |	321848.43 |
| **2** |393091.732 |	358788.664 |
| **4** |403797.186 |	367662.9 |
| **8** |404569.212 |	348835.566|

| ![table2_qps](./h2_c1000_m100_qps_scaling.png) |
|:------------------------------------:|
| **QPS (Throughput): Higher is better** |

#### B. p99 Latency
| Threads | h2server | nghttpd |
|:---------:|:----------:|:---------:|
| **1** | 547.7828 |	235.788 |
| **2** | 305.2898 |	188.0264|
| **4** | 157.3542 |	177.935 |
| **8** | 152.979	| 182.8562 |

| ![table2_p99](./h2_c1000_m100_p99_scaling.png) |
|:------------------------------------:|
| **p99 Latency: Lower is better** |

#### C. CPU Usage
| Threads | h2server | nghttpd |
|:---------:|:----------:|:---------:|
| **1**|99.942	| 85.59 |
| **2**|194.396	| 105.616 |
| **4**|203.208	| 113.07 |
| **8**|214.93	| 122.024 |


| ![table2_cpu](./h2_c1000_m100_cpu_scaling.png) |
|:------------------------------------:|
| **CPU Usage** |

#### D. Memory Usage
| Threads | h2server | nghttpd |
|:---------:|:----------:|:---------:|
| **1** | 141.1904297	| 112.3808594 |
| **2** | 136.2841797	| 116.7830078 |
| **4** | 141.8414063	| 115.0414063 |
| **8** | 143.1271484	| 112.4423828 |


| ![table2_mem](./h2_c1000_m100_mem_scaling.png) |
|:------------------------------------:|
| **Memory Usage** |


### ✅ Benchmark Summary

1. **Throughput (QPS)**

   * In the single-threaded case, `nghttpd` showed higher throughput (205K vs 321K).
   * In the 4–8 thread range, `h2server` scaled more effectively and outperformed `nghttpd`.

     * h2server: 404K QPS
     * nghttpd: 367K QPS

   👉 In multithreaded environments, `h2server` demonstrated better scalability.

2. **p99 Latency**

   * In the 1–2 thread range, `nghttpd` showed lower latency.
   * As the number of threads increased, `h2server` reduced latency more effectively and maintained lower p99 latency in the 4–8 thread range.

   👉 Under multithreaded workloads, `h2server` showed more stable and lower tail latency.

3. **CPU Usage**

   * `h2server` used CPU resources much more aggressively (for example, 214% vs 122% at 8 threads).
   * This indicates a trade-off: higher CPU usage in exchange for higher throughput and lower latency.

4. **Memory Usage**

   * `nghttpd` consistently used less memory across all tests.
   * `h2server` used slightly more memory (about 141 MB at 1 thread → about 143 MB at 8 threads).

5. **Overall Interpretation**

   * `h2server`: at the cost of higher CPU usage, it delivered higher throughput, lower latency, and stronger scalability under multithreaded workloads.
   * `nghttpd`: strong in single-thread efficiency and memory efficiency, but more limited in scalability.

   👉 One-line summary: **“nghttpd is stronger in single-thread efficiency, while h2server is stronger in multithread scalability and tail latency (p99).”** 

---

## Detailed Docs

For more detailed design background, implementation notes, later architectural improvements, and the latest benchmark results, please refer to the document below.

* [Detailed Documentation](https://hyeonsu-choe.github.io/posts/h2_server/) 

---

## 📜 License

This project is licensed under the [MIT License](./LICENSE). 

---

## 🧑‍💻 Contact

* **Maintainer**: Hyeonsu Choi
* **Email**: [hyeonsu.choe@gmail.com](mailto:hyeonsu.choe@gmail.com)
* **GitHub**: [github.com/hyeonsu-choe](https://github.com/hyeonsu-choe) 
