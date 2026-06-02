#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_DIR="$(cd "${ROOT_DIR}/.." && pwd)"

DOCKER_BIN="${DOCKER:-docker}"
DOCKER_SUDO="${DOCKER_SUDO:-sudo}"
DOCKER_IMAGE="${DOCKER_IMAGE:-retroarch188-psc-buildenv:stretch-armhf}"
DOCKERFILE_PATH="${DOCKERFILE_PATH:-${ROOT_DIR}/Dockerfile.buildenv}"
DOCKER_REBUILD_IMAGE="${DOCKER_REBUILD_IMAGE:-0}"
PACK_UPX="${PACK_UPX:-0}"

command -v "${DOCKER_BIN}" >/dev/null 2>&1 || {
  echo "Error: '${DOCKER_BIN}' not found in PATH."
  echo "Enable Docker in WSL or install docker.io first."
  exit 127
}

if [[ "${DOCKER_REBUILD_IMAGE}" == "1" ]] || ! ${DOCKER_SUDO} "${DOCKER_BIN}" image inspect "${DOCKER_IMAGE}" >/dev/null 2>&1; then
  echo "[docker] building image: ${DOCKER_IMAGE}"
  ${DOCKER_SUDO} "${DOCKER_BIN}" build -t "${DOCKER_IMAGE}" -f "${DOCKERFILE_PATH}" "${ROOT_DIR}"
fi

echo "[docker] building RetroArch inside ${DOCKER_IMAGE}"
${DOCKER_SUDO} "${DOCKER_BIN}" run --rm -it \
  -v "${WORKSPACE_DIR}:/work" \
  -w /work/RetroArch-1.8.8 \
  -e HOST_UID="$(id -u)" \
  -e HOST_GID="$(id -g)" \
  -e PACK_UPX="${PACK_UPX}" \
  "${DOCKER_IMAGE}" \
  bash -lc '
set -euo pipefail

make clean || true
rm -f config.mk config.h config.log

PKG_CONFIG="${PKG_CONFIG:-arm-linux-gnueabihf-pkg-config}" \
CFLAGS="-O3 -pipe -marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -fno-plt -fno-pie -fno-PIE" \
CXXFLAGS="-O3 -pipe -marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -fno-plt -fno-pie -fno-PIE" \
LDFLAGS="-Wl,-O1 -no-pie" \
./configure --host=arm-linux-gnueabihf \
  --enable-wayland --disable-x11 \
  --disable-kms --disable-plain_drm \
  --disable-mali_fbdev \
  --enable-opengl --disable-opengl1 --enable-opengl_core \
  --enable-opengles --enable-opengles3 --enable-egl \
  --enable-udev --enable-alsa --enable-sdl2 --disable-sdl \
  --enable-neon --enable-floathard \
  --enable-freetype \
  --disable-discord --disable-pulse \
  --disable-ffmpeg --disable-qt \
  --disable-online_updater --disable-update_cores --disable-update_assets \
  --disable-vulkan

grep -E "HAVE_WAYLAND|HAVE_KMS|HAVE_OPENGL_CORE|HAVE_MALI_FBDEV|HAVE_SDL2|HAVE_UDEV|HAVE_ALSA|HAVE_FREETYPE|HAVE_PULSE" config.mk

make HAVE_CLASSIC=1 HAVE_C_A7A7=1 HAVE_HAKCHI=1 -j"$(nproc)"
arm-linux-gnueabihf-strip -v retroarch

if [[ "${PACK_UPX:-0}" == "1" ]]; then
  if command -v upx >/dev/null 2>&1; then
    echo "[info] UPX version: $(upx --version | head -n 1)"
    if ! upx --best --lzma retroarch; then
      echo "[warn] UPX could not pack this RetroArch binary with the container UPX version."
      echo "[warn] Continuing with stripped (unpacked) binary."
    fi
  else
    echo "[warn] UPX not found in container; skipping pack step."
  fi
fi

chown "${HOST_UID:-0}:${HOST_GID:-0}" retroarch config.mk config.log 2>/dev/null || true
'

echo "[docker] done"
