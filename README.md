# H2 Server

> 🇺🇸 [영문 버전 보기](./docs/README-eng.md)

**h2server**는 **`Modern C++`**로 구현한 고성능 HTTP/2 서버입니다.  
Linux 환경에서 동작하며 `epoll`, [`nghttp2`](https://nghttp2.org), [OpenSSL](https://www.openssl.org/)을 사용합니다.  
선택적인 TLS 연결을 지원하고, `mmap` 기반 캐시를 통해 저지연 파일 서비스를 제공합니다.  
이 프로젝트는 제가 이전에 Golang으로 개발했던 [webserver](https://github.com/hyeonsu-choe/websvr)에서 영감을 받아 시작되었으며, 이벤트 기반 I/O, TLS 처리, HTTP/2 프로토콜과 같은 시스템 레벨 기술들을 학습하고 벤치마크하기 위한 개인 프로젝트로써 개발되었습니다.

[[상세 문서 바로가기]](https://hyeonsu-choe.github.io/posts/h2_server/)

---

## 📌 Branch Overview

이 저장소는 단일 구현에 머무르지 않고, **고성능 HTTP/2 서버 아키텍처를 단계적으로 실험·검증하는 방향으로 발전**했습니다.

- **origin**
  - `acceptor → load balancer → worker` 구조를 기반으로 한 초기 고성능 설계
  - HTTP/2, TLS, 멀티스레드 처리, 파일 서비스, `mmap` 캐시의 기본 구조와 성능 특성을 검증한 베이스라인 브랜치

- **feature/static-partitioning**
  - 워커 책임과 처리 경로를 더 명확히 분리하기 위해 **static partitioning**을 적용한 실험 브랜치
  - readiness 중심 구조의 한계를 보완하기 위해 **H2C 경로에 completion 모델**을 도입
  - 최신 구조 개선과 성능 실험 결과가 반영된 브랜치

> 이 문서는 **origin 브랜치 기준**의 구조와 벤치마크를 설명합니다.  
> 최신 구조와 최신 성능 결과는 `feature/static-partitioning` 브랜치를 참고해 주세요.

### Latest branch snapshot (`feature/static-partitioning`)

Ubuntu 24.04 환경에서 수행한 최신 benchmark에서는 다음과 같은 결과를 확인했습니다.

- 4스레드 이상 구간에서 `h2server h2c` 및 `h2server h2c-io-uring`이 `nghttpd`를 추월
- 최고 성능은 `h2server_h2c_io_uring_n8`
- **Max QPS: 605,540**
- static partitioning 및 completion 모델이 고부하 구간에서 의미 있는 처리량 개선을 보여줌

> 최신 benchmark 상세 표와 설명은 `feature/static-partitioning` 브랜치 README 또는 상세 문서를 참고해 주세요.

---

## 🚀 Key Features

- [nghttp2 v1.68.0](https://nghttp2.org) 기반 **HTTP/2 지원**
- [OpenSSL v3.0.13](https://www.openssl.org/) 기반 **TLS 1.2/1.3 지원**
- `--h2c` 옵션으로 HTTP/2 Cleartext(h2c) 사용 가능
- `epoll (edge-triggered mode)` 기반 이벤트 주도 I/O 모델
- `mmap` 기반 파일 캐싱
- `Router`를 통해 `GET`, `POST` 메서드에 대한 사용자 정의 핸들러 등록 지원
- **multipart/form-data** 업로드 지원
- 효율적인 멀티스레드 워커 디스패치를 위한 **indirection table 기반 커스텀 로드 밸런서**
- 과부하 상황에서도 안전한 스케줄링과 예측 가능한 메모리 사용을 위한 **Bounded CircularQueue**

---

## 🛠 Tech Stack

- OS: Linux(Ubuntu 24.04)
- Language: C++17
- Networking: epoll, socket, non-blocking I/O
- Protocol: HTTP/2([nghttp2](https://nghttp2.org)), TLS([OpenSSL](https://www.openssl.org))
- Performance: 멀티스레드 세션 관리, `mmap` 기반 파일 캐싱

---

## ⚙️ Build

```bash
git clone https://github.com/hyeonsu-choe/h2server.git
cd h2server
make -j$(nproc)
````

> **참고**
> 위 명령은 저장소 내 루트 경로에 **Makefile**이 있다고 가정합니다.  
> `Makefile`이 `src/` 아래에 있다면 다음처럼 실행하세요:
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

> **참고**
> `docker build`는 프로젝트 루트(`docker_build/`와 `src/`를 모두 포함하는 디렉터리)에서 실행해야 합니다.

---

## 🖥 Usage

### 실행 형식

```bash
./h2server [OPTIONS]
```

### 📋 Options

| Flag                  | Type / Default             | 설명                                   |
| --------------------- | -------------------------- | ------------------------------------ |
| `-p, --port <NUM>`    | integer / `443`            | 수신 대기할 TCP 포트                        |
| `-k, --key <PATH>`    | path / `./cert/server.key` | TLS 개인 키 파일 경로. <br>`--h2c` 사용 시 불필요 |
| `-c, --cert <PATH>`   | path / `./cert/server.crt` | TLS 인증서 파일 경로. <br>`--h2c` 사용 시 불필요  |
| `-n, --threads <NUM>` | integer / `1`              | 워커 스레드 개수                            |
| `--h2c`               | flag                       | HTTP/2 cleartext 모드(TLS 없음) 활성화      |
| `-h, --help`          | flag                       | 도움말 출력 후 종료                          |

### 실행 예시

#### A) TLS 기반 H2, HTTPS

```bash
./h2server --port 443 --key ./cert/server.key --cert ./cert/server.crt --threads 4
```

#### B) Cleartext 기반 H2C, HTTP

```bash
./h2server --port 8080 --h2c --threads 4
```

---

## 🐳 Run with Docker

### A) 직접 실행

```bash
# TLS 예시: 443 포트 사용
docker run -d --name h2server \
  -e RUN_MODE=h2 \
  -e PORT=443 \
  -e THREADS=4 \
  -p 443:443 h2-server
```

```bash
# H2C 예시: 8080 포트 사용
docker run -d --name h2server-h2c \
  -e RUN_MODE=h2c \
  -e PORT=8080 \
  -e THREADS=4 \
  -p 8080:8080 h2-server
```

### B) docker-compose로 실행

`docker_build/docker-compose.yml` (예시)

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

실행:

```bash
cd docker_build
docker-compose up -d
```

---

## Routing & Handlers

### 핸들러 등록

**Router**는 들어오는 HTTP/2 요청(`method + path`)을 사용자 정의 핸들러에 매핑합니다.  
이를 통해 I/O 및 프로토콜 처리(`nghttp2`/`OpenSSL`/`epoll`)와 애플리케이션 로직을 분리합니다.

* 지원 메서드: `GET`, `POST`
* 등록 시점: 서버 시작 전(`listen_and_serve` 호출 전)

```cpp
server.add_handler(Method::GET,  "/files/{file_name}", downloader);
server.add_handler(Method::POST, "/files",            uploader);
```

* 경로 파라미터: `/files/{file_name}`의 `{file_name}`
* 매칭 우선순위:

  * `exact > parameter > wildcard`

### 핸들러 시그니처

핸들러는 `int handler(Request& request)` 형태를 사용합니다.

예시: 파일 다운로드 핸들러

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

응답 헬퍼

* `reply_ok_with_file()` - 파일 본문과 함께 `200 OK` 응답
* `reply_ok()` - 본문 없이 `200 OK`
* `reply_404()` - `404 Not Found`

---

## 📊 Benchmark Summary (origin baseline)

이 문서는 **origin 브랜치 기준** 벤치마크를 포함합니다.  
이 초기 benchmark는 `mmap` 캐시, TLS 처리, 멀티스레드 확장성 등 **기본 설계의 타당성 검증**에 초점을 두었습니다.

기존 benchmark에서 확인한 핵심 경향은 다음과 같습니다.

* 단일 스레드에서는 `nghttpd`가 더 높은 처리량을 보임
* 멀티스레드 구간에서는 `h2server`가 더 나은 확장성을 보임
* `mmap` 기반 캐시 적용 시 tail latency가 크게 개선됨
* CPU 사용량은 더 높지만, 고부하 멀티스레드 환경에서 처리량과 지연 시간 측면의 이점이 있음

---

## 📊 Benchmark (`h2load` 기준)

VMware 가상화 환경 특성상 호스트 OS(Windows)의 스케줄링 간섭 및 자원 경합으로 인해 측정값의 변동 폭(Max-Min)이 다소 크게 발생합니다.

* 통계적 접근:

  * 단순 평균값은 이상치(Outlier)에 왜곡될 소지가 있어, 총 5회 이상의 반복 측정 후 중앙값(Median) 및 **백분위수(P50, P99)** 를 기준으로 성능을 평가함
* 유의성 판단:

  * 가상화 레이어의 간섭을 최소화하기 위해 CPU Affinity(`taskset`)를 설정하고, Warm-up 타임을 부여함

> 🧪 **벤치마크 방법**
>
> * 각 벤치마크는 케이스 별 5회씩 연속 실행 후 그 결과 값을 사용했습니다.
> * 지연 시간 백분위는 `h2load`가 출력한 TSV(Tab-Separated Values) 로그를 기준으로 계산했습니다.
> * CPU 및 메모리 사용량은 60초 부하 구간 동안 `pidstat`로 수집했습니다.

> 📌 **핵심 결과**
>
> * MMAP 최적화는 낮은 동시성과 높은 동시성 환경 모두에서 p99 지연 시간을 60% 이상 줄였습니다.
> * TLS는 중간 정도의 오버헤드(p99 기준 약 20~30ms)를 유발했지만, 전체 성능은 안정적으로 유지되었습니다.
> * `mmap` 기반 캐시가 없을 경우 응답 처리 중 많은 파일이 동시에 열리면서 파일 디스크립터 수가 증가했고, 결국 리소스 한계에 도달하면서 실패율 증가로 이어졌습니다.

### 🌐 Benchmark 환경

모든 벤치마크는 VMware 기반 가상 환경에서 수행되었습니다:

* **Host OS**: Windows 11 Pro (64-bit, 24H2)
* **가상화 환경**: VMware Workstation 17 Pro
* **Guest OS**: Ubuntu 22.04 LTS (64-bit)
* **Host CPU**: AMD Ryzen 5 7500F (6-Core)
* **vCPU 구성**: 서버에 4개 코어, 클라이언트에 2개 코어 할당
* **할당 메모리**: 8 GB
* **디스크**: NVMe SSD 기반 가상 디스크
* **네트워크**: VMware 가상 인터페이스를 통한 Host-only network
* **커널 버전**: 6.17
* **컴파일러**: g++ 13.3.0 (C++17)
* **벤치마크 도구**

  * `h2load`: v1.68.0
  * `pidstat`: v12.6.1

모든 벤치마크는 nghttp2의 `h2load`를 사용해 수행했습니다:

```bash
./h2load -c<clients> -m<streams> --warm-up-time=5 -D 60 <web server addr>/index.html
```

⚠️ **참고**: `/index.html` 파일 크기는 158바이트입니다.

### ▶️ 클라이언트 측 테스트 명령

```bash
./h2load -c1000 -m100 --warm-up-time=5 -D 60 https://test.com/index.html --log-file=result.tsv
```

p99, p90, p50 지연 시간은 TSV 파일로부터 다음과 같이 계산했습니다:

```bash
# p99
total=$(cut -f3 result.tsv | wc -l); p99=$(echo "$total * 0.99" | bc | cut -d. -f1); cut -f3 result.tsv | sort -n | sed -n "${p99}p"

# p90
total=$(cut -f3 result.tsv | wc -l); p90=$(echo "$total * 0.90" | bc | cut -d. -f1); cut -f3 result.tsv | sort -n | sed -n "${p90}p"

# p50
total=$(cut -f3 result.tsv | wc -l); p50=$(echo "$total * 0.50" | bc | cut -d. -f1); cut -f3 result.tsv | sort -n | sed -n "${p50}p"
```

서버 측 CPU 및 메모리 사용량은 다음 명령으로 측정했습니다:

```bash
pidstat -r -u -p <PID> 1 60
```

### 🏆 성능 비교

| Server          | Version / Commit |        QPS | Success Rate (%) | p99 Latency (ms) | p90 Latency (ms) | p50 Latency (ms) | CPU Usage (%) | Memory Usage (MB) |
| --------------- | ---------------: | ---------: | ---------------: | ---------------: | ---------------: | ---------------: | ------------: | ----------------: |
| h2server        |       c34cd1d9ce | 205574.332 |             100% |         547.7828 |         508.3812 |         486.7182 |        99.942 |            141.19 |
| libevent-server |   nghttp2 1.60.0 | 204798.998 |             100% |         541.5324 |         507.1514 |          479.899 |        99.912 |             95.45 |
| nghttpd         |   nghttp2 1.60.0 |  321848.43 |             100% |          235.788 |         166.4234 |          157.616 |         85.59 |            112.38 |

| ![table1\_qps](./docs/h2_c1000_m100_3servers_qps.png) | ![table1\_p99](./docs/h2_c1000_m100_3servers_p99.png) |
| :---------------------------------------------------: | :---------------------------------------------------: |
|             **QPS (Throughput): 높을수록 좋음**             |                **p99 Latency: 낮을수록 좋음**               |

| ![table1\_cpu](./docs/h2_c1000_m100_3servers_cpu.png) | ![table1\_mem](./docs/h2_c1000_m100_3servers_mem.png) |
| :---------------------------------------------------: | :---------------------------------------------------: |
|                      **CPU 사용량**                      |                      **메모리 사용량**                      |

⚠️ **참고**: `libevent-server`의 경우 기본 파일 디스크립터 제한(1024) 상태에서 `h2load -c1000 -m100`을 실행하면 응답 처리 중 리소스 한계에 도달하여 실패율이 20% 미만으로 떨어졌고, 이로 인해 유의미한 성능 측정이 불가능했습니다.  
따라서 `libevent-server`는 테스트 전에 `ulimit -n`으로 파일 디스크립터 제한을 기본값 1,024에서 65,535로 높인 뒤 벤치마크를 수행했습니다.

### 📈 스레드 확장 성능

> **테스트 참고 사항**
>
> * `libevent-server`: 단일 스레드만 지원하므로 비교 대상에서 제외했습니다.
> * `nghttpd`: `-n 8`(워커 스레드 8개)로 실행할 때 기본 파일 디스크립터 제한(1024) 때문에 `h2load`가 무한 대기 상태에 빠졌습니다. 이 테스트만 `ulimit -n`으로 제한을 **65,535**까지 높여 벤치마크를 완료했습니다.
> * 별도 언급이 없는 다른 테스트는 기본 파일 디스크립터 제한을 사용했습니다.

#### A. QPS

| Threads |   h2server |    nghttpd |
| :-----: | ---------: | ---------: |
|  **1**  | 205574.332 |  321848.43 |
|  **2**  | 393091.732 | 358788.664 |
|  **4**  | 403797.186 |   367662.9 |
|  **8**  | 404569.212 | 348835.566 |

| ![table2\_qps](./docs/h2_c1000_m100_qps_scaling.png) |
| :--------------------------------------------------: |
|             **QPS (Throughput): 높을수록 좋음**            |

#### B. p99 Latency

| Threads | h2server |  nghttpd |
| :-----: | -------: | -------: |
|  **1**  | 547.7828 |  235.788 |
|  **2**  | 305.2898 | 188.0264 |
|  **4**  | 157.3542 |  177.935 |
|  **8**  |  152.979 | 182.8562 |

| ![table2\_p99](./docs/h2_c1000_m100_p99_scaling.png) |
| :--------------------------------------------------: |
|               **p99 Latency: 낮을수록 좋음**               |

#### C. CPU Usage

| Threads | h2server | nghttpd |
| :-----: | -------: | ------: |
|  **1**  |   99.942 |   85.59 |
|  **2**  |  194.396 | 105.616 |
|  **4**  |  203.208 |  113.07 |
|  **8**  |   214.93 | 122.024 |

| ![table2\_cpu](./docs/h2_c1000_m100_cpu_scaling.png) |
| :--------------------------------------------------: |
|                      **CPU 사용량**                     |

#### D. Memory Usage

| Threads |    h2server |     nghttpd |
| :-----: | ----------: | ----------: |
|  **1**  | 141.1904297 | 112.3808594 |
|  **2**  | 136.2841797 | 116.7830078 |
|  **4**  | 141.8414063 | 115.0414063 |
|  **8**  | 143.1271484 | 112.4423828 |

| ![table2\_mem](./docs/h2_c1000_m100_mem_scaling.png) |
| :--------------------------------------------------: |
|                      **메모리 사용량**                     |

### ✅ 벤치마크 요약

1. **Throughput (QPS)**

   * 단일 스레드에서는 `nghttpd`가 더 높은 처리량을 보였습니다(205K vs 321K).
   * 4~8 스레드 환경에서는 `h2server`가 더 효과적으로 확장되며 `nghttpd`를 앞섰습니다.

     * h2server: 404K QPS
     * nghttpd: 367K QPS

   👉 멀티스레드 환경에서는 `h2server`가 더 나은 확장성을 보였습니다.

2. **p99 Latency**

   * 1~2 스레드 구간에서는 `nghttpd`가 더 낮은 지연 시간을 보였습니다.
   * 스레드 수가 증가할수록 `h2server`는 지연 시간을 더 효과적으로 낮추며, 4~8 스레드 구간에서 더 낮은 p99 지연 시간을 유지했습니다.

   👉 멀티스레드 워크로드에서는 `h2server`가 더 안정적이고 낮은 tail latency를 보여줍니다.

3. **CPU Usage**

   * `h2server`는 CPU 자원을 훨씬 더 적극적으로 활용했습니다(예: 8스레드에서 214% vs 122%).
   * 이는 더 높은 처리량과 더 낮은 지연 시간을 위해 CPU 사용량을 더 많이 소비하는 트레이드오프를 의미합니다.

4. **Memory Usage**

   * `nghttpd`는 모든 테스트에서 일관되게 더 낮은 메모리 사용량을 보였습니다.
   * `h2server`는 약간 더 많은 메모리를 사용했습니다(1스레드 약 141MB → 8스레드 약 143MB).

5. **전체 해석**

   * `h2server`: 더 높은 CPU 사용량을 대가로, 멀티스레드 환경에서 높은 처리량과 낮은 지연 시간, 강한 확장성을 보여줍니다.
   * `nghttpd`: 단일 스레드 성능과 메모리 효율성은 우수하지만, 확장성은 제한적입니다.

   👉 한 줄 요약: **“nghttpd는 단일 스레드 효율에 강하고, h2server는 멀티스레드 확장성과 tail latency(p99)에서 우수합니다.”**

---

## Detailed Docs

더 자세한 설계 배경, 구현 과정, 이후 브랜치에서의 구조 개선과 최신 benchmark는 아래 문서를 참고해 주세요.

* [상세 문서 바로가기](https://hyeonsu-choe.github.io/posts/h2_server/)

---

## 📜 License

이 프로젝트는 [MIT License](./LICENSE)를 따릅니다.

---

## 🧑‍💻 Contact

* **Maintainer**: Hyeonsu Choi
* **Email**: [hyeonsu.choe@gmail.com](mailto:hyeonsu.choe@gmail.com)
* **GitHub**: [github.com/hyeonsu-choe](https://github.com/hyeonsu-choe)
