ARG PG_MAJOR=18
FROM pgxn/pgxn-tools:1.7.1

ARG PG_MAJOR
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    NO_CLUSTER=1 pg-start ${PG_MAJOR}

ENV PATH=/usr/lib/postgresql/${PG_MAJOR}/bin:${PATH}
