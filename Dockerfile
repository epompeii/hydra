#syntax=docker/dockerfile:1

FROM postgres_base

# workaround for https://github.com/docker-library/postgres/issues/1112
RUN echo en_US.UTF-8 UTF-8 >> /etc/locale.gen

RUN set -eux; \
  apt-get update; \
  apt-get install -y --no-install-recommends \
  curl \
  ca-certificates \
  ; \
  rm -rf /var/lib/apt/lists/*

# columnar ext
# NOTE(owenthereal): build columnar with pgxman in this repo
COPY --from=columnar /pg_ext /

COPY files/postgres/docker-entrypoint-initdb.d /docker-entrypoint-initdb.d/

ARG POSTGRES_BASE_VERSION
# Install pgxman extensions
# Always force rebuild of this layer
ARG TIMESTAMP=1
COPY third-party/pgxman /tmp/pgxman/
RUN curl -sfL https://github.com/pgxman/release/releases/latest/download/install.sh | sh -s -- /tmp/pgxman/pgxman_${POSTGRES_BASE_VERSION}.yaml && \
  pgxman install pgsql-http=1.5.0 --pg ${POSTGRES_BASE_VERSION} --yes && \
  rm -rf /tmp/pgxman
