FROM gitpod/workspace-full

USER root

RUN apt-get update && apt-get install -y gcc-arm-none-eabi

ENV npm_config_user=root

# Install custom tools, runtimes, etc.
# For example "bastet", a command-line tetris clone:
# RUN brew install bastet
#
# More information: https://www.gitpod.io/docs/config-docker/
