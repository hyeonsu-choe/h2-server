# H2 Server

> 🇰🇷 [View Korean Version](../README.md)

**h2server** is a high-performance HTTP/2 server implemented in **Modern C++**.  
It is built on Linux using `epoll`, `io_uring`, `nghttp2`, and `OpenSSL`, and was developed with the goal of delivering low-latency, high-performance file serving through an `mmap`-based file cache and a multithreaded worker architecture. :contentReference[oaicite:0]{index=0}

This project goes beyond simply implementing HTTP/2 features.  
It is a personal project intended to directly validate and benchmark system-level design concepts such as **event-driven I/O**, **TLS handling**, **multithreaded scalability**, **load balancing**, and **static partitioning**. :contentReference[oaicite:1]{index=1}

[[Detailed Documentation]](https://hyeonsu-choe.github.io/posts/h2_server/)

---

## Table of Contents

- [Key Highlights](#key-highlights)
- [Features](#features)
- [Architecture](#architecture)
- [Tech Stack](#tech-stack)
- [Quick Start](#quick-start)
- [Build](#build)
- [Run](#run)
- [Routing & Handler Example](#routing--handler-example)
- [Benchmark Summary](#benchmark-summary)
- [Static Partitioning Benchmark](#static-partitioning-benchmark)
- [Detailed Docs](#detailed-docs)
- [License](#license)
- [Contact](#contact)

---

## Key Highlights

- **A custom-built HTTP/2 server implemented in Modern C++**
- Support for **H2 / TLS 1.2 / TLS 1.3** via `nghttp2` + `OpenSSL`
- Event-driven asynchronous I/O model based on `epoll (ET)` and `io_uring`
- Support for **HTTP/2 Cleartext** through `--h2c` and `--h2c-io-uring`
- Reduced file I/O overhead with an `mmap`-based file cache
- Multithreaded worker architecture and a custom load balancer
- Experiments with SOREUSEPORT-based static partitioning and the H2C completion model
- Achieved up to **605,540 QPS** in the latest `static-partitioning` benchmark :contentReference[oaicite:2]{index=2}

---

## Features

- **HTTP/2 support** based on [nghttp2](https://nghttp2.org)
- **TLS 1.2 / 1.3 support** based on [OpenSSL](https://www.openssl.org/)
- HTTP/2 Cleartext (h2c) support via `--h2c` and `--h2c-io-uring`
- Event-driven asynchronous I/O model based on `epoll (edge-triggered mode)` and `io_uring (multishot)`
- `mmap`-based file caching
- Support for registering `GET` and `POST` handlers through `Router`
- Support for **multipart/form-data** uploads
- **Custom load balancer based on an indirection table**
- **Bounded CircularQueue** for predictable memory usage even under overload :contentReference[oaicite:3]{index=3}

---

## Architecture

This project was designed around the following goals:

- **Event-driven network processing**
  - `epoll`-based non-blocking I/O
  - `io_uring`-based non-blocking I/O (limited to the `--h2c-io-uring` mode)
- **Separation of protocol and application logic**
  - Separation between HTTP/2 session handling and user-defined handler logic
- **Multithreaded scalability**
  - Worker-based parallel processing
  - Experiments with load balancing and static partitioning
- **File-serving optimization**
  - `mmap`-based file caching
- **Protocol mode experimentation**
  - Comparison across H2 (TLS), H2C (cleartext), and H2C with `io_uring` :contentReference[oaicite:4]{index=4}

> For more detailed explanations of the design background and architecture, please refer to the [detailed documentation](https://hyeonsu-choe.github.io/posts/h2_server/).

---

## Branch Overview

This project evolved with a stronger focus on the **step-by-step experimentation and validation of a high-performance HTTP/2 server architecture** rather than on feature implementation alone.

- **origin**
  - The initial high-performance design based on an `acceptor → load balancer → worker` structure
  - A baseline branch used to validate the core behavior of HTTP/2, TLS, multithreaded processing, and file-serving parallelism

- **static-partitioning**
  - A branch that applies static partitioning to more clearly separate worker responsibilities and processing paths, while addressing the limitations of readiness-based abstraction
  - To complement the limitations of the existing readiness-centric approach, this branch applies a **completion model to the H2C path** and experiments with improved scalability and throughput under heavy load
  - This is currently the main branch containing the latest architectural improvements and performance experiments

Performance comparisons were conducted primarily between `origin` and `static-partitioning` to verify how architectural changes affected actual throughput (QPS) and thread scalability. :contentReference[oaicite:5]{index=5}

---

## Tech Stack

- **OS**: Linux (Ubuntu 24.04)
- **Language**: C++17
- **Networking**: `epoll`, `io_uring`, sockets, non-blocking I/O
- **Protocol**: HTTP/2 (`nghttp2`), TLS (`OpenSSL`)
- **Performance**: Multithreaded session management, `mmap`-based file caching :contentReference[oaicite:6]{index=6}

---

## Quick Start

### Build

```bash
git clone https://github.com/hyeonsu-choe/h2server.git
cd h2server
make -j$(nproc)
````

> This assumes that the `Makefile` is located at the repository root.
> If the `Makefile` is under `src/`, run:
>
> ```bash
> cd h2server/src
> make -j$(nproc)
> ```



### Run (TLS)

```bash
./h2server --port 443 --key ./cert/server.key --cert ./cert/server.crt --threads 4
```



### Run (H2C)

```bash
./h2server --port 8080 --h2c --threads 4
```



---

## Build

### Native Build

```bash
git clone https://github.com/hyeonsu-choe/h2server.git
cd h2server
make -j$(nproc)
```

### Docker Build

```bash
git clone https://github.com/hyeonsu-choe/h2server.git
cd h2server
docker build -t h2-server -f ./docker_build/Dockerfile .
```

> `docker build` must be executed from the project root directory, which includes both `docker_build/` and `src/`. 

---

## Run

### Command Format

```bash
./h2server [OPTIONS]
```

### Options

| Flag                  | Type / Default             | Description                  |
| --------------------- | -------------------------- | ---------------------------- |
| `-p, --port <NUM>`    | integer / `443`            | TCP listening port           |
| `-k, --key <PATH>`    | path / `./cert/server.key` | TLS private key path         |
| `-c, --cert <PATH>`   | path / `./cert/server.crt` | TLS certificate path         |
| `-n, --threads <NUM>` | integer / `1`              | Number of worker threads     |
| `--h2c`               | flag                       | Enable HTTP/2 cleartext mode on epoll|
| `--h2c-io-uring`               | flag                       | Enable HTTP/2 cleartext mode on io_uring |
| `-h, --help`          | flag                       | Print help and exit          |

### Docker Run

#### TLS

```bash
docker run -d --name h2server \
  -e RUN_MODE=h2 \
  -e PORT=443 \
  -e THREADS=4 \
  -p 443:443 h2-server
```

#### H2C

```bash
docker run -d --name h2server-h2c \
  -e RUN_MODE=h2c \
  -e PORT=8080 \
  -e THREADS=4 \
  -p 8080:8080 h2-server
```

#### docker-compose Example

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

## Routing & Handler Example

### Handler Registration

`Router` maps incoming HTTP/2 requests (`method + path`) to user-defined handlers.
This allows I/O and protocol handling to be separated from application logic.

* Supported methods: `GET`, `POST`
* Registration timing: before server startup (`listen_and_serve` is called)

```cpp
server.add_handler(Method::GET,  "/files/{file_name}", downloader);
server.add_handler(Method::POST, "/files",            uploader);
```

* Path parameter: `{file_name}` in `/files/{file_name}`
* Matching priority:

  * `exact > parameter > wildcard` 

### Handler Signature

```cpp
int handler(Request& request)
```

Example:

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



Response helpers:

* `reply_ok_with_file()` - `200 OK` + file body
* `reply_ok()` - `200 OK`
* `reply_404()` - `404 Not Found` 

---

## Benchmark Summary

This project goes beyond simple feature implementation and has continuously compared and analyzed performance from the perspective of **QPS**, **latency**, **CPU/MEM usage**, and **thread scalability**.

Previous benchmarks showed the following trends:

* `nghttpd` delivered higher throughput in single-threaded scenarios
* `h2server` showed better scalability in multithreaded ranges
* Tail latency improved significantly with the `mmap`-based cache
* CPU usage was higher, but throughput and latency advantages appeared under high-load multithreaded conditions

> Please refer to the detailed documentation for previous benchmark graphs and full metrics. 

---

## Static Partitioning Benchmark

After upgrading to Ubuntu 24.04, performance measurements on the `static-partitioning` branch were conducted primarily with **QPS-focused** comparisons using only `h2load`, in order to reduce the overhead introduced by profiling tools. 

### Test Environment

* **Host OS**: Windows 11
* **Host CPU**: AMD Ryzen 5 7500F 6-Core Processor
* **Virtualization**: VMware Workstation Pro 25H2u1
* **Guest OS**: Ubuntu 24.04
* **vCPU allocation**:

  * Server: 4 cores
  * Client: 2 cores
* **RAM**: 8 GB each
* **Network**: VMware Host-only (`vmxnet3`)
* **Clients**: 1000
* **Max Streams**: 100
* **Duration**: 60s
* **Warm-up**: 5s
* **Repeat**: 5 runs 

### Average QPS

| Threads |  nghttpd h2 | nghttpd h2c | h2server h2 | h2server h2c | h2server h2c-io-uring |
| ------- | ----------: | ----------: | ----------: | -----------: | --------------------: |
| 1       | 237,691.400 | 265,844.880 | 204,269.332 |  231,736.878 |           215,849.772 |
| 2       | 357,122.614 | 415,930.222 | 281,325.252 |  313,524.066 |           406,326.790 |
| 4       | 374,448.666 | 457,900.000 | 426,418.254 |  555,360.632 |           553,962.568 |
| 8       | 402,795.666 | 454,465.334 | 408,376.000 |  538,905.998 |           546,887.852 |

### Max QPS

| Threads |  nghttpd h2 | nghttpd h2c | h2server h2 | h2server h2c | h2server h2c-io-uring |
| ------- | ----------: | ----------: | ----------: | -----------: | --------------------: |
| 1       | 274,885.750 | 279,853.330 | 234,428.330 |  245,015.670 |           218,185.600 |
| 2       | 417,983.830 | 473,583.330 | 299,750.000 |  326,406.670 |           421,581.880 |
| 4       | 417,850.000 | 507,730.000 | 461,459.470 |  585,625.000 |           582,921.700 |
| 8       | 406,971.670 | 498,368.330 | 417,993.330 |  597,368.330 |           605,540.000 |

### Summary

* `h2server h2c` and `h2server h2c-io-uring` outperform `nghttpd` from the 4-thread range and above
* The best-performing configuration is `h2server_h2c_io_uring_n8`
* **Max QPS: 605,540**
* Static partitioning and the completion model showed meaningful performance gains under high-load conditions 

---

## Detailed Docs

For more detailed design background, implementation notes, and benchmark analysis, please refer to the document below.

* [Detailed Documentation](https://hyeonsu-choe.github.io/posts/h2_server/) 

---

## License

This project is licensed under the [MIT License](./LICENSE). 

---

## Contact

* **Maintainer**: Hyeonsu Choi
* **Email**: [hyeonsu.choe@gmail.com](mailto:hyeonsu.choe@gmail.com)
* **GitHub**: [github.com/hyeonsu-choe](https://github.com/hyeonsu-choe) 

```

원하시면 이어서 바로, 이 영문 README를 **GitHub README에 더 자연스러운 오픈소스 스타일**로 한번 더 다듬은 버전도 만들어드리겠습니다.
```
