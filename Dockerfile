FROM ubuntu:24.04 AS builder
RUN apt-get update && apt-get install -y \
    g++ make liburing-dev libssl-dev && \
    rm -rf /var/lib/apt/lists/*
COPY . /build
RUN cd /build && make WITH_TLS=1 -j$(nproc)

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y \
    liburing2t64 libssl3t64 && \
    rm -rf /var/lib/apt/lists/*
RUN useradd -r -s /bin/false xcdn
COPY --from=builder /build/xcdn /usr/local/bin/xcdn
COPY --from=builder /build/public /app/public
COPY --from=builder /build/config.yaml /app/config.yaml
RUN chown -R xcdn:xcdn /app
USER xcdn
WORKDIR /app
EXPOSE 8080 8443
ENTRYPOINT ["xcdn"]
CMD ["--config", "/app/config.yaml"]
