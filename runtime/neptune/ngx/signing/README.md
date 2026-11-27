# Brinewell NGX code-signing identity

`brinewell-ngx-v1.pem` is the Brinewell self-signed code-signing certificate
(`O=Brinewell`, `CN=Brinewell NGX Code Signing v1`, `CA:FALSE`, key usage
`digitalSignature`, EKU `codeSigning`). It is the single public source of the
signing identity used for the opt-in NGX shims.

The matching private key is not part of any repository. It lives only on the
signing host (default `$HOME/.config/brinewell-signing/ngx-v1/`, overridable
with `BRINEWELL_NGX_SIGNING_DIR`).

## Scope

This key signs `nvngx.dll` only. It is not used for `nvapi64.dll`, for the
Wine drivers, or for any other component, and it is never installed as a
certificate authority.

## Rotation and missing-key behavior

Rotating the identity means generating a new key pair under a new signing
directory, replacing this PEM, and publishing a new payload; previously
trusted certificates in game prefixes are a separate lifecycle.

A checkout or build host without the private key still builds an unsigned
`nvngx.dll` with a warning; such a payload carries `mode: "unsigned"` in
`payload.json` and is never presented as signed.
