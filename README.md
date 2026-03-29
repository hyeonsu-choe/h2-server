# H2 Server

> 🇺🇸 [영문 버전 보기](./docs/README-eng.md)

**h2server**는 **Modern C++**로 구현한 고성능 HTTP/2 서버입니다.  
Linux 환경에서 `epoll`, `io_uring`, `nghttp2`, `OpenSSL`을 기반으로 동작하며,  
`mmap` 기반 파일 캐시와 멀티스레드 워커 구조 기반의 저지연·고성능 파일 서비스 제공을 목표로 개발했습니다.

이 프로젝트는 단순한 HTTP/2 기능 구현을 넘어,  
**이벤트 기반 I/O**, **TLS 처리**, **멀티스레드 확장성**, **로드 밸런싱**, **정적 분리(static partitioning)** 와 같은  
시스템 레벨 설계를 직접 검증하고 벤치마크하기 위한 개인 프로젝트입니다.

[[상세 문서 바로가기]](https://hyeonsu-choe.github.io/posts/h2_server/)

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

- **Modern C++ 기반 HTTP/2 서버 직접 구현**
- `nghttp2` + `OpenSSL` 기반 **H2 / TLS 1.2 / TLS 1.3** 지원
- `epoll (ET)`, `io_uring` 기반 이벤트 주도 비동기 I/O 모델
- `--h2c`, `--h2c-io-uring` 옵션을 통한 **HTTP/2 Cleartext** 지원
- `mmap` 기반 파일 캐시로 파일 I/O 오버헤드 절감
- 멀티스레드 워커 구조 및 커스텀 로드 밸런서 구현
- SOREUSEPORT 기반의 정적 분리(static partitioning) 및 H2C completion 모델 실험
- 최신 static-partitioning 벤치마크 기준 **최대 605,540 QPS** 달성

---

## Features

- [nghttp2](https://nghttp2.org) 기반 **HTTP/2 지원**
- [OpenSSL](https://www.openssl.org/) 기반 **TLS 1.2 / 1.3 지원**
- `--h2c`와 `--h2c-io-uring` 옵션으로 HTTP/2 Cleartext(h2c) 사용 가능
- `epoll (edge-triggered mode)`, `io_uring (multishot)` 기반 이벤트 주도 비동기 I/O 모델
- `mmap` 기반 파일 캐싱
- `Router`를 통한 `GET`, `POST` 핸들러 등록 지원
- **multipart/form-data** 업로드 지원
- **indirection table 기반 커스텀 로드 밸런서**
- 과부하 상황에서도 예측 가능한 메모리 사용을 위한 **Bounded CircularQueue**

---

## Architecture

이 프로젝트는 다음과 같은 목표를 중심으로 설계되었습니다.

- **이벤트 기반 네트워크 처리**
  - `epoll` 기반 non-blocking I/O
  - `io_uring` 기반 non-blocking I/O (--h2c-io-uring 옵션 사용 한정)
- **프로토콜/애플리케이션 로직 분리**
  - HTTP/2 세션 처리와 사용자 핸들러 로직 분리
- **멀티스레드 확장성**
  - 워커 기반 병렬 처리
  - 로드 밸런싱 및 정적 분리 구조 실험
- **파일 서비스 최적화**
  - `mmap` 기반 파일 캐싱
- **프로토콜 모드 실험**
  - H2(TLS), H2C(cleartext), H2C io_uring 비교

> 더 자세한 설계 배경과 구조 설명은 [상세 문서](https://hyeonsu-choe.github.io/posts/h2_server/)를 참고해 주세요.

---

## Branch Overview

이 프로젝트는 기능 구현 자체보다도, **고성능 HTTP/2 서버 아키텍처를 단계적으로 실험·검증하는 과정**에 초점을 두고 발전시켰습니다.

- **origin**
  - `acceptor → load balancer → worker` 구조를 기반으로 한 초기 고성능 설계
  - HTTP/2, TLS, 멀티스레드 처리, 파일 서비스의 기본 동작과 병렬 처리 구조를 검증한 베이스라인 브랜치

- **static-partitioning**
  - 워커의 책임과 처리 경로를 더 명확히 분리하고, readiness 추상화의 한계를 보완하기 위해 정적 분리(static partitioning)를 적용한 브랜치
  - 기존 readiness 중심 접근의 한계를 보완하기 위해, **H2C 경로에 completion 모델을 적용**하여 고부하 환경에서의 확장성과 처리량 개선을 실험
  - 현재는 각종 구조 개선과 성능 실험 결과가 이 브랜치에 반영된 최신 브랜치

성능 비교는 주로 `origin`과 `static-partitioning`을 기준으로, 아키텍처 변경이 실제 처리량(QPS)과 스레드 확장성에 어떤 영향을 주는지 검증하는 방식으로 진행했습니다.

---

## Tech Stack

- **OS**: Linux (Ubuntu 24.04)
- **Language**: C++17
- **Networking**: `epoll`, `io_uring`, socket, non-blocking I/O
- **Protocol**: HTTP/2 (`nghttp2`), TLS (`OpenSSL`)
- **Performance**: 멀티스레드 세션 관리, `mmap` 기반 파일 캐싱

---

## Quick Start

### Build

```bash
git clone https://github.com/hyeonsu-choe/h2server.git
cd h2server
make -j$(nproc)
````

> 저장소 루트에 `Makefile`이 있다고 가정합니다.  
> `Makefile`이 `src/` 하위에 있다면:  
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

> `docker build`는 프로젝트 루트(`docker_build/`와 `src/`를 모두 포함하는 디렉터리)에서 실행해야 합니다.

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

`Router`는 들어오는 HTTP/2 요청(`method + path`)을 사용자 정의 핸들러에 매핑합니다.  
이를 통해 I/O 및 프로토콜 처리와 애플리케이션 로직을 분리합니다.

* 지원 메서드: `GET`, `POST`
* 등록 시점: 서버 시작 전 (`listen_and_serve` 호출 전)

```cpp
server.add_handler(Method::GET,  "/files/{file_name}", downloader);
server.add_handler(Method::POST, "/files",            uploader);
```

* 경로 파라미터: `/files/{file_name}` 의 `{file_name}`
* 매칭 우선순위:

  * `exact > parameter > wildcard`

### Handler Signature

```cpp
int handler(Request& request)
```

예시:

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

응답 헬퍼:

* `reply_ok_with_file()` - `200 OK` + file body
* `reply_ok()` - `200 OK`
* `reply_404()` - `404 Not Found`

---

## Benchmark Summary

이 프로젝트는 단순 기능 구현을 넘어서,  
**QPS**, **지연 시간**, **CPU/MEM 사용량**, **스레드 확장성**을 중심으로 지속적으로 성능을 비교·분석해 왔습니다.

기존 벤치마크에서는 다음과 같은 경향을 확인했습니다.

* 단일 스레드에서는 `nghttpd`가 더 높은 처리량을 보임
* 멀티스레드 구간에서는 `h2server`가 더 나은 확장성을 보임
* `mmap` 기반 캐시 적용 시 tail latency가 크게 개선됨
* CPU 사용량은 더 높지만, 고부하 멀티스레드 환경에서 처리량과 지연 시간 측면의 이점이 있음

> 기존 상세 벤치마크 그래프 및 수치는 상세 문서를 참고해 주세요.

---

## Static Partitioning Benchmark

Ubuntu 24.04 업그레이드 이후의 `static-partitioning` 기준 성능 측정에서는,  
프로파일링 도구 사용 시 발생되는 오버헤드를 줄이기 위해 `h2load`만 사용하여 **QPS 중심**으로 비교했습니다.

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

* 4스레드 이상 구간에서 `h2server h2c` 및 `h2server h2c-io-uring`이 `nghttpd`를 추월
* 최고 성능은 `h2server_h2c_io_uring_n8`
* **Max QPS: 605,540**
* 정적 분리(static partitioning)와 completion 모델이 고부하 구간에서 유의미한 성능을 보여줌

---

## Detailed Docs

더 자세한 설계 배경, 구현 과정, 벤치마크 해석은 아래 문서를 참고해 주세요.

* [상세 문서 바로가기](https://hyeonsu-choe.github.io/posts/h2_server/)

---

## License

This project is licensed under the [MIT License](./LICENSE).

---

## Contact

* **Maintainer**: Hyeonsu Choi
* **Email**: [hyeonsu.choe@gmail.com](mailto:hyeonsu.choe@gmail.com)
* **GitHub**: [github.com/hyeonsu-choe](https://github.com/hyeonsu-choe)

```