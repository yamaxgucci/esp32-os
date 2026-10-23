# Monocypher, in the kernel, for every application that has to check a
# signature - see Kconfig for why it is here and not in the image.
#
# The vendored sources are private to this component: what leaves it is
# argon/crypto.h, and that only says what the ABI table says.
idf_component_register(
    SRCS "src/crypto.c" "src/sha256.c"
         "vendor/monocypher.c" "vendor/monocypher-ed25519.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "vendor"
    REQUIRES argon_abi)
