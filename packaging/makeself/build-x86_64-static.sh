#!/usr/bin/env bash

# SPDX-License-Identifier: GPL-3.0-or-later

# shellcheck source=./packaging/installer/functions.sh
. "$(dirname "$0")"/../installer/functions.sh || exit 1

if command -v podman >/dev/null 2>&1 ; then
  CONTAINER_RUNTIME="podman"
elif command -v wget >/dev/null 2>&1 ; then
  CONTAINER_RUNTIME="docker"
else
  echo >&2 "I need docker or podman to proceed, but neither is available on this system."
  exit 1
fi

set -e

DOCKER_CONTAINER_NAME="netdata-package-x86_64-static-alpine312"

if ! docker inspect "${DOCKER_CONTAINER_NAME}" > /dev/null 2>&1; then
  # To run interactively:
  #   docker run -it netdata-package-x86_64-static /bin/sh
  # (add -v host-dir:guest-dir:rw arguments to mount volumes)
  #
  # To remove images in order to re-create:
  #   docker rm -v $(sudo docker ps -a -q -f status=exited)
  #   docker rmi netdata-package-x86_64-static
  #
  # This command maps the current directory to
  #   /usr/src/netdata.git
  # inside the container and runs the script install-alpine-packages.sh
  # (also inside the container)
  #
  run "${CONTAINER_RUNTIME}" run -v "$(pwd)":/usr/src/netdata.git:rw alpine:3.12 \
    /bin/sh /usr/src/netdata.git/packaging/makeself/install-alpine-packages.sh

  # save the changes made permanently
  id=$("${CONTAINER_RUNTIME}" ps -l -q)
  run "${CONTAINER_RUNTIME}" commit "${id}" "${DOCKER_CONTAINER_NAME}"
fi

# Run the build script inside the container
if [ -t 1 ]; then
  run "${CONTAINER_RUNTIME}" run -a stdin -a stdout -a stderr -i -t -v \
    "$(pwd)":/usr/src/netdata.git:rw \
    "${DOCKER_CONTAINER_NAME}" \
    /bin/sh /usr/src/netdata.git/packaging/makeself/build.sh "${@}"
else
  run docker run -v "$(pwd)":/usr/src/netdata.git:rw \
    "${DOCKER_CONTAINER_NAME}" \
    /bin/sh /usr/src/netdata.git/packaging/makeself/build.sh "${@}"
fi

if [ "${USER}" ]; then
  sudo chown -R "${USER}" .
fi
