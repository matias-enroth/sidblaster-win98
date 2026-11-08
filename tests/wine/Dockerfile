FROM debian:bookworm-slim
RUN dpkg --add-architecture i386 && apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends wine wine32 python3 procps && \
    rm -rf /var/lib/apt/lists/*
ENV WINEDEBUG=-all WINEPREFIX=/root/.wine
RUN wineboot -i 2>/dev/null; wineserver -w; true
