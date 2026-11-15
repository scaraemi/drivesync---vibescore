# scoreconvert: PDF / photos / audio / other notation formats -> .mscz + .mid
# Contents: Audiveris (OMR), MuseScore Studio (engraving, import, export), Basic Pitch (audio), FastAPI.
FROM python:3.11-slim-bookworm

# Download URLs; when empty they are looked up on GitHub (CI passes them, to avoid API rate limits).
ARG AUDIVERIS_VERSION=5.10.2
ARG AUDIVERIS_URL=
ARG MUSESCORE_VERSION=latest
ARG MUSESCORE_URL=

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl jq ffmpeg poppler-utils xz-utils \
        libgl1 libegl1 libopengl0 libglib2.0-0 libnss3 libxkbcommon0 libxkbcommon-x11-0 libfontconfig1 \
        libfreetype6 libasound2 libdbus-1-3 libxcb-cursor0 libxcb-icccm4 libxcb-keysyms1 libxcb-shape0 \
        libxcb-xinerama0 libxcb-randr0 libxcb-render-util0 libxcb-image0 libxrender1 libxi6 libsm6 \
        libpulse0 libjack-jackd2-0 fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/*

# Audiveris: the Linux installer bundles its own Java runtime.
RUN url="$AUDIVERIS_URL"; [ -n "$url" ] || url=$(curl -fsSL "https://api.github.com/repos/Audiveris/audiveris/releases/tags/${AUDIVERIS_VERSION}" \
        | jq -r '.assets[].browser_download_url' | grep -E 'ubuntu22.*x86_64\.deb$' | head -1) \
    && echo "Audiveris: $url" && curl -fsSL -o /tmp/audiveris.deb "$url" \
    && apt-get update && apt-get install -y --no-install-recommends /tmp/audiveris.deb \
    && rm -rf /tmp/audiveris.deb /var/lib/apt/lists/* \
    && ln -s "$(dpkg -L audiveris | grep -E '/bin/Audiveris$' | head -1)" /usr/local/bin/audiveris

# MuseScore Studio: the AppImage, extracted (no FUSE in containers).
RUN api=https://api.github.com/repos/musescore/MuseScore/releases/${MUSESCORE_VERSION}; \
    [ "$MUSESCORE_VERSION" = latest ] || api=https://api.github.com/repos/musescore/MuseScore/releases/tags/${MUSESCORE_VERSION}; \
    url="$MUSESCORE_URL"; [ -n "$url" ] || url=$(curl -fsSL "$api" | jq -r '.assets[].browser_download_url' | grep -E 'x86_64\.AppImage$' | head -1); \
    [ -n "$url" ] \
    && echo "MuseScore: $url" && curl -fsSL -o /tmp/ms.AppImage "$url" && chmod +x /tmp/ms.AppImage \
    && cd /opt && /tmp/ms.AppImage --appimage-extract >/dev/null && mv squashfs-root musescore && rm /tmp/ms.AppImage \
    && printf '#!/bin/sh\nexec /opt/musescore/AppRun "$@"\n' > /usr/local/bin/mscore && chmod +x /usr/local/bin/mscore

WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY scoreconvert scoreconvert
COPY tests tests

ENV MSCORE=mscore AUDIVERIS=audiveris QT_QPA_PLATFORM=offscreen TF_CPP_MIN_LOG_LEVEL=3 \
    SCORECONVERT_DATA=/data SCORECONVERT_WORKERS=1 PYTHONUNBUFFERED=1
VOLUME /data
EXPOSE 8080
HEALTHCHECK CMD curl -fs localhost:8080/healthz || exit 1
CMD ["uvicorn", "scoreconvert.server:app", "--host", "0.0.0.0", "--port", "8080"]
