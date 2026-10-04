# Monocypher 4.0.3, vendored unmodified -- see PROVENANCE.md. The core file plus the optional
# RFC 8032 Ed25519 (SHA-512) module, which is the variant the host tooling (ed25519_ref.py) speaks.
idf_component_register(
  SRCS "monocypher.c" "monocypher-ed25519.c"
  INCLUDE_DIRS "."
)
