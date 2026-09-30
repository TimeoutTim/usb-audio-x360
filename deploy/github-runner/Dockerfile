FROM ubuntu:24.04

ARG RUNNER_VERSION=2.336.0
ARG RUNNER_SHA256=04cf0be1aff4c3ec3554466c39124ca250e3effd8873bb7e8d68535aa9505d5d

ENV DEBIAN_FRONTEND=noninteractive
ENV DISPLAY=:99

RUN dpkg --add-architecture i386 \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
       ca-certificates \
       clang \
       coreutils \
       curl \
       git \
       jq \
       libclang-rt-18-dev \
       libicu74 \
       p7zip-full \
       python3 \
       tini \
       wine \
       wine32:i386 \
       wine64 \
       xvfb \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --create-home --uid 1001 runner \
    && install -d -o runner -g runner /actions-runner \
       /var/cache/usb-audio360 /opt/xbox360

WORKDIR /actions-runner

RUN curl --fail --location --retry 3 \
      --output actions-runner.tar.gz \
      "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz" \
    && echo "${RUNNER_SHA256}  actions-runner.tar.gz" | sha256sum --check --strict \
    && tar --extract --gzip --file actions-runner.tar.gz \
    && rm actions-runner.tar.gz \
    && ./bin/installdependencies.sh \
    && chown -R runner:runner /actions-runner

COPY --chown=runner:runner entrypoint.sh /usr/local/bin/runner-entrypoint
RUN chmod 0755 /usr/local/bin/runner-entrypoint

USER runner

ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/runner-entrypoint"]
