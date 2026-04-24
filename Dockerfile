# Build FreeSWITCH from source with custom mod_audio_fork.
# Produces a minimal image with only the modules needed for WebSocket audio streaming.
#
# Usage:
#   docker build -t drellia/freeswitch-mrf .

# =============================================================================
# Stage 1: Build FreeSWITCH + libwebsockets + mod_audio_fork
# =============================================================================
FROM debian:bookworm-slim AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    git ca-certificates curl \
    build-essential cmake automake autoconf libtool libtool-bin pkg-config \
    libssl-dev libcurl4-openssl-dev libpcre3-dev \
    libspeex-dev libspeexdsp-dev \
    libedit-dev libsqlite3-dev libtiff-dev libjpeg-dev \
    libopus-dev libsndfile1-dev uuid-dev \
    yasm nasm \
    python3-dev \
    wget \
    && rm -rf /var/lib/apt/lists/*

# ---------------------------------------------------------------------------
# Build libwebsockets 4.3.3
# ---------------------------------------------------------------------------
RUN wget -qO /tmp/lws.tar.gz \
      https://github.com/warmcat/libwebsockets/archive/refs/tags/v4.3.3.tar.gz \
    && cd /tmp && tar xzf lws.tar.gz \
    && cd libwebsockets-4.3.3 && mkdir build && cd build \
    && cmake .. \
      -DLWS_WITHOUT_TESTAPPS=ON \
      -DLWS_WITHOUT_TEST_SERVER=ON \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
    && make -j"$(nproc)" && make install \
    && ldconfig \
    && rm -rf /tmp/lws.tar.gz /tmp/libwebsockets-4.3.3

# ---------------------------------------------------------------------------
# Build spandsp (FreeSWITCH needs it, not in Debian bookworm)
# ---------------------------------------------------------------------------
RUN cd /tmp \
    && git clone --depth 1 https://github.com/freeswitch/spandsp.git \
    && cd spandsp \
    && ./bootstrap.sh \
    && ./configure --prefix=/usr/local \
    && make -j"$(nproc)" && make install \
    && ldconfig \
    && rm -rf /tmp/spandsp

# ---------------------------------------------------------------------------
# Build sofia-sip (FreeSWITCH's SIP stack)
# ---------------------------------------------------------------------------
RUN cd /tmp \
    && git clone --depth 1 https://github.com/freeswitch/sofia-sip.git \
    && cd sofia-sip \
    && ./bootstrap.sh \
    && ./configure --prefix=/usr/local \
    && make -j"$(nproc)" && make install \
    && ldconfig \
    && rm -rf /tmp/sofia-sip

# ---------------------------------------------------------------------------
# Build FreeSWITCH 1.10 with minimal modules
# ---------------------------------------------------------------------------
RUN cd /usr/local/src \
    && git clone --depth 1 -b v1.10 https://github.com/signalwire/freeswitch.git

WORKDIR /usr/local/src/freeswitch

# Trim modules.conf to only the modules we need
RUN cp build/modules.conf.in build/modules.conf.in.orig \
    && cat > build/modules.conf.in <<'MODULES'
applications/mod_commands
applications/mod_dptools
dialplans/mod_dialplan_xml
endpoints/mod_loopback
endpoints/mod_sofia
event_handlers/mod_event_socket
formats/mod_native_file
formats/mod_sndfile
formats/mod_tone_stream
loggers/mod_console
loggers/mod_logfile
xml_int/mod_xml_curl
MODULES

RUN ./bootstrap.sh -j \
    && ./configure \
      --prefix=/usr/local/freeswitch \
      --enable-core-pgsql-support=no \
      --enable-core-odbc-support=no \
    && make -j"$(nproc)" \
    && make install \
    && rm -rf /usr/local/freeswitch/etc/freeswitch/autoload_configs \
    && rm -rf /usr/local/freeswitch/etc/freeswitch/dialplan \
    && rm -rf /usr/local/freeswitch/etc/freeswitch/sip_profiles \
    && rm -rf /usr/local/freeswitch/etc/freeswitch/directory \
    && mkdir -p /usr/local/freeswitch/etc/freeswitch/autoload_configs \
    && mkdir -p /usr/local/freeswitch/etc/freeswitch/dialplan \
    && mkdir -p /usr/local/freeswitch/etc/freeswitch/sip_profiles \
    && mkdir -p /usr/local/freeswitch/etc/freeswitch/directory \
    && mkdir -p /usr/local/freeswitch/log

# ---------------------------------------------------------------------------
# Build our custom mod_audio_fork
# ---------------------------------------------------------------------------
COPY modules/mod_audio_fork/ /build/mod_audio_fork/
WORKDIR /build/mod_audio_fork

RUN gcc -fPIC -c \
      -I/usr/local/freeswitch/include/freeswitch \
      $(pkg-config --cflags libwebsockets) \
      mod_audio_fork.c -o mod_audio_fork.o \
    && g++ -fPIC -std=c++11 -c \
      -I/usr/local/freeswitch/include/freeswitch \
      $(pkg-config --cflags libwebsockets) \
      lws_glue.cpp -o lws_glue.o \
    && g++ -shared -o mod_audio_fork.so mod_audio_fork.o lws_glue.o \
      $(pkg-config --libs libwebsockets) \
      -lspeexdsp

# =============================================================================
# Stage 2: Minimal runtime image
# =============================================================================
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

# Runtime dependencies only
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 libcurl4 libpcre3 \
    libspeex1 libspeexdsp1 \
    libedit2 libsqlite3-0 libtiff6 libjpeg62-turbo \
    libopus0 libsndfile1 libuuid1 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Copy FreeSWITCH installation
COPY --from=builder /usr/local/freeswitch/ /usr/local/freeswitch/

# Copy shared libraries built from source (libwebsockets, spandsp, sofia-sip)
COPY --from=builder /usr/local/lib/ /usr/local/lib/
RUN ldconfig

# Install our custom mod_audio_fork
COPY --from=builder /build/mod_audio_fork/mod_audio_fork.so \
     /usr/local/freeswitch/lib/freeswitch/mod/mod_audio_fork.so

# Add freeswitch to PATH
ENV PATH="/usr/local/freeswitch/bin:${PATH}"

# SIP, ESL, RTP range
EXPOSE 5060/udp 5060/tcp 8021/tcp 16384-32768/udp

CMD ["freeswitch", "-nonat", "-nf"]
