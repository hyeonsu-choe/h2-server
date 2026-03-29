# build
FROM ubuntu:24.04 AS build-stage
WORKDIR /build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update
RUN apt-get install -y --no-install-recommends build-essential
RUN apt-get install -y --no-install-recommends autoconf automake 
RUN apt-get install -y --no-install-recommends libtool pkg-config
RUN apt-get install -y --no-install-recommends libssl-dev zlib1g-dev ca-certificates liburing-dev

RUN rm -rf /var/lib/apt/lists/*

ARG NGHTTP2_VER=1.68.0
COPY ./docker_build/nghttp2-${NGHTTP2_VER}.tar.gz /build/
RUN cd /build && tar -xvf nghttp2-${NGHTTP2_VER}.tar.gz
RUN cd /build/nghttp2-${NGHTTP2_VER} && ./configure --prefix=/usr && make -j && make install

COPY ./src/ /build/src
RUN cd /build/src && make

# runtime
FROM ubuntu:24.04 AS runtime-stage
WORKDIR /app

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update
RUN apt-get install -y --no-install-recommends libssl3 zlib1g ca-certificates liburing2

RUN rm -rf /var/lib/apt/lists/*

COPY --from=build-stage /usr/lib/x86_64-linux-gnu/liburing.so.* /usr/lib/x86_64-linux-gnu/
COPY --from=build-stage /usr/lib/libnghttp2.so.* /usr/lib/
COPY --from=build-stage /usr/include/nghttp2 /usr/include/nghttp2
COPY --from=build-stage /build/src/h2server /app/h2server
COPY --from=build-stage /build/src/index.html /app/index.html
COPY ./src/cert /app/cert
COPY ./docker_build/run.sh /app/run.sh

RUN chmod +x /app/h2server /app/run.sh

ENV PORT=443
ENV RUN_MODE=h2c
ENV THREADS=1

EXPOSE 80 443

#ENTRYPOINT ["tail", "-f", "/dev/null"]
CMD ["/bin/bash", "/app/run.sh"]


