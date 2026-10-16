# Reproducible PSP build. pspdev pinned to the release in docs/m-1-toolchain.md
# (v20261001); the digest makes the pin immutable.
#
#   docker build --output build-ci-psp .
#
# leaves build-ci-psp/EBOOT.PBP on the host. See docs/m-1-toolchain.md.
FROM pspdev/pspdev:v20261001@sha256:54895e6f5afb71b8f4f6915ee6d5023e1e087ca7afdf2dcb1d7a694a5233e45f AS build
WORKDIR /src
COPY . .
RUN sh tools/build_psp.sh build-ci-psp

# Export stage: only the EBOOT, so --output copies nothing else.
FROM scratch
COPY --from=build /src/build-ci-psp/EBOOT.PBP /
